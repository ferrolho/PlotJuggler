// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_runtime/ToolboxRuntimeHost.h"

#include <QMetaObject>
#include <algorithm>
#include <chrono>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "pj_base/sdk/plugin_data_api.hpp"
#include "pj_base/sdk/service_traits.hpp"
#include "pj_base/sdk/toolbox_plugin_base.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_plugins/host/message_parser_handle.hpp"
#include "pj_plugins/host/service_registry_builder.hpp"
#include "pj_runtime/DataSourceRuntimeHost.h"
#include "pj_runtime/ExtensionCatalogService.h"

namespace PJ {

ToolboxRuntimeHost::ToolboxRuntimeHost(
    DataEngine& engine, ObjectStore& object_store, sdk::SettingsBackend& settings, Callbacks callbacks)
    : ToolboxRuntimeHost(engine, object_store, settings, std::move(callbacks), ParserIngestDeps{}) {}

ToolboxRuntimeHost::ToolboxRuntimeHost(
    DataEngine& engine, ObjectStore& object_store, sdk::SettingsBackend& settings, Callbacks callbacks,
    ParserIngestDeps parser_ingest)
    : write_host_(engine, object_store),
      settings_host_(settings),
      callbacks_(std::move(callbacks)),
      engine_(engine),
      object_store_(object_store),
      parser_ingest_deps_(std::move(parser_ingest)),
      runtime_vtable_{
          PJ_TOOLBOX_PLUGIN_PROTOCOL_VERSION,        sizeof(PJ_toolbox_runtime_host_vtable_t),
          &ToolboxRuntimeHost::onReportMessage,      &ToolboxRuntimeHost::onNotifyDataChanged,
          &ToolboxRuntimeHost::onCreateParserIngest, &ToolboxRuntimeHost::onReleaseParserIngest,
      },
      runtime_{this, &runtime_vtable_} {}

ToolboxRuntimeHost::~ToolboxRuntimeHost() {
  // Flush anything a toolbox left unreleased so rows aren't lost on teardown.
  // Mirror the release path: take the contexts out under the lock, run plugin
  // parser destructors and engine flushes outside it.
  std::unordered_map<uint32_t, std::unique_ptr<DataSourceRuntimeHost>> contexts;
  std::unordered_map<uint32_t, std::shared_ptr<IngestProgress>> progress;
  {
    std::lock_guard lock(parser_ingest_mu_);
    contexts.swap(parser_ingests_);
    progress.swap(ingest_progress_);
  }
  // Close any import the plugin never finished: the shell's started/finished
  // callbacks must stay paired or its progress UI wedges on a mid-import panel
  // close. Direct call — teardown runs on the construction thread (the same
  // contract every marshalled callback already assumes), and a queued metacall
  // would be purged with marshaller_ before delivery.
  for (auto& [id, state] : progress) {
    if (state->active.exchange(false) && callbacks_.on_ingest_finished) {
      callbacks_.on_ingest_finished(static_cast<DatasetId>(id));
    }
  }
  for (auto& [id, host] : contexts) {
    host->flushAll();
  }
}

void ToolboxRuntimeHost::requestStopActiveIngests() {
  std::lock_guard lock(parser_ingest_mu_);
  for (auto& [id, host] : parser_ingests_) {
    // Flag-only overload: safe from this (GUI) thread against a worker that
    // may be inside fail()/last_error_ right now.
    host->requestStop();
  }
}

void ToolboxRuntimeHost::registerServices(ServiceRegistryBuilder& registry) {
  registry.registerService<sdk::ToolboxHostService>(write_host_.raw());
  registry.registerService<sdk::ToolboxRuntimeHostService>(runtime_);
  registry.registerService<sdk::SettingsStoreService>(settings_host_.view());
}

void ToolboxRuntimeHost::onReportMessage(
    void* ctx, PJ_toolbox_message_level_t level, PJ_string_view_t message) noexcept {
  auto* self = static_cast<ToolboxRuntimeHost*>(ctx);
  if (self == nullptr || !self->callbacks_.on_message) {
    return;
  }
  try {
    std::string text = message.data != nullptr ? std::string(message.data, message.size) : std::string();
    // Marshal to the host thread (AutoConnection: direct when already there,
    // queued from a worker thread) so on_message can touch Qt UI safely.
    QMetaObject::invokeMethod(
        &self->marshaller_,
        [self, level, text = std::move(text)]() mutable {
          if (self->callbacks_.on_message) {
            self->callbacks_.on_message(level, std::move(text));
          }
        },
        Qt::AutoConnection);
  } catch (...) {
    // noexcept C-ABI boundary: never let an exception escape into the plugin.
  }
}

void ToolboxRuntimeHost::onNotifyDataChanged(void* ctx) noexcept {
  auto* self = static_cast<ToolboxRuntimeHost*>(ctx);
  if (self == nullptr) {
    return;
  }
  try {
    // Marshal to the host thread (AutoConnection: direct when already there,
    // queued from a worker thread). Seal buffered writes so the freshly written
    // rows/objects are visible before the host rebuilds its catalog — flush and
    // rebuild then always run together on the GUI thread.
    QMetaObject::invokeMethod(
        &self->marshaller_,
        [self]() {
          try {
            self->write_host_.flushPending();
            // Report the bulk-import datasets: created/released contexts are
            // drained per notify, while a MID-IMPORT context (active progress
            // sequence) re-reports on EVERY notify so the shell's playback
            // focus follows the growing import instead of collapsing back to
            // the union between ticks. Composed here (GUI thread) so the set
            // and the callback observing it stay ordered.
            std::vector<DatasetId> ingested;
            {
              std::lock_guard lock(self->parser_ingest_mu_);
              ingested.swap(self->pending_ingest_datasets_);
              for (const auto& [id, progress] : self->ingest_progress_) {
                const auto ds_id = static_cast<DatasetId>(id);
                if (progress->active.load() && std::find(ingested.begin(), ingested.end(), ds_id) == ingested.end()) {
                  ingested.push_back(ds_id);
                }
              }
            }
            if (self->callbacks_.on_data_changed) {
              self->callbacks_.on_data_changed(std::move(ingested));
            }
          } catch (...) {}
        },
        Qt::AutoConnection);
  } catch (...) {
    // noexcept C-ABI boundary.
  }
}

namespace {
// Same error taxonomy as DataSourceRuntimeHost::fail (the sibling ingest
// surface), under this host's own domain.
bool parserIngestFail(PJ_error_t* out_error, const char* msg) noexcept {
  sdk::fillError(out_error, 1, "pj.runtime.toolbox_ingest", msg);
  return false;
}
}  // namespace

bool ToolboxRuntimeHost::onCreateParserIngest(
    void* ctx, uint32_t data_source_id, PJ_data_source_runtime_host_t* out_host, PJ_error_t* out_error) noexcept {
  auto* self = static_cast<ToolboxRuntimeHost*>(ctx);
  if (self == nullptr || out_host == nullptr) {
    return parserIngestFail(out_error, "invalid arguments");
  }
  try {
    if (self->parser_ingest_deps_.catalog == nullptr) {
      return parserIngestFail(out_error, "parser ingest is not configured on this host");
    }
    // Same validation the toolbox write host applies to object topics
    // (plugin_data_host.cpp toolboxRegisterObjectTopic): the id must be a
    // live dataset.
    if (self->engine_.getDataset(static_cast<DatasetId>(data_source_id)) == nullptr) {
      return parserIngestFail(out_error, "data source not found — call createDataSource first");
    }
    std::lock_guard lock(self->parser_ingest_mu_);
    // Construct first, insert only on success: a throwing constructor must not
    // leave a null entry in the map (the release path dereferences entries).
    auto it = self->parser_ingests_.find(data_source_id);
    if (it == self->parser_ingests_.end()) {
      auto host = std::make_unique<DataSourceRuntimeHost>(
          self->engine_, *self->parser_ingest_deps_.catalog, static_cast<DatasetId>(data_source_id),
          PJ_data_source_handle_t{data_source_id}, self->object_store_,
          "toolbox-ingest-" + std::to_string(data_source_id), self->parser_ingest_deps_.register_object_parser,
          // No secondary store/engine (toolbox ingest writes straight into the
          // primary), and no plugin-DSO keepalive — the toolbox isn't a
          // DataSource plugin; its render parsers come from the catalog, which
          // owns their .so lifetime and outlives every ingest context.
          /*secondary_object_store=*/nullptr, /*secondary_data_engine=*/nullptr,
          /*library_keepalive=*/std::shared_ptr<void>{});
      it = self->parser_ingests_.emplace(data_source_id, std::move(host)).first;

      // Progress hooks: the plugin's progress_start/update/finish calls on the
      // fat pointer drive the shell's progressive-import surface. They run on
      // the plugin's ingest thread — flushes are engine-lock-safe there, and
      // UI-facing callbacks marshal through marshaller_ like every other
      // callback. `raw` is the context that invokes the hook, so it outlives
      // every invocation; `self` outlives the plugin per the teardown contract.
      DataSourceRuntimeHost* raw = it->second.get();
      auto progress = std::make_shared<IngestProgress>();
      self->ingest_progress_[data_source_id] = progress;
      const auto hook_ds = static_cast<DatasetId>(data_source_id);
      raw->on_progress_start = [self, hook_ds, progress](std::string_view label, uint64_t total, bool /*cancellable*/) {
        try {
          progress->total.store(total);
          progress->active.store(true);
          QMetaObject::invokeMethod(
              &self->marshaller_,
              [self, hook_ds, label = std::string(label), total]() {
                if (self->callbacks_.on_ingest_started) {
                  self->callbacks_.on_ingest_started(hook_ds, label, total);
                }
              },
              Qt::AutoConnection);
        } catch (...) {}
      };
      raw->on_progress_update = [self, hook_ds, progress, raw](uint64_t current) -> bool {
        try {
          const auto now = std::chrono::steady_clock::now();
          if (now - progress->last_flush < std::chrono::milliseconds(self->flush_throttle_ms_)) {
            return true;
          }
          progress->last_flush = now;
          // Seal both write surfaces so the marshalled on_ingest_progress can
          // treat "this tick" as "these rows are reader-visible": the context's
          // own parser-binding writers AND the toolbox write host the plugin
          // may be appending through (the cloud connector's Arrow/object path).
          raw->flushPending();
          self->write_host_.flushPending();
          QMetaObject::invokeMethod(
              &self->marshaller_,
              [self, hook_ds, current, total = progress->total.load()]() {
                if (self->callbacks_.on_ingest_progress) {
                  self->callbacks_.on_ingest_progress(hook_ds, current, total);
                }
              },
              Qt::AutoConnection);
        } catch (...) {}
        return true;  // host-side stop travels via stop_requested_, checked before this hook
      };
      raw->on_progress_finish = [self, hook_ds, progress]() {
        try {
          // Gate on an ACTIVE sequence: the SDK's finite-import pattern calls
          // progressFinish() as a safe no-op even when no sequence started, and
          // an unpaired finished callback would disturb the shell's pairing
          // bookkeeping for some other import.
          if (!progress->active.exchange(false)) {
            return;
          }
          QMetaObject::invokeMethod(
              &self->marshaller_,
              [self, hook_ds]() {
                if (self->callbacks_.on_ingest_finished) {
                  self->callbacks_.on_ingest_finished(hook_ds);
                }
              },
              Qt::AutoConnection);
        } catch (...) {}
      };
    }
    // Remember the dataset for the next notify_data_changed: an ingest context
    // marks a bulk import the host will want to focus playback on.
    const auto ds_id = static_cast<DatasetId>(data_source_id);
    auto& pending = self->pending_ingest_datasets_;
    if (std::find(pending.begin(), pending.end(), ds_id) == pending.end()) {
      pending.push_back(ds_id);
    }
    *out_host = it->second->hostHandle();  // out_host written ONLY on success (documented contract)
    return true;
  } catch (const std::exception& e) {
    return parserIngestFail(out_error, e.what());
  } catch (...) {
    return parserIngestFail(out_error, "unknown exception in create_parser_ingest");
  }
}

bool ToolboxRuntimeHost::onReleaseParserIngest(void* ctx, uint32_t data_source_id, PJ_error_t* out_error) noexcept {
  auto* self = static_cast<ToolboxRuntimeHost*>(ctx);
  if (self == nullptr) {
    return parserIngestFail(out_error, "invalid arguments");
  }
  try {
    std::unique_ptr<DataSourceRuntimeHost> victim;
    std::shared_ptr<IngestProgress> progress;
    {
      std::lock_guard lock(self->parser_ingest_mu_);
      auto it = self->parser_ingests_.find(data_source_id);
      if (it == self->parser_ingests_.end()) {
        return true;  // idempotent
      }
      victim = std::move(it->second);
      self->parser_ingests_.erase(it);
      if (auto pit = self->ingest_progress_.find(data_source_id); pit != self->ingest_progress_.end()) {
        progress = std::move(pit->second);
        self->ingest_progress_.erase(pit);
      }
    }
    // Seal rows BEFORE destruction so the next notify_data_changed/catalog
    // rebuild sees everything the parsers wrote.
    victim->flushAll();
    victim.reset();
    {
      // A released context marks a COMPLETED bulk import: report it on the
      // next notify so the shell runs its terminal focus/reconcile pass over
      // the finished dataset (the create-time report was drained long ago on
      // a progressive import). Published only AFTER the terminal flush above —
      // release and notify are both [thread-safe], so a concurrent notifier
      // must not drain this marker while the parser writers are half-flushed.
      std::lock_guard lock(self->parser_ingest_mu_);
      const auto ds_id = static_cast<DatasetId>(data_source_id);
      auto& pending = self->pending_ingest_datasets_;
      if (std::find(pending.begin(), pending.end(), ds_id) == pending.end()) {
        pending.push_back(ds_id);
      }
    }
    // Releasing without progress_finish must still pair the shell's
    // started/finished callbacks, or its progress UI never hides. Marshalled:
    // release is a [thread-safe] slot.
    if (progress != nullptr && progress->active.exchange(false)) {
      const auto ds_id = static_cast<DatasetId>(data_source_id);
      QMetaObject::invokeMethod(
          &self->marshaller_,
          [self, ds_id]() {
            if (self->callbacks_.on_ingest_finished) {
              self->callbacks_.on_ingest_finished(ds_id);
            }
          },
          Qt::AutoConnection);
    }
    return true;
  } catch (const std::exception& e) {
    return parserIngestFail(out_error, e.what());
  } catch (...) {
    return parserIngestFail(out_error, "unknown exception in release_parser_ingest");
  }
}

}  // namespace PJ
