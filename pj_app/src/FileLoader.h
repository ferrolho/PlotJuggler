#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>

#include "LoadInput.h"
#include "pj_base/types.hpp"

QT_BEGIN_NAMESPACE
class QWidget;
class QThread;
class QDialog;
QT_END_NAMESPACE

namespace pj::scene3d {
class TransformService;
}  // namespace pj::scene3d

namespace PJ::sdk {
class ObjectIngestPolicyResolver;
}  // namespace PJ::sdk

namespace PJ {

class CatalogModel;
class BrowserFileStore;
class DataSourceRuntimeHost;
class ExtensionCatalogService;
class SessionManager;

// Worker↔GUI rendezvous for the synchronous plugin message-box ABI (defined in
// FileLoader.cpp). Heap-shared so the queued GUI lambda and a blocked worker
// can safely outlive both a shutdown and the FileLoader itself.
struct PluginMessageGate;

// Hints supplied by callers that already know what plugin to use and what
// config to apply (e.g. layout-driven reload). When skip_dialog is true and
// the layout's preset_config_json applies cleanly to the matching plugin,
// FileLoader::loadFile bypasses the data-source dialog entirely. On any
// failure (id mismatch, loadConfig rejection), the dialog falls back open
// with the existing QSettings-based pre-fill.
struct LoadHints {
  QString expected_plugin_id;  // Empty -> no hint; FileLoader picks plugin by extension as usual.
  QString preset_config_json;  // Empty -> no hint; QSettings pre-fill is used.
  // Normally honored when both fields above are non-empty and the plugin id
  // matches. rewrite_preset_filepath also permits an empty saved config because
  // it constructs the minimal fresh-filepath config required by browser replay.
  bool skip_dialog = false;
  // Layout replay reuses matching DatasetIds; normal load/reload replaces them.
  bool prefer_reuse = false;
  // Browser source-bound replay must use the plugin recorded by the layout. If
  // it is not among the extension matches, fail visibly instead of silently
  // choosing the first plugin. Default false preserves desktop selection.
  bool require_expected_plugin = false;
  // A saved browser preset contains the old upload's vanished MEMFS filepath.
  // Rewrite only that field to LoadInput::backing_path before loadConfig().
  // Default false preserves desktop preset bytes exactly.
  bool rewrite_preset_filepath = false;
  // Non-zero -> this load REPLACES the given dataset regardless of source
  // identity (the dataset "Replace" action: the incoming file is usually a
  // different path). Rides the transactional reload machinery — the in-place
  // refill keeps DatasetId/TopicIds (and so curve keys) stable where topic
  // names match, cancel/failure restores the prior data, and the dataset is
  // renamed to the new source only on commit. A vanished target degrades to a
  // plain fresh load.
  DatasetId replace_dataset_id = 0;
};

// Drives the file-import path: pick a file, find the matching DataSource
// plugin via ExtensionCatalogService, ingest into the SessionManager's data
// engine, and refresh the curve catalog. Lives in pj_app because it talks to
// QFileDialog/QMessageBox and to pj_marketplace's plugin handles.
class FileLoader : public QObject {
  Q_OBJECT
 public:
  FileLoader(
      SessionManager& session, ExtensionCatalogService& extensions, CatalogModel& catalog, QObject* parent = nullptr);
  ~FileLoader() override;

  FileLoader(const FileLoader&) = delete;
  FileLoader& operator=(const FileLoader&) = delete;

  // Opens a multi-select file dialog filtered by every installed file-import
  // plugin's extensions. On accept, runs loadFile() for EACH chosen path in
  // order (so you can populate several datasets in one go). The chosen directory
  // is persisted in QSettings under "FileLoader/lastDir".
  void openFromDialog(QWidget* dialog_parent);

  // The dataset "Replace" entry point: same plugin-extension filter and lastDir
  // persistence as openFromDialog, but single-select, and the chosen file loads
  // with replace_dataset_id set so it transactionally replaces `dataset_id`'s
  // data (see LoadHints::replace_dataset_id).
  void replaceFromDialog(DatasetId dataset_id, QWidget* dialog_parent);

#ifdef PJ_TARGET_WASM
  struct BrowserSelectionResult {
    QString browser_name;
    std::optional<LoadInput> input;
    QString error;
  };
  using BrowserSelectionCallback = std::function<void(BrowserSelectionResult)>;

  // Opens the same browser picker/staging path used by openFromDialog, but
  // returns the staged LoadInput without enqueuing it. Source-bound layout
  // replay uses this to finish every required selection before importing any
  // data. Cancellation is an empty input/error pair.
  void selectBrowserInput(QWidget* dialog_parent, BrowserSelectionCallback callback);

