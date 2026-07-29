#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QByteArray>
#include <QMimeData>
#ifdef PJ_TARGET_WASM
#include <QPointer>
#endif
#include <QSet>
#include <QStringList>
#include <QTreeWidget>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace PJ {

class HeaderResizePolicy;

// Hierarchical tree of curves with two columns (name, value-at-tracker).
// Drag source emits "curveslist/add_curve" (left drag) or
// "curveslist/new_XY_axis" (right drag of exactly two curves).
class CurveTreeView : public QTreeWidget {
  Q_OBJECT
 public:
  using DragSelectionProvider = std::function<std::vector<QString>()>;

  struct CurvePath {
    QString key;
    QString dataset;
    QString topic;
    QString field;
    bool selectable = true;
    // false = the row never starts a drag and is excluded from any drag payload.
    // Curve leaves: a value-only leaf (string fields today — they can't be
    // plotted) keeps its Value cell but is drag-inert. Object topics: a type no
    // view can display (the caller's policy) — pair with `tooltip` to say why.
    bool draggable = true;
    // Optional name-cell tooltip. Empty = none.
    QString tooltip = {};
    bool is_image_topic = false;
    bool is_3d_object_topic = false;
    // An advertised-but-unsubscribed placeholder (a streaming source's topic with
    // no data yet, no storage id) — rendered "paused/ghost" (dimmed) by
    // CurveTreeItemDelegate, still selectable/draggable so it can be dropped to
    // register demand (see the pj_app-side TopicDemandController).
    bool is_placeholder = false;
    // A topic that HAS data but is not currently referenced by any displayed
    // widget on a per-topic-pause-capable dataset — dimmed the same way as
    // is_placeholder. Computed at construction time from
    // TopicDemandTracker::activeTopics(); kept live afterward via
    // setUnsubscribedKeys() rather than a rebuild, so a subscribe/unsubscribe
    // toggle only repaints, never restructures.
    bool is_unsubscribed = false;
  };

  // Hierarchical: split dataset/topic/field on every '/' (after '.' → '/').
  // ShowTopics: dataset and topic stay as literal nodes (topic shown
  // verbatim, e.g. "/camera/image"); the field still splits on '/' so
  // nested struct fields show up as sub-folders below the topic.
  enum class ViewMode { kHierarchical, kShowTopics };

  explicit CurveTreeView(QWidget* parent = nullptr);

  void setViewMode(ViewMode mode);
  [[nodiscard]] ViewMode viewMode() const {
    return view_mode_;
  }

  [[nodiscard]] static QString catalogItemsMimeType();
  // Mime format set on a right-drag of exactly two curves — the "create XY plot"
  // gesture. Present alongside catalogItemsMimeType() so drop sites can detect it.
  [[nodiscard]] static QString newXyAxisMimeType();
  [[nodiscard]] static QByteArray encodeCatalogKeys(const QStringList& keys);
  [[nodiscard]] static QStringList decodeCatalogKeys(const QMimeData* mime_data);

