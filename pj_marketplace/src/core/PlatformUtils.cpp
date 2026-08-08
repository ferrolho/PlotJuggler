// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QSysInfo>

#include "pj_marketplace/platform_utils.hpp"

namespace PJ {

QString PlatformUtils::currentPlatform() {
  // QSysInfo::kernelType() returns "linux", "winnt", "darwin", etc.
  // Normalise to the friendlier names used in registry artifact keys.
  const QString kernel = QSysInfo::kernelType();
  const QString arch = QSysInfo::currentCpuArchitecture();

  QString os;
  if (kernel == "linux") {
    os = "linux";
  } else if (kernel == "winnt") {
    os = "windows";
  } else if (kernel == "darwin") {
    os = "macos";
  } else {
    os = kernel;
  }

  return os + "-" + arch;
}

std::string PlatformUtils::pluginExtension() {
#if defined(Q_OS_WIN)
  return ".dll";
#elif defined(Q_OS_MACOS)
  return ".dylib";
#else
  return ".so";
#endif
}

bool PlatformUtils::isWindows() {
#ifdef Q_OS_WIN
  return true;
#else
  return false;
#endif
}

QString PlatformUtils::configDir() {
  return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
}

QString PlatformUtils::extensionsDir() {
  return configDir() + "/extensions";
}

QString PlatformUtils::pendingDir() {
  return configDir() + "/.extension_staging";
}

QString PlatformUtils::backupDir() {
  return configDir() + "/.backup";
}

QString PlatformUtils::canonicalStoreRoot(const QString& store_dir) {
  const QString canonical = QFileInfo(store_dir).canonicalFilePath();
  return canonical.isEmpty() ? QDir::cleanPath(store_dir) : canonical;
}

}  // namespace PJ