  // Fingerprint captured while BrowserFileStore staged this opaque upload.
  // Empty for native paths, unknown identities, and failed pre-staging calls.
  // Kept outside SessionManager because it is browser-session metadata, not a
  // durable source path or plugin concern.
  [[nodiscard]] QString browserContentSha256(const QString& source_identity) const;
#endif

  // Resolves the "pick file(s)" interaction inside openFromDialog() /
  // replaceFromDialog(). Returns the selected paths (empty on cancel; at most
  // one entry when `multi` is false). The shell injects one that threads
  // MainWindow's chrome metrics into PJ::FileDialog, keeping FileLoader free of a
  // MainWindow link (which also makes it testable headlessly). Unset -> plain
  // PJ::FileDialog::getOpenFileName(s), no metrics.
  using FilePicker = std::function<QStringList(
      QWidget* parent, const QString& caption, const QString& dir, const QString& filter, bool multi)>;
  void setFilePicker(FilePicker picker) {
    file_picker_ = std::move(picker);
  }

  // Programmatic entry point. ENQUEUES the load and returns immediately; the
  // bool now means "accepted/enqueued", NOT "loaded" — a single-instance load
  // runs progressively on a worker thread, so completion is asynchronous and
  // signalled by fileLoaded()/fileLoadFailed(). Loads run sequentially; a
  // second call while one is in flight queues behind it. Fanout control slots
  // run on the GUI thread, while each blocking import step runs on a worker.
  bool loadFile(const QString& path, QWidget* dialog_parent = nullptr);
  bool loadFile(const QString& path, QWidget* dialog_parent, const LoadHints& hints);
  bool loadFile(LoadInput input, QWidget* dialog_parent = nullptr, const LoadHints& hints = {});

  // Cancel the in-progress worker load. keep_partial=true keeps the rows parsed
  // so far (Primary/"Cancel"); false discards the dataset being filled
  // (Secondary/"Discard"). No-op when no worker load is running.
  void cancelCurrent(bool keep_partial);

  // Generation-guarded cancel: no-op unless `generation` equals the load
  // currently processing. A stop dialog captures the generation of the load it
  // was opened for (loadGeneration()) and passes it here, so a confirmation left
  // up after that load finished and a NEXT load began cannot cancel the wrong
  // load. keep_partial as above.
  void cancelCurrent(std::uint64_t generation, bool keep_partial);

  // Monotonic id of the load currently being processed (worker or suspended
  // prologue), or 0 when idle. Bumped each time a queued request begins. The
  // stop-dialog uses it to bind its cancel/auto-dismiss to one specific load.
  [[nodiscard]] std::uint64_t loadGeneration() const {
    return load_generation_;
  }

  // True while a load is running (worker active or mid-prologue) or queued.
  [[nodiscard]] bool isBusy() const;

  // DatasetId of the single-instance load currently filling on the worker, or 0
  // when none. Lets the shell grow the playback range as that dataset fills.
  // GUI-thread only.
  [[nodiscard]] DatasetId activeLoadDatasetId() const;

  // Stop the worker (discard) and drain the queue. Call from MainWindow::closeEvent
  // BEFORE tearing down the session/datastore; also invoked by the destructor.
  void joinForShutdown();

  // 3D TF ingest is triggered at load time through this service (owned by the
  // app shell, not the domain-neutral runtime). When unset, TF ingest is
  // skipped — non-3D builds simply never set it.
#ifdef PJ_WITH_SCENE3D
  void setTransformService(pj::scene3d::TransformService* service) {
    transform_service_ = service;
  }
#endif

  // Normalized full filesystem path the given dataset was loaded from, or empty
  // if this loader did not create it (e.g. a streaming or test dataset, or an id
  // it has since forgotten). Reads through SessionManager's source-path registry
  // (the single owner of dataset->path identity). The shell uses this to
  // translate a DatasetId back to the loaded-source entry when a dataset is
  // removed.
  [[nodiscard]] QString sourcePathForDataset(DatasetId dataset_id) const;
  // Drop the session's dataset->path association after dataset removal. Safe to
  // call for unknown ids.
  void untrackDataset(DatasetId dataset_id);

  // True when two source strings name the same loaded source: exact match for
  // opaque browser-upload identities, canonical filesystem-path identity
  // otherwise. The single comparison rule for matching recorded sources —
  // callers must use this instead of raw path equality.
  [[nodiscard]] static bool sameSourceIdentity(const QString& lhs, const QString& rhs);

  // Configure the object-ingest policy every load uses: scalars eager, objects
  // lazy-on-pull by default, and the heavy / scalar-less payloads (point clouds,
  // compressed point clouds, video frames, images, depth images, scene entities,
  // image annotations) PURE-LAZY so their bytes are re-fetched on read instead of
  // pinned in RAM at ingest. Static + resolver-typed so it is unit-testable
  // without standing up a full DataSourceRuntimeHost. TF stays eager on purpose:
  // its payload is tiny and its scalar fields are useful.
  static void applyDefaultIngestPolicies(PJ::sdk::ObjectIngestPolicyResolver& resolver);