  void addCurve(const QString& name);
  void addCurves(const std::vector<QString>& names);
  void addCurve(const CurvePath& path);
  void addCatalogItem(const CurvePath& path);
  void addCatalogItems(const std::vector<CurvePath>& paths);
  // Builds the hierarchical tree-path a row is filed under and searched by
  // (dataset/topic/field, '.'→'/' normalized) — the exact string addCatalogItem
  // stores in the row's search role. Exposed so a caller can name a row by its
  // tree location, e.g. to pre-arm requestExpansionWhenPromoted for a
  // placeholder that will later promote to a field-bearing group.
  [[nodiscard]] static QString treePathFromCurvePath(const CurvePath& path);
  void clearCurves();
  // Expanded-group snapshot for rebuild survival: a full rebuild (clearCurves +
  // re-add, e.g. after a catalog item removal) would otherwise collapse the
  // whole tree. Capture before, restore after. Paths are name chains joined
  // with a control character (names may contain '/'); restore only ADDS
  // expansions — vanished paths are skipped and nothing is collapsed, so it
  // composes with an active filter's own auto-expansion.
  [[nodiscard]] QStringList expandedGroupPaths() const;
  void restoreExpandedGroupPaths(const QStringList& paths);
  // Updates the "unsubscribed" (dimmed) flag on every on-screen row whose
  // catalog key is in `unsubscribed_keys`, clearing it on every other row — a
  // full-set replace, mirroring TopicDemandTracker's own declarative active-set
  // semantics. No structural change (no rebuild), so it is cheap to call after
  // every TopicDemandTracker::activeTopicsChanged.
  void setUnsubscribedKeys(const QSet<QString>& unsubscribed_keys);
  // Reads back what setUnsubscribedKeys (or CurvePath::is_unsubscribed at
  // construction) set for `key`; false for an unknown key.
  [[nodiscard]] bool isKeyUnsubscribed(const QString& key) const;
  // The catalog key a row resolves to (curve leaf, object-topic terminal, or
  // placeholder), or empty for pure group/folder rows — lets a context-menu
  // host map the clicked row back to a CatalogModel item.
  [[nodiscard]] static QString catalogKeyOf(const QTreeWidgetItem* item);
  // Every catalog key in `item`'s subtree, including `item`'s own (depth-first;
  // keyless group rows contribute nothing). Lets a context-menu host act on a
  // GROUP row — e.g. a promoted scalar topic, whose keys live on its field
  // leaves, not the topic node itself.
  [[nodiscard]] static QStringList catalogKeysUnder(const QTreeWidgetItem* item);
  // Full-set replace of the topics whose streaming is user-forced, each named
  // by its topic tree-path (dataset/topic, '.'→'/' — see treePathFromCurvePath
  // with an empty field). The TOPIC node's name paints in the accent blue.
  // The set is retained and re-applied across rebuilds.
  void setForcedTopicPaths(const QSet<QString>& topic_paths);
  // Test read-back: whether the topic node at `topic_path` carries the forced
  // mark right now.
  [[nodiscard]] bool isTopicPathForced(const QString& topic_path);
  // Arms a one-shot intent: once a placeholder leaf at `tree_path` promotes to a
  // field-bearing topic (its fields materialize as child rows), expand that
  // topic's node and its ancestors so the field breakdown is revealed, then
  // forget the intent. Fires at most once, so a later manual collapse survives
  // subsequent rebuilds; the intent itself survives rebuilds until honored.
  // `tree_path` is the row's normalized search path (dataset/topic/field, '.'→'/'
  // — see treePathFromCurvePath); matching keys on that search role, so it works
  // in both the hierarchical and show-topics views. A promotion to a single
  // unnamed field (no sub-path) reveals nothing to expand and is a no-op.
  void requestExpansionWhenPromoted(const QString& tree_path);
  void applyFilter(const QString& filter);
  // Restrict which topic KINDS the tree shows, ANDed with the text filter.
  // Classification is per TOPIC and governs the topic's whole subtree: a Scene2D
  // (image-family, kImageTopicRole) or Scene3D (3D-object, k3dObjectTopicRole)
  // topic — and every scalar field nested under it — is hidden together when
  // that kind is off; those fields count as the topic's scene kind, never as
  // Plot. Plot is the "by exclusion" bucket: a topic carrying no scene marker
  // anywhere above the row (a plain numeric topic and its fields). All three
  // default to true (no type restriction).
  void setVisibleCurveKinds(bool show_plot, bool show_scene2d, bool show_scene3d);
  // Message shown as a muted, column-spanning child row under each dataset node
  // whose topics are ALL filtered out (by text and/or setVisibleCurveKinds): the
  // dataset name stays visible and the row explains the blank instead of the
  // whole panel going empty. Empty string (default) disables it; a dataset with
  // no topics at all shows nothing.
  void setEmptyFilterMessage(const QString& message);
  void refreshIcons(const QString& theme);
  std::vector<QString> selectedCurveNames() const;
  // selectedCurveNames() returns only directly-selected leaves; this variant
  // expands selected group nodes to all their leaf descendants. Result is
  // sorted and deduplicated.
  std::vector<QString> selectedCurveNamesRecursive() const;
  // Returns catalog item keys for selected nodes, including object-topic
  // branch nodes. Scalar-only curve selection remains available through
  // selectedCurveNamesRecursive(). COMPLETE: not-draggable object rows are
  // included — deletion and selection-size logic must see the whole selection
  // (an empty result means "nothing selected", which some callers widen to
  // "everything"). Drag payloads use selectedCatalogKeysForDrag() instead.
  std::vector<QString> selectedCatalogKeysRecursive() const;
  // Drag-payload variant of selectedCatalogKeysRecursive(): not-draggable object
  // rows (CurvePath::draggable=false) are excluded; when `skipped_keys` is
  // given, their keys are appended to it so the caller can tell the user what
  // was left out.
  std::vector<QString> selectedCatalogKeysForDrag(QStringList* skipped_keys = nullptr) const;

