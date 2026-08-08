#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QDateTime>
#include <QList>
#include <QMap>
#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QString>
#include <cstdint>
#include <functional>
#include <utility>

#include "pj_base/diagnostic_sink.hpp"
#include "pj_marketplace/download_manager.hpp"
#include "pj_marketplace/extension.hpp"
#include "pj_marketplace/installed_extension.hpp"
#include "pj_marketplace/platform_utils.hpp"

namespace PJ {

// One user-visible diagnostic emitted by marketplace lifecycle operations.
struct ExtensionDiagnostic {
  QString id;       ///< Registry or embedded plugin id, when known.
  QString message;  ///< Human-readable diagnostic.
  bool is_error = false;
  QDateTime timestamp;  ///< UTC timestamp.
};

// Manages marketplace extension installs, updates, uninstalls, and startup cleanup.
// Installed metadata is derived from embedded DSO manifests, not local sidecars.
class ExtensionManager : public QObject {
  Q_OBJECT

 public:
  // Creates an owned DownloadManager and uses the standard user directories.
  ExtensionManager();

  // Uses the supplied downloader and directories; tests pass isolated temp paths.
  // The optional sink receives the same diagnostics as diagnosticReported() —
  // hosts can subscribe to one stream that also carries non-marketplace events.
  explicit ExtensionManager(
      DownloadManager* downloader, const QString& extensions_dir = PlatformUtils::extensionsDir(),
      const QString& pending_dir = PlatformUtils::pendingDir(), DiagnosticSink sink = {}, QObject* parent = nullptr);

  // Starts an async install for the current platform.
  void install(const Extension& ext);

  // Host/plugin compatibility of a registry extension: the current platform must
  // be listed in ext.platforms AND the host version must be >= its declared
  // min_plotjuggler_version (an empty value imposes no floor). The host version is
  // QCoreApplication::applicationVersion(), the single source the app sets at
  // startup (tests set it the same way). `reason` is a human-readable explanation
  // when !ok, for the footer / an install-button tooltip. install() refuses an
  // incompatible extension (see doInstall).
  struct HostCompatibility {
    bool ok = true;
    QString reason;
  };
  HostCompatibility hostCompatibility(const Extension& ext) const;

  // Asked when a local archive carries an id that is already installed: return true
  // to replace it, false to abort. Invoked on the GUI thread from the handler that
  // runs once extraction finishes, so an implementation may block on a modal dialog
  // there, but NOT while installFromLocalZip() itself is on the stack.
  //
  // The manager cannot ask this itself — the decision is UI policy and this module
  // links no widgets — so the host injects it, the same way
  // StreamingSourceManager takes its chrome-metrics provider and FileLoader its
  // file picker. Left unset, a conflict is refused, which is what a test or the
  // standalone harness wants.
  using ReplaceConfirmation =
      std::function<bool(const QString& id, const QString& installed_version, const QString& archive_version)>;

  // Registers the decision on behalf of `context`, which must be the object whose
  // lifetime the callback's captures depend on (for a UI host, the widget it opens
  // its dialog on). Only a QPointer to it is kept, because the question is asked
  // ASYNCHRONOUSLY, once extraction finishes: the host window can be closed while
  // the archive is still unpacking, and invoking a lambda that captured it then is
  // a use-after-free. A confirmation whose context is gone resolves as DECLINED and
  // is dropped, so a conflicting install can never silently replace a plugin nobody
  // approved, and never stalls waiting for an answer that cannot come.
  //
  // A null `context` or an empty `confirm` clears the registration instead.
  //
  // Exactly one confirmation is stored, so a second registration displaces the
  // first: a host holding two marketplace windows open resolves every pending
  // decision against the last registrant. That residual quirk is a UX wart, not a
  // safety hole, since the guard makes a displaced window's disappearance harmless.
  void setReplaceConfirmation(QObject* context, ReplaceConfirmation confirm);

  // Drops the registration only when `context` is the object that made it, so a
  // window closing after another has taken over cannot disarm the live one. Call it
  // from the registrant's destructor.
  void clearReplaceConfirmation(QObject* context);

