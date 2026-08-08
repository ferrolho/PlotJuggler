#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QString>
#include <string>

namespace PJ {

// Static helpers for platform detection and standard directory resolution.
//
// All path helpers return absolute paths without a trailing separator.
// Directories are NOT created here — callers are responsible for mkpath.
class PlatformUtils {
 public:
  // Returns the platform identifier used as key in registry artifact maps.
  // Format: "<os>-<arch>", e.g. "linux-x86_64", "windows-x86_64", "macos-arm64".
  static QString currentPlatform();

  // Returns true on Windows builds.
  static bool isWindows();

  // Returns the shared library extension for the current platform:
  //   Linux:   ".so"
  //   Windows: ".dll"
  //   macOS:   ".dylib"
  static std::string pluginExtension();

  // Root of all PlotJuggler user data (QStandardPaths::AppDataLocation, i.e. the
  // PlotJuggler/PlotJuggler4 org/app pair):
  //   Linux:   ~/.local/share/PlotJuggler/PlotJuggler4/
  //   Windows: %LOCALAPPDATA%/PlotJuggler/PlotJuggler4/
  //   macOS:   ~/Library/Application Support/PlotJuggler/PlotJuggler4/
  static QString configDir();

  // <config-root>/extensions/ — active, loaded extensions.
  static QString extensionsDir();

  // <config-root>/.extension_staging/ — restart staging for Windows updates.
  static QString pendingDir();

  // <config-root>/.backup/ — pre-update backups (F-12, deferred to April+).
  static QString backupDir();

  // Identity of a managed store: `store_dir` made absolute with symlinks resolved.
  //
  // Anything placed BESIDE a store must be derived from this rather than from the
  // configured string, because a configured path can be a symlink: a packaged
  // install pointing at a data volume, a developer linking the store elsewhere.
  // Derived textually, a sibling is created next to the LINK, which resolves onto
  // a different filesystem than the store itself and turns promote-by-rename into
  // EXDEV; and two names for one store yield two different sibling paths, so a
  // lock file keyed that way would hand out two writer leases for one store.
  //
  // Shared policy for every sibling PlotJuggler keeps next to the extensions dir:
  // the writer lease (ExtensionManager), the install transaction staging area
  // (ExtensionManager), and the bundled-seed staging area (pj_runtime's
  // ExtensionCatalogService).
  //
  // Falls back to the cleaned path when resolution fails — canonicalFilePath() is
  // empty for a path that does not exist, which is the first-run state rather than
  // an error. Create the store before deriving siblings from it to stay off that
  // path.
  static QString canonicalStoreRoot(const QString& store_dir);
};

}  // namespace PJ