  // Builds the MIME payload for a drag of the current selection: the
  // "curveslist/add_curve" / "curveslist/new_XY_axis" curve-name format and
  // the catalog-key format, each covering EVERY selected row (not just the row
  // under the cursor). Returns nullptr — and transfers ownership otherwise —
  // when the selection has nothing draggable for `button`. Exposed for tests
  // because the desktop live drag path ends in a blocking QDrag::exec() that
  // cannot be driven from a unit test. WASM reuses this payload with a
  // non-blocking in-app dispatcher (see the wasmDrag* helpers below), which is
  // covered by the browser (Playwright) suite rather than these native tests.
  // `skipped_keys` (optional) collects the not-draggable object rows the payload
  // excluded — see selectedCatalogKeysForDrag.
  [[nodiscard]] QMimeData* createDragMimeData(Qt::MouseButton button, QStringList* skipped_keys = nullptr) const;

  void setValuesColumnHidden(bool hidden);
  bool valuesColumnHidden() const {
    return isColumnHidden(1);
  }

  // Install/refresh the value provider and repaint column 1 ("Value") for the
  // on-screen scalar leaves. Rows whose row rect falls outside the viewport are
  // skipped, so a huge catalog formats only the visible handful. No-op when the
  // value column is hidden. `value_provider` receives each visible leaf's catalog
  // key (see catalogKeyForItem) and returns the preformatted cell text (empty
  // string for rows with no value / non-scalar rows). Non-leaf group rows are
  // never touched. The provider is retained, so the view re-applies it on its own
  // whenever the visible set changes (expand/collapse/scroll/resize) — the caller
  // only re-invokes this when the underlying values change (e.g. tracker moved).
  // Lifetime: re-application is deferred (QTimer::singleShot), so anything the
  // provider captures must stay valid until a new provider is installed, the
  // value column is hidden, or the view is destroyed.
  void refreshVisibleValues(const std::function<QString(const QString& key)>& value_provider);

  void setDragSelectionProvider(DragSelectionProvider provider);

 signals:
  // Emitted on a double-click of a childless, peek-eligible scalar placeholder
  // leaf: an advertised-but-unsubscribed row that is NOT an image/3D-object
  // terminal. The host (pj_app) responds by starting a bounded preview
  // subscription so one real sample lands and the placeholder promotes to
  // per-field rows. Carries the row's catalog key.
  void placeholderPeekRequested(const QString& catalog_key);
  // Emitted at most once per press gesture when the user pulls past the drag
  // threshold on a not-draggable object-topic row (CurvePath::draggable=false) —
  // the drag never starts, and without this notice the gesture would fail
  // silently. `reason` is the row's tooltip (may be empty; the host supplies a
  // fallback wording).
  void dragAttemptedOnNotDraggableRow(const QString& reason);
  // Emitted when a drag that DID start had to exclude not-draggable object rows
  // from its multi-selection payload, so the host can tell the user why fewer
  // topics arrive than were selected. Fires when the drag gesture ENDS (after
  // the QDrag loop returns; on WASM when the drop is delivered) — a toast
  // raised mid-drag could sit over the drop target and steal its hit test.
  void dragPayloadKeysSkipped(const QStringList& catalog_keys);

