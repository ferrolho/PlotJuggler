// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <QByteArray>
#include <QString>
#include <functional>
#include <memory>
#include <optional>

#include "LoadInput.h"

class QWidget;

namespace PJ {

class BrowserFileStore;

class FileSelectionService {
 public:
  struct Selection {
    QString browser_name;
    QByteArray bytes;
  };
  using Callback = std::function<void(Selection)>;

  struct StagedSelection {
    QString browser_name = {};
    std::optional<LoadInput> input = std::nullopt;
    QString error = {};
  };
  using StagedCallback = std::function<void(StagedSelection)>;

  // Opens Qt's content-based picker and returns immediately. An empty filename
  // represents cancellation/no selection; zero-byte files remain valid.
  static void selectFileContent(QWidget* parent, const QString& name_filter, Callback callback);

  // Hands complete content to Qt's platform save path. On WebAssembly this
  // requests a browser download and has no success/cancel callback or durable
  // destination path.
  static void saveFileContent(QWidget* parent, const QByteArray& bytes, const QString& file_name_hint);

  // Selects browser content and stages it behind a LoadInput lease. Cancellation
  // is reported as an empty input with no error; staging failures carry `error`.
  // browser_name is preserved for diagnostics once a selection was made.
  // The shared store keeps kickoff safe even if the caller is destroyed while
  // the browser picker is pending.
  static void selectAndStageFile(
      QWidget* parent, const QString& name_filter, std::shared_ptr<BrowserFileStore> store, StagedCallback callback);

  // Selects the aggregate first QFileDialog filter for the browser. The web
  // picker has no desktop-style filter dropdown, and duplicate aggregate /
  // per-plugin entries can become redundant native File System Access types.
  static QString browserContentFilter(const QString& name_filter);
};

}  // namespace PJ