 signals:
  /// A fan-out replacement is about to retire `dataset_id`. The shell captures
  /// path-qualified workspace state — stamping the retiring dataset with `path`,
  /// which on a dataset "Replace" load is a DIFFERENT file than it was loaded
  /// from — before the old catalog item is hidden, then rebinds it after
  /// fileLoaded exposes the reminted datasets.
  void sourceReplacementAboutToCommit(const QString& path, DatasetId dataset_id);

  void fileLoaded(
      const QString& path, const QString& prefix, const QString& plugin_id, const QString& plugin_config_json);
  void fileLoadFailed(const QString& path, const QString& reason);

  // Emitted when a load begins ingesting (after the modal dialog). `title` is the
  // filename; `file_index`/`file_total` are 1-based position among queued loads
  // (total<=1 => no N-of-M). `determinate` is false when the plugin reported no
  // step total (the bar should show busy/indeterminate).
  void ingestStarted(const QString& title, int file_index, int file_total, bool determinate);
  // Progress of the current load (maximum 0 => busy). Marshalled to the GUI thread.
  void ingestProgress(int current, int maximum);
  // The active load and the queue are both empty.
  void queueDrained();
  // A queued load took over as the current generation (emitted before its
  // dialog/ingest phases run). UI bound to a previous generation — e.g. the
  // stop-confirmation dialog — dismisses on this: fileLoaded/fileLoadFailed
  // fire while the finished load is still the current generation, so they
  // cannot signal the transition.
  void loadGenerationAdvanced(std::uint64_t generation);

 private:
#ifdef PJ_TARGET_WASM
  // Shared browser-picker completion for openFromDialog / replaceFromDialog:
  // stages the selection via selectBrowserInput, reports failures, and enqueues
  // the staged input through loadFile with `hints` (the entry points differ
  // only in the hints they pass). Must be called from a user-activation turn.
  void loadFromBrowserPicker(QWidget* dialog_parent, LoadHints hints);
#endif

  // One queued load request (a single loadFile call).
  struct LoadRequest {
    LoadInput input;
    QPointer<QWidget> dialog_parent;
    LoadHints hints;
    int file_index = 1;
    int file_total = 1;
  };
  // Per-load state that must outlive the GUI prologue into the worker and back.
  // Defined in the .cpp (holds a DataSourceHandle + DataSourceRuntimeHost).
  struct LoadContext;
  // Owned coroutine frame for the GUI-side load prologue. It keeps the staged
  // file lease, plugin handle, rollback state, and dialog inputs alive while a
  // browser-safe plugin dialog is open. Defined in the .cpp.
  struct BeginLoadTask;

  // Tears down one dataset created by an abandoned/rolled-back load: TF
  // invalidation (if a transform_service_ is wired), then optionally
  // ObjectStore eviction and/or the catalog-item removal, then the engine-side
  // erase via SessionManager::removeDataset (never dataEngine directly, so the
  // pinned time-origin is invalidated and the global reframe fires if needed).
  // TF is ALWAYS invalidated before eviction — invalidateDataset's cursor
  // cleanup walks listTopics(id), which eviction would otherwise empty first.
  // evict_objects=false skips ObjectStore eviction for a shell that never
  // started ingest (nothing to evict). remove_from_catalog=false skips the
  // catalog call for a dataset already tombstoned by an earlier removeDataset.
  void removeCreatedDataset(DatasetId dataset_id, bool evict_objects = true, bool remove_from_catalog = true);

  // GUI-thread body of one throttled ingest flush, shared by the single-instance
  // and fan-out progress ticks (each keeps its own staleness guard): publish the
  // newly committed rows to plots/playback, fold new FrameTransforms so 3D
  // scenes track the load live, and advance the progress strip.
  void publishIngestProgress(DatasetId dataset_id, int current, int maximum);

