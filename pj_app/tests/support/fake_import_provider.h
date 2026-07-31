#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Shared descriptor-import fake for the layout-import suites.
//
// Two layers:
//   * FakeJob + kFakeJobVtable — a PJ_joinable_job_t whose worker blocks on a
//     start gate as its FIRST action (so no callback can precede
//     start_import's return, the ABI contract), released by the test, by
//     cancel, or by destroy. Instrumentation is optional per-job hooks
//     (`on_event` for event logs, `unlink` for owner detach), so both the
//     scripted provider below and the headless-session test's
//     event-ordering fake ride the SAME job mechanics.
//   * The descriptor-SCRIPTED provider toolbox (registerStaticToolbox, no
//     DSO): the descriptor JSON drives it —
//       {"name":"s1","trust":"trusted|confirm|refused","path":"<effective>",
//        "estimated":123,"import":"eager|promoted|fail|block",
//        "materialized":bool,   // optional: overrides the cache verdict
//        "identity":"..."}      // optional: overrides the source identity
//     query_descriptor answers trust/path/identity/estimate + the
//     is_materialized verdict (defaults: identity = "fake:"+path,
//     materialized derived from the file's existence — override them to
//     script verdicts INDEPENDENT of the filesystem); start_import runs the
//     scripted outcome on a real worker thread through the C ABI.

#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QTimer>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <semaphore>
#include <string>
#include <thread>
#include <vector>

#include "pj_base/descriptor_import_protocol.h"
#include "pj_base/toolbox_protocol.h"

namespace pj_fake_import {

inline constexpr const char* kProviderId = "fake-import-provider";

[[nodiscard]] inline PJ_string_view_t sv(const char* text) {
  return PJ_string_view_t{text, std::strlen(text)};
}

// ---------------------------------------------------------------------------
// The job half.
// ---------------------------------------------------------------------------

struct FakeJob {
  std::thread worker;
  std::atomic<bool> cancelled{false};
  std::binary_semaphore start_gate{0};
  std::atomic<bool> start_released{false};
  // Optional instrumentation: `on_event` receives the vtable-side events
  // ("job.cancel", "job.join.returned", "job.destroy"); `unlink` detaches
  // the job from its owner's registry inside destroy, before deletion.
  std::function<void(const char*)> on_event;
  std::function<void(FakeJob*)> unlink;

  // At-most-once release: binary_semaphore::release() on an already-released
  // semaphore is UB, and the gate can be released from the test, cancel, or
  // destroy in any combination.
  void releaseStartOnce() {
    bool expected = false;
    if (start_released.compare_exchange_strong(expected, true)) {
      start_gate.release();
    }
  }

  void event(const char* name) const {
    if (on_event) {
      on_event(name);
    }
  }
};

inline void fakeJobCancel(void* ctx) noexcept {
  auto* job = static_cast<FakeJob*>(ctx);
  job->event("job.cancel");
  job->cancelled.store(true);
  job->releaseStartOnce();  // a blocked worker must observe the cancel
}

inline void fakeJobJoin(void* ctx) noexcept {
  auto* job = static_cast<FakeJob*>(ctx);
  if (job->worker.joinable()) {
    job->worker.join();
  }
  job->event("job.join.returned");
}

inline void fakeJobDestroy(void* ctx) noexcept {
  auto* job = static_cast<FakeJob*>(ctx);
  // Destroy cancels and joins when necessary (the ABI contract) — without
  // re-emitting cancel/join events, so an event log keeps one entry per
  // caller-side call.
  job->cancelled.store(true);
  job->releaseStartOnce();
  if (job->worker.joinable()) {
    job->worker.join();
  }
  job->event("job.destroy");
  if (job->unlink) {
    job->unlink(job);
  }
  delete job;
}

inline constexpr PJ_joinable_job_vtable_t kFakeJobVtable{
    sizeof(PJ_joinable_job_vtable_t), 0, &fakeJobCancel, &fakeJobJoin, &fakeJobDestroy};

// ---------------------------------------------------------------------------
// The scripted provider half.
// ---------------------------------------------------------------------------

struct FakeLog {
  std::mutex mu;
  std::vector<std::string> started;            // import start order, by "name"
  std::vector<std::uint64_t> start_max_bytes;  // request->max_transfer_bytes per start
  void addStart(std::string name, std::uint64_t max_bytes) {
    const std::lock_guard<std::mutex> lock(mu);
    started.push_back(std::move(name));
    start_max_bytes.push_back(max_bytes);
  }
  [[nodiscard]] std::vector<std::string> snapshot() {
    const std::lock_guard<std::mutex> lock(mu);
    return started;
  }
  [[nodiscard]] std::vector<std::uint64_t> maxBytes() {
    const std::lock_guard<std::mutex> lock(mu);
    return start_max_bytes;
  }
  void clear() {
    const std::lock_guard<std::mutex> lock(mu);
    started.clear();
    start_max_bytes.clear();
  }
};

inline FakeLog g_log;
inline std::atomic<std::uint32_t> g_next_dataset_id{101};

struct FakeProviderInstance;
inline FakeProviderInstance* g_instance = nullptr;

struct FakeProviderInstance {
  PJ_descriptor_import_provider_v1_t ext{};
  // Borrowed-view backing storage for query_descriptor results (valid until
  // the next query on this instance — the ABI's lifetime contract).
  std::string query_identity;
  std::string query_path;
  // Covers last_job lookup+use AND the destroy-side unlink, so a
  // releaseStart can never race the pointee's deletion.
  std::mutex job_mu;
  FakeJob* last_job = nullptr;  // non-owning; owned by the returned out_job->ctx

