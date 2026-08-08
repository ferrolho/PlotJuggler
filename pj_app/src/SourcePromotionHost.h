#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QObject>
#include <QPointer>
#include <QString>
#include <QVector>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "FileLoader.h"
#include "pj_base/descriptor_import_protocol.h"
#include "pj_base/expected.hpp"
#include "pj_base/types.hpp"

namespace PJ {

class ServiceRegistryBuilder;
class SessionManager;

// Host side of the SDK service "pj.source_promotion.v1"
// (PJ_source_promotion_host_vtable_t): lets a descriptor-import provider hand
// its materialized artifact to the host to be promoted to a stock file-backed
// source. Registered PER TOOLBOX INSTANCE (MainWindow::launchToolbox), so the
// provider's identity is HOST-DERIVED from the binding (`provider_id`) — the
// plugin never supplies its own identity and cannot spoof another provider's.
//
// One accepted promotion drives the stock replace-dataset pipeline:
//   - validate the target dataset BELONGS TO THE BOUND PROVIDER'S INGEST
//     (`owns_dataset`, the ToolboxRuntimeHost::hasIngestForDataset authority;
//     liveness is FileLoader's strict-replacement check — a vanished target
//     fails the load, it never degrades to a fresh load);
//   - FileLoader::loadFileTicketed with a preset config, DialogPolicy::kNever
//     (no dialogs, preset authoritative) and require_replacement;
//   - on the SYNCHRONOUS pre-catalog loadCommitting seam, attach the
//     SessionManager SourceRecord {provider_id, source_identity,
//     descriptor_json} — atomically with catalog publication;
//   - report the transaction outcome through result_cb EXACTLY ONCE per
//     ACCEPTED call. promote_to_file_source returning false is a synchronous
//     reject: result_cb never runs. Accepted-then-invalid (not owned, vanished
//     target, unknown loader plugin, rolled-back load, teardown) is ok=false —
//     the ABI's accepted-vs-succeeded distinction. EAGER_ONLY semantics stay
//     plugin-side; this host only reports transaction ok/failed.
//
// Threading: promote_to_file_source is [thread-safe, asynchronous] — callable
// from any plugin thread. The thunk copies the full request into owned
// storage, does only structural validation synchronously, and queues the job
// onto this QObject's (GUI) thread; all FileLoader/SessionManager work happens
// there. result_cb fires on the GUI thread — serialized per request, may
// re-enter promote_to_file_source.
//
// Teardown: the destructor fails (ok=false) every accepted-but-unfinished
// promotion while the plugin instance/DSO is still alive, and stops observing
// the loader. It deliberately does NOT cancel an in-flight replacement load —
// the load completes as an ordinary strict replace without a record attach;
// only the promotion transaction is reported failed.
//
// Not movable: the service fat pointer stores `this`.
class SourcePromotionHost : public QObject {
  Q_OBJECT
 public:
  // `provider_id` is the plugin's stable manifest id from the host-side
  // catalog binding. `owns_dataset` answers "did this plugin instance's ingest
  // produce the dataset" (null => owns nothing, every promotion is failed).
  SourcePromotionHost(
      FileLoader& loader, SessionManager& session, QString provider_id, std::function<bool(DatasetId)> owns_dataset,
      QObject* parent = nullptr);
  ~SourcePromotionHost() override;

  // Phase 1 of the two-phase teardown (idempotent; the destructor calls it):
  // stops observing the loader, closes the intake (every later
  // promote_to_file_source sync-rejects against a LIVE mutex), and fails
  // every accepted-but-unfinished promotion exactly once — which reaches
  // plugin code, so it MUST run while the plugin instance/DSO is alive.
  // The object must then STAY alive until the plugin's workers are joined
  // (instance destroy): a straggler worker calling the raw service pointer
  // in that window hits the safe synchronous rejection instead of a
  // use-after-free. Only afterwards may the host itself be destroyed.
  void shutdown();

  SourcePromotionHost(const SourcePromotionHost&) = delete;
  SourcePromotionHost& operator=(const SourcePromotionHost&) = delete;

  // Register "pj.source_promotion.v1" into the builder used to bind the
  // toolbox plugin (before the handle's bind()). It is this host's only service
  // and promotion is its entire purpose, so a rejection fails the Status; do not
  // bind the plugin on a failure, or promotion requests would reach a host the
  // caller never published.
  [[nodiscard]] Status registerServices(ServiceRegistryBuilder& registry);

  // The raw C-ABI fat pointer (for direct wiring / tests).
  [[nodiscard]] PJ_source_promotion_host_t raw() const noexcept {
    return raw_;
  }

 private:
  // One ACCEPTED promotion. The identity/request fields are immutable after
  // construction; `fired` is guarded by mu_ (the exactly-once gate shared with
  // the destructor sweep); failure_reason/record_attached are GUI-thread only.
  // The load ticket lives only in by_ticket_ (the sole ticket authority).
  struct Promotion {
    quint64 id = 0;
    DatasetId dataset = 0;
    QString source_identity;
    QString local_path;
    QString loader_plugin_id;
    QString loader_config_json;
    QString descriptor_json;
    PJ_source_promotion_result_fn result_cb = nullptr;
    void* callback_ctx = nullptr;
    bool fired = false;
    QString failure_reason;
    bool record_attached = false;
  };

  static bool onPromote(
      void* ctx, const PJ_source_promotion_request_v1_t* request, PJ_source_promotion_result_fn result_cb,
      void* callback_ctx, PJ_error_t* out_error) noexcept;

  // GUI thread: validate ownership and drive the replacement load.
  void processPromotion(quint64 promotion_id);
  // GUI thread: deliver the result exactly once (no-op when already fired).
  void finishPromotion(const std::shared_ptr<Promotion>& promotion, bool ok, const QString& message);

  QPointer<FileLoader> loader_;
  SessionManager& session_;
  const QString provider_id_;
  const std::function<bool(DatasetId)> owns_dataset_;

  // Guards pending_/next_promotion_id_/shutting_down_ and every Promotion's
  // `fired` flag — the only state the any-thread entry point touches.
  std::mutex mu_;
  bool shutting_down_ = false;
  quint64 next_promotion_id_ = 0;
  std::unordered_map<quint64, std::shared_ptr<Promotion>> pending_;

  // GUI-thread only: promotions whose replacement load is in flight, keyed by
  // the load ticket; and the promotion currently inside loadFileTicketed (its
  // synchronous-failure fileLoadFailed fires before the ticket is known).
  std::unordered_map<quint64, std::shared_ptr<Promotion>> by_ticket_;
  std::shared_ptr<Promotion> issuing_;

  PJ_source_promotion_host_vtable_t vtable_;
  PJ_source_promotion_host_t raw_;
};

}  // namespace PJ
