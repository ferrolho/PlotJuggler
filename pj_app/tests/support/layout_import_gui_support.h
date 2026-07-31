#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Shared scaffolding for the three MainWindow layout-import GUI test
// binaries (alive / policy / cancel): the friend test peer, the
// source-bound layout-file builder, the diagnostic recorder, and the common
// main() macro. The descriptor-scripted fake provider itself is the shared
// tests/support/fake_import_provider.h. One MainWindow per binary (its dtor
// leaks process state), so each scenario lives in its own target; only the
// verbatim scaffolding lives here.

#include <gtest/gtest.h>

#include <QApplication>
#include <QDomDocument>
#include <QDomElement>
#include <QFile>
#include <QObject>
#include <QStandardPaths>
#include <QString>
#include <QStringList>

#include "LayoutImportBatch.h"
#include "LayoutXml.h"
#include "MainWindow.h"
#include "PendingDisplayBinder.h"
#include "fake_import_provider.h"
#include "pj_marketplace/qt_diagnostic_bridge.hpp"
#include "pj_plotting/PlotWidget.h"
#include "pj_runtime/AppSession.h"
#include "pj_runtime/ExtensionCatalogService.h"

namespace PJ {

// Reaches the private restore surfaces of MainWindow for the layout-import
// GUI scenarios: the explicit-interactivity load entry, the progressive
// in-flight flag, the batch handle, and the pending binder state.
class MainWindowLayoutImportTestPeer {
 public:
  [[nodiscard]] static AppSession& session(MainWindow& window) {
    return *window.session_;
  }
  static void loadLayout(MainWindow& window, const QString& path, bool interactive) {
    window.loadLayoutFromPath(
        path, interactive ? MainWindow::LayoutLoadInteractivity::kInteractive
                          : MainWindow::LayoutLoadInteractivity::kAutomated);
  }
  [[nodiscard]] static bool progressiveInFlight(const MainWindow& window) {
    return window.progressive_layout_in_flight_;
  }
  [[nodiscard]] static LayoutImportBatch* batch(MainWindow& window) {
    return window.layout_import_batch_.get();
  }
  [[nodiscard]] static bool batchActive(const MainWindow& window) {
    return window.layoutImportBatchActive();
  }
  // True when the pending binder holds NO retained intents.
  [[nodiscard]] static bool binderEmpty(MainWindow& window) {
    return window.pending_binder_ == nullptr || window.pending_binder_->empty();
  }
  [[nodiscard]] static int plotCount(MainWindow& window) {
    int count = 0;
    window.forEachPlot([&count](PlotWidget* /*plot*/) { ++count; });
    return count;
  }
  [[nodiscard]] static PlotWidget* firstPlot(MainWindow& window) {
    PlotWidget* found = nullptr;
    window.forEachPlot([&found](PlotWidget* plot) {
      if (found == nullptr) {
        found = plot;
      }
    });
    return found;
  }
  [[nodiscard]] static QtDiagnosticBridge* diagnosticBridge(MainWindow& window) {
    return window.diagnostic_bridge_;
  }
  [[nodiscard]] static int restoreWaiterCount(const MainWindow& window) {
    return static_cast<int>(window.restore_waiters_.size());
  }
  [[nodiscard]] static int totalCurveCount(MainWindow& window) {
    int count = 0;
    window.forEachPlot([&count](PlotWidget* plot) { count += static_cast<int>(plot->curveList().size()); });
    return count;
  }
};

}  // namespace PJ