  // GUI: dequeue and begin the next load if idle. Emits queueDrained when the
  // queue empties with neither a suspended prologue nor a worker.
  void startNext();
  // GUI coroutine for one request: resolve plugin, await its dialog without a
  // nested event loop, then either hand completion to the ingest worker or
  // finish the prologue and advance the queue.
  [[nodiscard]] BeginLoadTask beginLoad(LoadRequest request);
  // GUI: complete a non-worker prologue and schedule the next request without
  // recursively replacing the currently executing coroutine frame.
  void finishPrologue();
  // WORKER thread body: runs ctx_->handle.start(), driving throttled
  // flushPending()+queued notifyIngest() from on_progress_update.
  void runIngestOnWorker(std::uint64_t generation);
  // GUI (queued from the worker): join the worker, finalize or discard, then
  // startNext().
  void onWorkerFinished(std::uint64_t generation);
  // GUI: post-ingest reconciliation for the just-finished single-instance load
  // (catalog rebuild, TF ingest, saveConfig, source-path tracking, fileLoaded).
  void finishLoadOnGui();
  // GUI: after a replacing reload's RefillGuard has rolled the dataset back to its
  // pre-reload data (start-fail / discard / shutdown), reflect the restored data in
  // the catalog and rebuild the per-dataset TF buffer. The guard restores the data +
  // re-notifies adapters; this refreshes the catalog tree + scene TF on top.
  /// Failure exit for a replacing reload: roll the refill back (guard dtor),
  /// refresh the catalog/UI to the restored state, and emit fileLoadFailed.
  void failReplacingLoad(DatasetId dataset_id, const QString& path, const QString& reason);
  void refreshAfterReplacingRollback(DatasetId dataset_id);

  // Surface one load failure to the user, aggregating consecutive failures into
  // a SINGLE dialog: while a warning dialog is still open, each new reason is
  // appended to its body (and the title becomes "N loads failed") instead of
  // opening another overlapping box. Non-blocking (show(), not exec()), so the
  // async load queue is never stalled. `parent` may be null (headless/no-UI
  // load) — then only the count is tracked and no dialog is shown.
  void reportLoadWarning(QWidget* parent, const QString& reason);

  SessionManager& session_;
  ExtensionCatalogService& extensions_;
  CatalogModel& catalog_;
  FilePicker file_picker_;
  std::shared_ptr<BrowserFileStore> browser_file_store_;
#ifdef PJ_TARGET_WASM
  QHash<QString, QString> browser_content_sha256_;
#endif
#ifdef PJ_WITH_SCENE3D
  pj::scene3d::TransformService* transform_service_ = nullptr;
#endif

  // --- Sequential async load queue (single-instance loads run on a worker) ---
  std::deque<LoadRequest> queue_;
  std::unique_ptr<BeginLoadTask> begin_load_task_;
  std::unique_ptr<QThread> worker_;
  // Runtime host of the fan-out entry whose start() is on the worker right now
  // (else nullptr). The host is a coroutine-frame local, so cancelCurrent /
  // joinForShutdown reach its requestStop() through this. GUI-thread only.
  DataSourceRuntimeHost* active_fanout_ingest_ = nullptr;
  // GUI-thread pointer to the synchronous-ABI plugin question currently open.
  // Shutdown rejects it before joining a worker that may be waiting for it.
  // Heap-shared because the WA_DeleteOnClose dialog (a child of the main
  // window) can outlive this loader: its finished/destroyed hooks capture this
  // shared state, never FileLoader memory.
  std::shared_ptr<QPointer<QDialog>> active_plugin_message_dialog_;
  // joinForShutdown() shuts the gate (pending questions answer -1, queued ones
  // become no-ops) so a worker parked in the message-box ABI can never deadlock
  // the join; a fresh gate is minted afterwards so the loader stays reusable.
  std::shared_ptr<PluginMessageGate> message_gate_;
  // The single load-warning dialog open right now; V9 aggregation appends new
  // failure lines to it instead of stacking N overlapping dialogs. Cleared when
  // it closes (via its finished/destroyed hook). GUI-thread only.
  QPointer<QDialog> active_load_warning_;
  // Failure reasons shown in active_load_warning_, in order. Repopulates the
  // dialog body on each append; reset when the dialog closes.
  QStringList load_warning_lines_;
  std::unique_ptr<LoadContext> ctx_;  // current single-instance load; null when idle
  // True while a load is being processed (suspended prologue or worker).
  // GUI-thread only; guards startNext re-entrancy.
  bool active_load_ = false;
  // Monotonic id of the load currently processing; bumped each time startNext
  // dequeues a request. 0 while idle. GUI-thread only. Lets a stop-dialog bind
  // to one specific load (see cancelCurrent(generation, ...)).
  std::uint64_t load_generation_ = 0;
  // Counter backing load_generation_. Kept separate so returning to idle or
  // joinForShutdown() never reuses an old token: queued worker/progress calls
  // from an earlier load can therefore be rejected after shutdown + reuse.
  std::uint64_t next_load_generation_ = 0;
  // Cancellation request for the current worker load: 0=none, 1=keep, 2=discard.
  // Written by cancelCurrent/joinForShutdown (GUI), read by the worker.
  std::atomic<int> cancel_mode_{0};
  // Minimum wall-clock between worker-side flush+notify cycles (test seam).
  int flush_throttle_ms_ = 50;
  // Suppresses queue advancement while joinForShutdown destroys a suspended
  // dialog/prologue coroutine; cleared when shutdown completes so the loader
  // stays usable afterwards.
  bool shutting_down_ = false;
};

}  // namespace PJ