  // Sideloads a plugin from a local ZIP that the registry does not list.
  //
  // Unlike install(), there is no registry entry to validate against: the id and
  // version come FROM the archive's embedded plugin manifest, which is the only
  // source of truth here. Consequently there is also no checksum to verify — a
  // local file has no provenance, and the integrity gate is the manifest scan
  // (the DSO must load and describe itself coherently), not a digest.
  //
  // The archive must hold exactly one top-level directory containing the plugin.
  // On success the directory lands in extensionsDir() keyed by the discovered id —
  // the same root every other install writes to, so a sideload follows a
  // `--plugin-dir` override exactly like a registry install does. The plugin becomes
  // live at the next application start; installing never loads it into the running
  // session.
  //
  // If that id is ALREADY installed, setReplaceConfirmation() decides. A confirmed
  // replacement is staged rather than written over the existing directory, because
  // a DSO the running session loaded cannot be swapped underneath it (Windows locks
  // the file) and dlopen keys its cache by path name, so a same-path replacement is
  // never re-read. applyPendingInstalls() promotes the stage on the next launch,
  // backing up the displaced version first, and installPendingRestart() is emitted
  // instead of installFinished(). The version comparison that update() applies is
  // deliberately skipped: a local archive is an explicit act on a specific file,
  // and rebuilding without bumping the version is the normal case.
  //
  // Reports through the same installStarted / installFinished / installError
  // signals as install(), except that installStarted is emitted only once the id
  // is known (after the manifest is read), since that signal is keyed by id.
  void installFromLocalZip(const QString& zip_path);

  // Removes an installed extension or schedules Windows cleanup after restart.
  void uninstall(const QString& extension_id);

  // Reverts a core extension that was updated ABOVE its bundled version back to the
  // shipped one. Staged, not immediate: it marks the updated copy for restart
  // cleanup (so the card shows "Needs Restart", not "Install") and emits
  // downgradePendingRestart(). On the next launch applyPendingUninstalls removes it
  // and the host seed restores the bundled version. Safe because the bundled build
  // ships with the app and is always compatible. No-op if `id` is not a core
  // extension sitting above its bundled version.
  void downgradeToBundled(const QString& extension_id);

  // Replaces an installed extension. Stages the new version in pending_dir_;
  // applyPendingInstalls() promotes it (backing up the old one) at the next
  // startup, so an update never hot-swaps a loaded DSO.
  void update(const Extension& ext);

  // Promotes validated staged installs from PlatformUtils::pendingDir().
  void applyPendingInstalls();

  // Deletes extension directories previously marked for restart cleanup.
  void applyPendingUninstalls();

  // Returns true when the latest disk scan found this extension id.
  bool isInstalled(const QString& id) const;

  // Sets id -> version for the plugins that ship with the application ("core"),
  // computed by the host from the bundled plugin directory. Membership (not a
  // per-folder marker) gates uninstall, so it survives updates and needs no
  // on-disk flag; the version enables the "downgrade to bundled" affordance.
  void setBundledVersions(const QMap<QString, QString>& id_to_version);

  // Sets id -> version for the copies the startup seed actually WROTE into the
  // managed dir this session, so installedVersion() can report them: the scan
  // snapshot predates the write and cannot be refreshed into agreement (see
  // installedVersion). Distinct from setBundledVersions(), which names every
  // shipped plugin whether or not the seed touched it.
  //
  // Only ids whose payload was promoted successfully belong here — an id the
  // seed skipped or failed to write must keep reporting what the scan found.
  // Unlike the bundled set this is not policy but a statement of fact about the
  // managed dir, so the host reports it in every mode.
  void setSeededVersions(const QMap<QString, QString>& id_to_version);

  // Returns true when `id` ships with the application ("core"). A core extension
  // at its bundled version cannot be uninstalled (uninstall() refuses it, the UI
  // disables the action); a core extension updated ABOVE its bundled version can
  // be reverted via "downgrade to bundled". False when no bundled set was provided
  // (standalone app / --plugin-dir run).
  bool isBundled(const QString& id) const;

  // The version `id` ships with, or empty if `id` is not core. Used to decide
  // between the disabled Uninstall (installed == bundled) and the "downgrade to
  // bundled" action (installed > bundled), and to show the version transition.
  QString bundledVersion(const QString& id) const;