 protected:
#ifdef PJ_TARGET_WASM
  bool event(QEvent* event) override;
#endif
  void mousePressEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;

 private:
  enum class SortMode { kImmediate, kDeferred };

  void addCurve(const QString& name, SortMode sort_mode);
  void addCatalogItem(const CurvePath& path, SortMode sort_mode);
  // The node representing the topic at `topic_path`: the row whose search role
  // equals it (placeholder leaf / object terminal), or the group above its
  // field leaves (promoted scalar topic). Null when nothing matches.
  QTreeWidgetItem* findTopicNode(const QString& topic_path);
  // Re-stamps kForcedRole from forced_topic_paths_ (clear-all then set).
  void applyForcedTopicMarks();
  QTreeWidgetItem* ensureGroupSegments(const QStringList& segments);
  QTreeWidgetItem* ensureGroup(const QString& path);
  // Walks the tree; for every pending_expand_paths_ entry whose group node now
  // exists, expands that node and its ancestors and drops the entry. No-op when
  // there are no pending intents.
  void expandPendingGroups();
  // Repaint visible value cells now, using the retained provider (no-op if none
  // / column hidden). scheduleValueRefresh() coalesces this onto the next event
  // loop turn — used after expand/collapse/scroll/resize so the freshly-laid-out
  // rows have valid rects before we read them.
  void applyVisibleValues();
  // Recursive worker for applyVisibleValues: writes the value cell for `item` if
  // it's an on-screen scalar leaf, else recurses into its children. A member
  // (not a per-call std::function) so the ≤10 Hz refresh allocates nothing.
  void applyVisibleValuesToSubtree(QTreeWidgetItem* item, int viewport_height);
  void scheduleValueRefresh();
  void sortTree();
  // Re-run the active filter (last_filter_) over the whole tree, hiding or showing
  // each row. Unlike applyFilter() it has no text-equality short-circuit, so it
  // also re-evaluates rows inserted since the filter was last set.
  void refilterTree();
  // Re-assert the active filter after rows are inserted, so a filter typed before
  // the data arrived still applies to it. No-op when no filter is active (freshly
  // inserted rows are visible by default).
  void reapplyFilter();
  // Show/hide the managed empty-filter placeholder under `dataset_node`: a single
  // italic, muted, column-spanning child row, displayed (with the node force-shown
  // and expanded) when `subtree_hidden` and the dataset actually has topics — so a
  // fully-filtered dataset keeps its name and explains the blank. See
  // setEmptyFilterMessage. No-op when the message is empty.
  void updateEmptyMessageChild(QTreeWidgetItem* dataset_node, bool subtree_hidden);
  void setDescendantsExpanded(QTreeWidgetItem* item, bool expanded);
  std::vector<QString> selectedCurveNamesForDrag() const;
  // Shared walk behind selectedCatalogKeysRecursive (complete) and
  // selectedCatalogKeysForDrag (not-draggable rows excluded and reported).
  std::vector<QString> collectSelectedCatalogKeys(bool exclude_not_draggable, QStringList* skipped_keys) const;
  // The one drag-start threshold, measured from drag_start_pos_ — used by both
  // the real drag arming and the not-draggable notice so they can never disagree.
  [[nodiscard]] bool pastDragThreshold(const QPoint& pos) const;
#ifdef PJ_TARGET_WASM
  // QDrag::exec() needs Qt WASM's Asyncify build because it enters a nested
  // event loop. The production browser build deliberately avoids Asyncify, so
  // keep the mouse grab established by the press and dispatch the ordinary Qt
  // DnD events to the widget under the pointer instead. Drop sites therefore
  // share their exact MIME validation and mutation paths with desktop. These
  // helpers are exercised by the browser (Playwright) suite, not native tests.
  void beginWasmDrag(QMimeData* mime_data, QMouseEvent* event, QStringList skipped_keys);
  void updateWasmDrag(const QPoint& global_pos, Qt::MouseButtons buttons, Qt::KeyboardModifiers modifiers);
  void finishWasmDrag(QMouseEvent* event);
  void cancelWasmDrag();
  void abortWasmDrag(Qt::KeyboardModifiers modifiers);
  void balanceWasmSourcePress(Qt::MouseButton button, Qt::KeyboardModifiers modifiers);
#endif

