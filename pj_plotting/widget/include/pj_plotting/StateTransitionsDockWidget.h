#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QDomDocument>
#include <QDomElement>
#include <QWidget>
#include <memory>
#include <vector>

#include "pj_plotting/StateTransitionsController.h"
#include "pj_runtime/IDataWidget.h"
#include "pj_widgets/StateTransitionsView.h"

namespace PJ {

class CatalogModel;
class PlaybackEngine;
class SessionManager;

/// The plot-area State Transitions dock: a StateTransitionsView bound by a
/// StateTransitionsController, packaged as the `IDataWidget` the dock system
/// hosts. Kind string / XML payload tag: `state_transitions`.
///
/// Persistence: each row serializes its stable dataset/topic/field identity (the
/// scene-dock pattern — NOT LayoutXml's plot-curve passes) and is rebound on
/// load via `CatalogModel::resolveCurveKey`. A `<series>` that cannot resolve
/// (its dataset is not loaded) is KEPT and re-serialized on the next save, so a
/// layout round-trip without the data loses nothing; it is not rendered, but it
/// re-resolves automatically as soon as the catalog surfaces its dataset
/// (`itemsAdded`) — the strip's analog of PendingDisplayBinder's flush, so a
/// layout restored mid-load self-heals like plot curves do.
class StateTransitionsDockWidget : public QWidget, public IDataWidget {
  Q_OBJECT
 public:
  StateTransitionsDockWidget(
      SessionManager* session, CatalogModel* catalog, PlaybackEngine* playback, QWidget* parent = nullptr);

  // --- IDataWidget ---
  QWidget* widget() override {
    return this;
  }
  void onTrackerTime(double time) override;
  /// Accepts every dropped key that is a discrete series; true if any row was
  /// added. The DockWidget routes curve-list drops on a mounted strip (and the
  /// auto-create seed) through this.
  bool tryAcceptSeriesKeys(const QStringList& catalog_keys) override;
  QDomElement xmlSaveState(QDomDocument& doc) const override;
  bool xmlLoadState(const QDomElement& element) override;

  [[nodiscard]] StateTransitionsView* view() noexcept {
    return view_;
  }
  [[nodiscard]] StateTransitionsController* controller() noexcept {
    return controller_.get();
  }

 signals:
  /// The widget's persisted state changed through a user gesture (series
  /// added/removed, view chrome committed). The shell debounces this into a
  /// workspace-undo snapshot — the same route the scene docks use.
  void workspaceChanged();

 private:
  /// One saved-but-unresolved series (dataset not currently loaded): the raw
  /// identity attributes, preserved verbatim across save/load cycles.
  struct PendingSeries {
    QString dataset_source;
    QString dataset_path;
    qint64 dataset_id = 0;
    QString topic;
    QString field;
    bool visible = true;
  };

  /// Resolve one saved identity and materialize its row (restoring the eye
  /// state). False when the catalog cannot resolve it (yet).
  bool tryResolveSeries(const PendingSeries& saved);
  /// Drains pending_series_ entries that now resolve; connected to the
  /// catalog's itemsAdded so late-loading datasets bind their rows.
  void retryPendingSeries();
  /// Restore the saved zoom / pan / vertical offset (zoom first so the pan
  /// maps under it).
  void applyViewChrome(const QDomElement& element);

  StateTransitionsView* view_ = nullptr;
  std::unique_ptr<StateTransitionsController> controller_;
  CatalogModel* catalog_ = nullptr;
  std::vector<PendingSeries> pending_series_;
};

}  // namespace PJ
