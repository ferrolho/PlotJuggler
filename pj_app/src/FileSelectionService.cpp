// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "FileSelectionService.h"

#include <QFileDialog>
#include <string>
#include <utility>

#include "BrowserFileStore.h"
#ifdef Q_OS_WASM
#include <emscripten/val.h>
#endif
using namespace Qt::StringLiterals;

namespace PJ {

#ifdef Q_OS_WASM
namespace {

class ScopedOpenFilePickerFallback {
 public:
  ScopedOpenFilePickerFallback()
      : window_(emscripten::val::global("window")),
        picker_(window_["showOpenFilePicker"]),
        had_own_property_(
            emscripten::val::global("Object")["prototype"]["hasOwnProperty"].call<bool>(
                "call", window_, std::string("showOpenFilePicker"))) {
    window_.set("showOpenFilePicker", emscripten::val::undefined());
  }

  ~ScopedOpenFilePickerFallback() {
    if (had_own_property_) {
      window_.set("showOpenFilePicker", picker_);
    } else {
      emscripten::val::global("Reflect").call<bool>("deleteProperty", window_, emscripten::val("showOpenFilePicker"));
    }
  }

  ScopedOpenFilePickerFallback(const ScopedOpenFilePickerFallback&) = delete;
  ScopedOpenFilePickerFallback& operator=(const ScopedOpenFilePickerFallback&) = delete;

 private:
  emscripten::val window_;
  emscripten::val picker_;
  bool had_own_property_;
};

}  // namespace
#endif

QString FileSelectionService::browserContentFilter(const QString& name_filter) {
  return name_filter.section(u";;"_s, 0, 0);
}

void FileSelectionService::selectFileContent(QWidget* parent, const QString& name_filter, Callback callback) {
  // The aggregate first filter already contains every supported extension.
  const QString browser_filter = browserContentFilter(name_filter);
#ifdef Q_OS_WASM
  // Qt 6.11 prefers the File System Access API when Chromium exposes it, but
  // showOpenFilePicker can reject immediately in the cross-origin-isolated
  // context required by a threaded build. Qt chooses its implementation during
  // this call, so mask only the open-picker probe long enough to select its
  // portable <input type=file> path, then restore the browser API immediately.
  ScopedOpenFilePickerFallback fallback;
#endif
  QFileDialog::getOpenFileContent(  // NOLINT(pj-canonical-chrome): Qt's browser byte API has no PJ wrapper.
      browser_filter,
      [callback = std::move(callback)](const QString& name, const QByteArray& bytes) mutable {
        callback(Selection{.browser_name = name, .bytes = bytes});
      },
      parent);
}

void FileSelectionService::saveFileContent(QWidget* parent, const QByteArray& bytes, const QString& file_name_hint) {
#ifdef Q_OS_WASM
  // Qt 6.11 gates BOTH its open and save File System Access paths on
  // showOpenFilePicker. In the COOP/COEP context needed by this threaded build,
  // the save picker is not dependable. Mask only the capability probe for the
  // duration of this call so Qt chooses its synchronous Blob/<a download>
  // fallback, then restore the browser API immediately.
  ScopedOpenFilePickerFallback fallback;
#endif
  QFileDialog::saveFileContent(  // NOLINT(pj-canonical-chrome): Qt's browser download API has no PJ wrapper.
      bytes, file_name_hint, parent);
}

void FileSelectionService::selectAndStageFile(
    QWidget* parent, const QString& name_filter, std::shared_ptr<BrowserFileStore> store, StagedCallback callback) {
  selectFileContent(
      parent, name_filter, [store = std::move(store), callback = std::move(callback)](Selection selection) mutable {
        if (selection.browser_name.isEmpty()) {
          callback({});
          return;
        }
        if (!store) {
          callback({
              .browser_name = selection.browser_name,
              .error = QStringLiteral("Browser file staging is unavailable."),
          });
          return;
        }
        const QString browser_name = selection.browser_name;
        store->stageAsync(
            selection.browser_name, std::move(selection.bytes),
            [callback = std::move(callback), browser_name](BrowserFileStore::StageResult result) mutable {
              callback({
                  .browser_name = browser_name,
                  .input = std::move(result.input),
                  .error = std::move(result.error),
              });
            });
      });
}

}  // namespace PJ