  // ── Enable/disable (installed but not loaded) ──────────────────────────────
  // A disabled extension stays on disk but the host skips loading it. The state
  // persists globally in QSettings so both the marketplace and the runtime
  // catalog read the same list, and it applies on the next launch (a loaded DSO
  // cannot be unloaded live).

  // True when `id` is NOT in the disabled list (independent of installed state).
  bool isEnabled(const QString& id) const;

  // Adds/removes `id` from the persisted disabled list. Does not touch the
  // already-loaded plugin — the change takes effect on the next launch.
  void setEnabled(const QString& id, bool enabled);

  // The persisted set of disabled extension ids, read straight from QSettings.
  // Static so the runtime catalog can honor it without an ExtensionManager
  // instance (the single source of truth for the QSettings key).
  static QStringList disabledExtensionIds();

  // Rebuilds installed state by scanning extension directories for plugin DSOs.
  void refreshInstalledFromDisk();

  // Replaces the installed-state cache with a caller-provided snapshot.
  void setInstalledExtensions(QMap<QString, InstalledExtension> installed);

  // Returns true when a staged install has a matching intent and valid DSO.
  bool hasPendingInstall(const QString& id) const;

  // Returns true when an installed directory is marked for restart cleanup.
  bool hasPendingUninstall(const QString& id) const;

  // Records of the extensions whose removal is staged, kept because they are no
  // longer in installedExtensions() yet still exist on disk and keep running
  // until the restart. A UI that composes its rows from the installed set alone
  // would drop them and leave the user with no sign that anything is pending —
  // which matters most for an extension the registry does not list, since the
  // registry cannot supply a replacement row. Pruned once the marker is gone.
  QMap<QString, InstalledExtension> stagedUninstalls() const;

  // The version of `id` actually on disk. Prefer this over reading
  // `installedExtensions()[id].version` directly — the scan snapshot can be
  // stale in a specific window that this helper compensates for.
  //
  // The window: the startup seed writes a core plugin's managed copy, but it has
  // to read the installed version first to decide whether to write at all — and
  // that read opens the DSO. glibc then answers every later dlopen of the same
  // path from that first-loaded image (plugin DSOs export STB_GNU_UNIQUE
  // symbols, so dlclose never unloads them), so no rescan in this process can
  // observe what the seed promoted; only a restart can.
  //
  // Which is why the seed reports what it wrote, via setSeededVersions(), and
  // this returns that value verbatim for the ids it names. Inferring it from the
  // bundled set instead would break on the seed's own downgrade case: restoring
  // an incompatible installed copy leaves the managed dir BELOW the version the
  // scan reported, so no rule over (scanned, bundled) can name it.
  //
  // Empty when `id` is not installed. An id the seed did not write falls through
  // to the scanned version, which is then authoritative — that covers a
  // marketplace copy the seed deliberately left alone, and every id in a
  // standalone-app or --plugin-dir run.
  QString installedVersion(const QString& id) const;

  // Returns true when the installed version is newer than the registry version.
  bool hasNewerInstalledVersion(const Extension& ext) const;

  // Compares registry and installed versions using QVersionNumber.
  bool hasUpdate(const Extension& ext) const;

  // Returns the current installed-extension snapshot keyed by id.
  QMap<QString, InstalledExtension> installedExtensions() const;

  // Returns recent lifecycle diagnostics for UI display.
  QList<ExtensionDiagnostic> diagnostics() const;

  // Clears the in-memory diagnostic history.
  void clearDiagnostics();

  // True when a diagnostic has been recorded that has not yet been surfaced to
  // the user via markDiagnosticsSurfaced(). Lets the UI show a diagnostic on
  // window open exactly once (diagnostics live here and persist across the
  // per-open window instances), instead of re-showing a stale one every re-open.
  bool hasUnsurfacedDiagnostics() const;

  // Marks every diagnostic recorded so far as surfaced, so hasUnsurfacedDiagnostics()
  // returns false until a newer one arrives.
  void markDiagnosticsSurfaced();

  // Root directory where extension DSOs are discovered and managed.
  QString extensionsDir() const {
    return extensions_dir_;
  }

#ifdef PJ_MARKETPLACE_TESTING
  // Test hook for forcing direct or staged install paths.
  void testDoInstall(const Extension& ext, bool staging, bool allow_existing = false) {
    doInstall(ext, staging, allow_existing);
  }

#endif

