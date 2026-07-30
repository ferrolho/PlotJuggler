#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QDateTime>
#include <QString>

namespace PJ {

// Installed extension discovered from an embedded plugin manifest on disk.
//
// The name/description/category trio mirrors the same fields on Extension, but
// is sourced from the manifest rather than the registry. It exists so an
// extension NO registry lists can still be rendered as an ordinary row: without
// it, the manifest metadata read during discovery would be discarded and the
// UI would have nothing to display for a sideloaded plugin.
struct InstalledExtension {
  QString id;  ///< Matches Extension::id from the registry
  QString version;
  QDateTime install_date;
  QString path;  ///< Absolute path to <config-root>/extensions/<id>/
  bool enabled = true;

  QString name;         ///< Manifest display name; falls back to id when the manifest omits it
  QString description;  ///< Manifest description; may be empty
  /// Manifest category, expected to use the same vocabulary the registry ships
  /// ("data_loader" | "data_stream" | "message_parser" | "toolbox") since that is
  /// what the filter row matches against. A plugin declaring anything else simply
  /// matches no category facet.
  QString category;
};

}  // namespace PJ