  void releaseStart() {
    const std::lock_guard<std::mutex> lock(job_mu);
    if (last_job != nullptr) {
      last_job->releaseStartOnce();
    }
  }
};

[[nodiscard]] inline QJsonObject parseDescriptor(PJ_string_view_t descriptor_json) {
  const QByteArray bytes(descriptor_json.data, static_cast<qsizetype>(descriptor_json.size));
  return QJsonDocument::fromJson(bytes).object();
}

[[nodiscard]] inline QString makeDescriptor(
    const QString& name, const QString& trust, const QString& path, std::uint64_t estimated = 0,
    const QString& import_mode = QStringLiteral("eager")) {
  QJsonObject obj;
  obj.insert(QStringLiteral("name"), name);
  obj.insert(QStringLiteral("trust"), trust);
  obj.insert(QStringLiteral("path"), path);
  obj.insert(QStringLiteral("estimated"), static_cast<double>(estimated));
  obj.insert(QStringLiteral("import"), import_mode);
  return QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

inline bool fakeQueryDescriptor(
    void* plugin_ctx, PJ_string_view_t descriptor_json, PJ_descriptor_query_result_v1_t* out,
    PJ_error_t* /*out_error*/) noexcept {
  auto* self = static_cast<FakeProviderInstance*>(plugin_ctx);
  const QJsonObject scripted = parseDescriptor(descriptor_json);
  const QString trust = scripted.value(QStringLiteral("trust")).toString(QStringLiteral("trusted"));
  if (trust == QStringLiteral("refused")) {
    out->trust = PJ_DESCRIPTOR_TRUST_REFUSED;
  } else if (trust == QStringLiteral("confirm")) {
    out->trust = PJ_DESCRIPTOR_TRUST_NEEDS_CONFIRMATION;
  } else {
    out->trust = PJ_DESCRIPTOR_TRUST_TRUSTED;
  }
  self->query_path = scripted.value(QStringLiteral("path")).toString().toStdString();
  self->query_identity = scripted.contains(QStringLiteral("identity"))
                             ? scripted.value(QStringLiteral("identity")).toString().toStdString()
                             : "fake:" + self->query_path;
  const QJsonValue materialized = scripted.value(QStringLiteral("materialized"));
  out->is_materialized = materialized.isBool() ? (materialized.toBool() ? 1 : 0)
                                               : (QFileInfo::exists(QString::fromStdString(self->query_path)) ? 1 : 0);
  out->source_identity = PJ_string_view_t{self->query_identity.data(), self->query_identity.size()};
  out->local_path_utf8 = PJ_string_view_t{self->query_path.data(), self->query_path.size()};
  out->estimated_bytes = static_cast<uint64_t>(scripted.value(QStringLiteral("estimated")).toDouble(0));
  return true;
}

inline bool fakeStartImport(
    void* plugin_ctx, const PJ_descriptor_import_start_request_v1_t* request,
    const PJ_descriptor_import_callbacks_v1_t* callbacks, void* callback_ctx, PJ_joinable_job_t* out_job,
    PJ_error_t* /*out_error*/) noexcept {
  auto* self = static_cast<FakeProviderInstance*>(plugin_ctx);
  if (request == nullptr || callbacks == nullptr || callbacks->on_terminal == nullptr) {
    return false;
  }
  const QJsonObject scripted = parseDescriptor(request->descriptor_json);
  const std::string name = scripted.value(QStringLiteral("name")).toString().toStdString();
  const QString mode = scripted.value(QStringLiteral("import")).toString(QStringLiteral("eager"));
  g_log.addStart(name, request->max_transfer_bytes);

  auto on_dataset = callbacks->on_dataset;
  auto on_terminal = callbacks->on_terminal;
  auto* job = new FakeJob();
  job->unlink = [self](FakeJob* dying) {
    const std::lock_guard<std::mutex> lock(self->job_mu);
    if (self->last_job == dying) {
      self->last_job = nullptr;
    }
  };
  // Fill out_job BEFORE spawning the worker: no job callback may occur
  // before start_import returns, and the worker must not race the caller's
  // read of *out_job.
  out_job->ctx = job;
  out_job->vtable = &kFakeJobVtable;
  {
    const std::lock_guard<std::mutex> lock(self->job_mu);
    self->last_job = job;
  }
  const bool fail = mode == QStringLiteral("fail");
  const bool promoted = mode == QStringLiteral("promoted");
  job->worker = std::thread([job, on_dataset, on_terminal, callback_ctx, fail, promoted] {
    // Every worker blocks on its start gate FIRST, so no job callback can
    // precede start_import's return (the ABI contract). Non-"block" modes
    // are auto-released by the queued 0-timer below.
    job->start_gate.acquire();
    if (job->cancelled.load()) {
      on_terminal(callback_ctx, PJ_DESCRIPTOR_IMPORT_CANCELLED, sv("cancelled"));
      return;
    }
    if (fail) {
      on_terminal(callback_ctx, PJ_DESCRIPTOR_IMPORT_FAILED, sv("scripted import failure"));
      return;
    }
    if (on_dataset != nullptr) {
      on_dataset(callback_ctx, PJ_data_source_handle_t{g_next_dataset_id.fetch_add(1)});
    }
    on_terminal(
        callback_ctx, promoted ? PJ_DESCRIPTOR_IMPORT_SUCCEEDED_PROMOTED : PJ_DESCRIPTOR_IMPORT_SUCCEEDED_EAGER_ONLY,
        sv("done"));
  });
  if (mode != QStringLiteral("block")) {
    // start_import runs on the GUI thread: a queued 0-timer releases the
    // gate strictly after this call returned to the event loop, keeping the
    // "no callback before start_import returns" contract without manual
    // test intervention. `self` may be destroyed before the timer fires;
    // the g_instance guard makes the release a no-op then.
    QTimer::singleShot(0, [self]() {
      if (g_instance == self) {
        self->releaseStart();
      }
    });
  }
  return true;
}

inline void* fakeCreate() noexcept {
  auto* instance = new FakeProviderInstance();
  instance->ext.struct_size = sizeof(PJ_descriptor_import_provider_v1_t);
  instance->ext.query_descriptor = &fakeQueryDescriptor;
  instance->ext.start_import = &fakeStartImport;
  g_instance = instance;
  return instance;
}

inline void fakeDestroyToolbox(void* ctx) noexcept {
  if (g_instance == ctx) {
    g_instance = nullptr;
  }
  delete static_cast<FakeProviderInstance*>(ctx);
}

inline uint64_t fakeCapabilities(void* /*ctx*/) noexcept {
  return 0;
}

inline bool fakeBind(void* /*ctx*/, PJ_service_registry_t /*registry*/, PJ_error_t* /*out_error*/) noexcept {
  return true;
}

inline bool fakeSaveConfig(void* /*ctx*/, PJ_string_view_t* out_json, PJ_error_t* /*out_error*/) noexcept {
  if (out_json != nullptr) {
    out_json->data = "{}";
    out_json->size = 2;
  }
  return true;
}

inline bool fakeLoadConfig(void* /*ctx*/, PJ_string_view_t /*config_json*/, PJ_error_t* /*out_error*/) noexcept {
  return true;
}

inline PJ_borrowed_dialog_t fakeDialog(void* /*ctx*/) noexcept {
  return PJ_borrowed_dialog_t{nullptr, nullptr};
}

inline void fakeOnDataChanged(void* /*ctx*/) noexcept {}

inline const void* fakeGetExtension(void* ctx, PJ_string_view_t id) noexcept {
  auto* self = static_cast<FakeProviderInstance*>(ctx);
  const std::string requested(id.data == nullptr ? "" : id.data, id.size);
  return requested == PJ_DESCRIPTOR_IMPORT_EXTENSION_V1 ? &self->ext : nullptr;
}

inline const PJ_toolbox_vtable_t kFakeVtable{
    PJ_TOOLBOX_PLUGIN_PROTOCOL_VERSION,
    sizeof(PJ_toolbox_vtable_t),
    &fakeCreate,
    &fakeDestroyToolbox,
    R"({"id":"fake-import-provider","name":"Fake Import Provider","version":"1.0.0"})",
    &fakeCapabilities,
    &fakeBind,
    &fakeSaveConfig,
    &fakeLoadConfig,
    &fakeDialog,
    &fakeOnDataChanged,
    &fakeGetExtension,
};

}  // namespace pj_fake_import