 signals:
  // Emitted when an install or update starts.
  void installStarted(const QString& id);

  // Emitted with percentage progress for the active download.
  void installProgress(const QString& id, int percent);

  // Reports post-download work whose duration is not covered by installProgress.
  // See DownloadManager::WorkPhase for the meaning of each value; consumers
  // should switch their UI to an indeterminate/busy indicator when this fires.
  void installPhase(const QString& id, PJ::DownloadManager::WorkPhase phase);

  // Emitted when install or update completes.
  void installFinished(const QString& id, bool success);

  // Human-readable failure detail; followed by installFinished(id, false).
  void installError(const QString& id, const QString& error_message);

  // Emitted when an update is staged and will be active after a restart. A fresh
  // install promotes immediately and emits installFinished instead.
  void installPendingRestart(const QString& id);

  // Emitted when uninstall completes.
  void uninstallFinished(const QString& id, bool success);

  // Human-readable uninstall failure detail.
  void uninstallError(const QString& id, const QString& error_message);

  // Emitted when uninstall requires restart cleanup.
  void uninstallPendingRestart(const QString& id);

  // Emitted when a core extension is staged to revert to its bundled version on the
  // next launch (downgradeToBundled). The card shows "Needs Restart".
  void downgradePendingRestart(const QString& id);

  // Emitted whenever a diagnostic is appended to diagnostics().
  void diagnosticReported(const QString& id, const QString& message, bool is_error);

 private:
  // How a replace conflict was resolved. Everything except kAccepted aborts the
  // install; they are distinguished only so the user is told which one happened.
  enum class ReplaceDecision : std::uint8_t {
    kNoConfirmation,  ///< Nothing registered, so a conflict is refused outright.
    kOwnerGone,       ///< The registrant died before or during the answer.
    kSuperseded,      ///< The registration changed while the question was open.
    kDeclined,
    kAccepted,
  };

  // Puts the replace question to the registered confirmation behind its context
  // guard. A dead guard yields kOwnerGone WITHOUT invoking the callback, and drops
  // the registration so the next conflict is refused rather than asked again.
  //
  // The callback typically blocks on a modal dialog, which spins the event loop, so
  // a second window can register or clear WHILE it runs. Two consequences are
  // handled here rather than by the callers: the callable is copied before being
  // invoked (assigning to the executing std::function would otherwise be undefined
  // behavior), and the answer is matched to the registration that asked via
  // (context, replace_confirmation_generation_) — an answer from a registration
  // that has since been displaced is kSuperseded, never accepted on the strength
  // of its successor being alive.
  ReplaceDecision askReplaceConfirmation(const QString& id, const QString& archive_version);

  // Clears the registration and bumps the generation, so an answer still in flight
  // from it can no longer be accepted. Every drop goes through here to keep the
  // bump paired with the clear.
  void dropReplaceConfirmation();

  // Called by both constructors to finish setup after members are assigned.
  void initComponents();

  // Shared install implementation for direct and staged destinations.
  void doInstall(const Extension& ext, bool staging, bool allow_existing = false);

  // Disconnects downloader signals for the current operation.
  void disconnectDlConns();

  // Writes the restart-cleanup marker into an installed extension directory.
  // Returns false if the marker file could not be created — caller must NOT remove
  // the in-memory entry in that case, otherwise the directory will leak.
  bool schedulePendingUninstall(const QString& path);

  // Appends a diagnostic and notifies observers.
  void reportDiagnostic(const QString& id, const QString& message, bool is_error);

  // Emits installError + installFinished(false) and records a diagnostic.
  void emitInstallFailure(const QString& id, const QString& message);

  // Stamps a freshly-promoted directory with its absolute path + mtime and
  // adds it to the installed_ map under `id`. Caller supplies the record
  // already populated from the embedded manifest.
  //
  // A fresh install (default) starts enabled and clears any stale disabled
  // entry. A staged update/replace passes preserve_disabled_state=true so
  // promoting the new version keeps the user's enable/disable choice — a plugin
  // the user disabled must not silently come back enabled after its update.
  void registerInstalledExtension(
      const QString& id, const QString& dst, InstalledExtension record, bool preserve_disabled_state = false);