namespace pj_layout_import_gui {

// The shared fake provider, re-exported so the GUI tests keep addressing it
// through this namespace.
using pj_fake_import::g_instance;
using pj_fake_import::kFakeVtable;
using pj_fake_import::kProviderId;

// GUI-scenario descriptor: name/estimate are irrelevant here.
[[nodiscard]] inline QString makeDescriptor(const QString& trust, const QString& path, const QString& import_mode) {
  return pj_fake_import::makeDescriptor(QStringLiteral("gui"), trust, path, 0, import_mode);
}

// One plot with a topic-addressed (unresolved) curve plus one
// materialize-bearing <fileInfo> whose descriptor scripts the fake.
// `provider` overrides the <materialize provider> attribute (an unknown id
// drives the §6.4 provider-absent fallback).
[[nodiscard]] inline QDomDocument buildImportLayoutDoc(
    const QString& curve_topic, const QString& source_path, const QString& descriptor,
    const QString& provider = QString::fromUtf8(kProviderId)) {
  QDomDocument doc;
  QDomElement root = doc.createElement(QStringLiteral("root"));
  root.setAttribute(QStringLiteral("pj4_version"), QStringLiteral("4"));
  root.setAttribute(QStringLiteral("binding"), QStringLiteral("source"));
  doc.appendChild(root);

  QDomElement tabbed = doc.createElement(QStringLiteral("tabbed_widget"));
  tabbed.setAttribute(QStringLiteral("parent"), QStringLiteral("main_window"));
  QDomElement tab = doc.createElement(QStringLiteral("Tab"));
  tab.setAttribute(QStringLiteral("id"), QStringLiteral("t1"));
  tab.setAttribute(QStringLiteral("containers"), QStringLiteral("1"));
  QDomElement container = doc.createElement(QStringLiteral("Container"));
  QDomElement dock_area = doc.createElement(QStringLiteral("DockArea"));
  dock_area.setAttribute(QStringLiteral("id"), QStringLiteral("a1"));
  dock_area.setAttribute(QStringLiteral("name"), QStringLiteral("View"));
  QDomElement plot = doc.createElement(QStringLiteral("plot"));
  plot.setAttribute(QStringLiteral("id"), QStringLiteral("plot1"));
  plot.setAttribute(QStringLiteral("mode"), QStringLiteral("TimeSeries"));
  QDomElement curve = doc.createElement(QStringLiteral("curve"));
  // Stable topic identity only — no concrete key: the curve resolves (or is
  // retained as a pending intent) against whatever the import produces.
  curve.setAttribute(QStringLiteral("topic"), curve_topic);
  plot.appendChild(curve);
  dock_area.appendChild(plot);
  container.appendChild(dock_area);
  tab.appendChild(container);
  tabbed.appendChild(tab);
  root.appendChild(tabbed);

  QDomElement wrapper = doc.createElement(QStringLiteral("previouslyLoaded_Datafiles"));
  QDomElement file_info = doc.createElement(QStringLiteral("fileInfo"));
  file_info.setAttribute(QStringLiteral("filename"), source_path);
  QDomElement materialize = doc.createElement(QStringLiteral("materialize"));
  materialize.setAttribute(QStringLiteral("provider"), provider);
  materialize.setAttribute(QStringLiteral("identity"), QStringLiteral("id-1"));
  PJ::layout_xml::appendJsonAsCdata(doc, materialize, descriptor);
  file_info.appendChild(materialize);
  wrapper.appendChild(file_info);
  root.appendChild(wrapper);
  return doc;
}

[[nodiscard]] inline bool writeLayoutFile(const QString& path, const QDomDocument& doc) {
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly)) {
    return false;
  }
  file.write(doc.toByteArray(2));
  return true;
}

// The buildImportLayoutDoc + writeLayoutFile nest every scenario starts with.
[[nodiscard]] inline bool writeImportLayout(
    const QString& layout_path, const QString& curve_topic, const QString& source_path, const QString& descriptor,
    const QString& provider = QString::fromUtf8(kProviderId)) {
  return writeLayoutFile(layout_path, buildImportLayoutDoc(curve_topic, source_path, descriptor, provider));
}

// Records every diagnostic id the window reports into `ids` (delivery is
// queued — pump before asserting).
inline void recordDiagnosticIds(PJ::MainWindow& window, QStringList& ids) {
  QObject::connect(
      PJ::MainWindowLayoutImportTestPeer::diagnosticBridge(window), &PJ::QtDiagnosticBridge::diagnosticReported,
      &window, [&ids](int /*level*/, const QString& /*source*/, const QString& id, const QString& /*message*/) {
        ids.push_back(id);
      });
}

}  // namespace pj_layout_import_gui

// The common GUI-test main(): offscreen platform unless overridden,
// test-mode QStandardPaths, one QApplication.
#define PJ_MAIN_WINDOW_TEST_MAIN                         \
  int main(int argc, char** argv) {                      \
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) { \
      qputenv("QT_QPA_PLATFORM", "offscreen");           \
    }                                                    \
    QStandardPaths::setTestModeEnabled(true);            \
    QApplication app(argc, argv);                        \
    ::testing::InitGoogleTest(&argc, argv);              \
    return RUN_ALL_TESTS();                              \
  }