  QPoint drag_start_pos_;
  Qt::MouseButton drag_button_ = Qt::NoButton;
  std::vector<QString> drag_curve_names_;
  QStringList drag_catalog_keys_;
  bool suppress_next_release_ = false;
  // One-shot dragAttemptedOnNotDraggableRow state, armed (with the row's tooltip as the
  // reason) by a left press on a not-draggable object row and consumed by the
  // first past-threshold move of that gesture. Shares drag_start_pos_ with the
  // real drag arming — the two are mutually exclusive per press.
  std::optional<QString> not_draggable_reason_;
#ifdef PJ_TARGET_WASM
  std::unique_ptr<QMimeData> wasm_drag_mime_;
  QPointer<QWidget> wasm_drag_target_;
  Qt::MouseButton wasm_drag_button_ = Qt::NoButton;
  // Not-draggable keys excluded from the in-flight WASM drag's payload; emitted as
  // dragPayloadKeysSkipped when the drop is delivered (finishWasmDrag) and
  // dropped silently on cancel/abort.
  QStringList wasm_drag_skipped_keys_;
  // Reentrancy guard for the synchronous drop dispatch: sendEvent into the drop
  // target runs the drop site's handler inline (e.g. adding a curve rebuilds
  // this very tree via clearCurves), which can loop back into finishWasmDrag /
  // beginWasmDrag on the same stack. INVARIANT: while a drop is in flight, no
  // new drag starts and no nested drop dispatches — those re-entries early-out.
  bool in_wasm_drop_ = false;
#endif
  QString last_filter_;
  // setVisibleCurveKinds flags; all true = no type restriction (the default).
  bool show_plot_ = true;
  bool show_scene2d_ = true;
  bool show_scene3d_ = true;
  // See setEmptyFilterMessage. Empty string disables the overlay.
  QString empty_filter_message_;
  DragSelectionProvider drag_selection_provider_;
  // Owned by the header; borrowed here to rebalance after the Value column is
  // shown or hidden, which the policy cannot observe on its own.
  HeaderResizePolicy* header_policy_ = nullptr;
  ViewMode view_mode_ = ViewMode::kHierarchical;
  // Retained value-column provider (see refreshVisibleValues). Re-applied on
  // visibility changes so expanding/scrolling fills the newly-revealed rows.
  std::function<QString(const QString&)> value_provider_;
  // Coalesces the deferred re-apply so a burst of expand/scroll events schedules
  // a single refresh on the next event loop turn.
  bool value_refresh_scheduled_ = false;
  // Topics currently marked as force-streamed (see setForcedTopicPaths);
  // retained so rebuilds re-stamp the marks.
  QSet<QString> forced_topic_paths_;
  // One-shot auto-expand intents keyed by tree-path (see
  // requestExpansionWhenPromoted). Survives rebuilds; each entry is erased the
  // first time a matching group node is expanded.
  QSet<QString> pending_expand_paths_;
};

// Format a scalar for the curve-list "Value" column, PlotJuggler-3 style: fixed
// `precision` decimals, then trailing zeros (and a bare trailing '.') overwritten
// with spaces with one space appended — so a right-aligned monospace column keeps
// every decimal point in the same place (e.g. 1.2 -> "1.2   ", 5 -> "5     ",
// -0.001 -> "-0.001 "). Non-finite values (NaN/inf) render as "-".
[[nodiscard]] QString formatScalarForColumn(double value, int precision);

}  // namespace PJ