  // Backs up (or removes) any directory under extensions_dir_ — other than
  // `keep_dir` — whose embedded plugin id equals `id`, so the promoted "<id>"
  // directory is that extension's sole install: a prior copy stored under a
  // DIFFERENT directory name (e.g. a bundled plugin in "data-load-foo" for id
  // "foo") would otherwise linger as a duplicate that refreshInstalledFromDisk
  // resolves non-deterministically by directory name. The scan dlopens sibling
  // DSOs, so during the startup drain this must only run once every staged
  // promotion has landed (see applyPendingInstalls) — opening a not-yet-promoted
  // sibling would pin its pre-update image in the process.
  void replaceConflictingInstallDirs(const QString& id, const QString& keep_dir);

  // Emits uninstallError + uninstallFinished(false) and records a diagnostic.
  void emitUninstallFailure(const QString& id, const QString& message);

  // Deletes every `.pj_install_*` transaction directory directly under `parent`,
  // except the one a running install is still extracting into. Called for the
  // staging sibling installs actually use, and for the extensions dir itself,
  // where an older build could have left one inside the scanned tree.
  void sweepTransactionRoots(const QString& parent);

  DownloadManager* downloader_ = nullptr;
  QString extensions_dir_;
  ReplaceConfirmation replace_confirmation_;
  // Lifetime guard for replace_confirmation_: null once the registrant is gone,
  // which turns a pending decision into a decline instead of a call into freed
  // memory. See setReplaceConfirmation().
  QPointer<QObject> replace_confirmation_context_;
  // Identifies WHICH registration is armed, bumped by every set and every drop.
  // A live context alone cannot say that: the question is answered inside a modal
  // event loop, so the slot may hold a DIFFERENT (also live) registration by the
  // time the answer comes back. See askReplaceConfirmation().
  quint64 replace_confirmation_generation_ = 0;
  QString pending_dir_;
  DiagnosticSink sink_;

  QMap<QString, InstalledExtension> installed_;

  // id -> the record it had when its uninstall was staged. Survives the panel
  // being closed and reopened because this manager outlives the window; a
  // restart needs no entry, since the drain removes the extension outright.
  QMap<QString, InstalledExtension> staged_uninstalls_;

  // id -> version for the plugins that ship with the application ("core"), set by
  // the host via setBundledVersions(). Membership locks uninstall (isBundled); the
  // version drives the downgrade-to-bundled affordance. Empty by default.
  QMap<QString, QString> bundled_versions_;

  // id -> version the startup seed wrote into the managed dir this session, set by
  // the host via setSeededVersions(). installedVersion() answers from here first,
  // because the scan snapshot predates the write. Empty by default.
  QMap<QString, QString> seeded_versions_;

  // Non-empty while a fetch is running; guards against concurrent install() calls.
  QString pending_id_;
  // ID returned by DownloadManager::fetch(); used to correlate incoming signals.
  int pending_op_id_ = -1;
  // Ensures the disk-space check runs at most once per fetch operation.
  bool disk_space_checked_ = false;
  // Set before calling cancel() to preserve the real reason shown to the user.
  QString cancel_reason_;
  // Transaction directory used by the currently running fetch/extract operation.
  QString pending_extract_dir_;
  // Backup location of the previous install, set when applyPendingInstalls()
  // promotes a staged update; used for failure diagnostics.
  QString pending_backup_path_;
  QList<ExtensionDiagnostic> diagnostics_;
  // Monotonic count of every diagnostic ever recorded (never reset by the
  // kMaxDiagnostics ring-buffer trim), and how many have been surfaced to the
  // user. total > surfaced means there is an unsurfaced diagnostic.
  quint64 diagnostics_recorded_ = 0;
  quint64 diagnostics_surfaced_ = 0;

  // Stored so we can disconnect cleanly after each operation completes.
  QMetaObject::Connection dl_progress_conn_;
  QMetaObject::Connection dl_phase_conn_;
  QMetaObject::Connection dl_finished_conn_;
  QMetaObject::Connection dl_failed_conn_;
  QMetaObject::Connection dl_cancelled_conn_;
};

}  // namespace PJ
