// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_marketplace/marketplace_window.hpp"

#include <QColor>
#include <QDesktopServices>
#include <QDialog>
#include <QEvent>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QSettings>
#include <QSignalBlocker>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QStyledItemDelegate>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTextBrowser>
#include <QTimer>
#include <QVBoxLayout>
#include <QWindow>
#include <algorithm>
#include <utility>

#include "pj_marketplace/download_manager.hpp"
#include "pj_marketplace/extension_manager.hpp"
#include "pj_marketplace/platform_utils.hpp"
#include "pj_marketplace/registry_manager.hpp"
#include "pj_marketplace/version_compare.hpp"
#include "pj_widgets/CheckButton.h"
#include "pj_widgets/ChromeMetrics.h"
#include "pj_widgets/ElidingLabel.h"
#include "pj_widgets/FileDialog.h"
#include "pj_widgets/FrameworkTokens.h"
#include "pj_widgets/MessageBox.h"
#include "pj_widgets/Scrollbar.h"
#include "pj_widgets/Search.h"
#include "pj_widgets/ToggleSwitch.h"
#include "ui_extension_detail_dialog.h"
#include "ui_marketplace_window.h"
using namespace Qt::StringLiterals;

namespace PJ {

namespace {

// Column order of the plugin table (see setupUi / rebuildTable).
enum Column {
  kColName = 0,
  kColCategory,            // the registry `category` value (data_loader, toolbox, …)
  kColInstalledVersion,    // the installed version, or "—" when not installed
  kColMarketplaceVersion,  // the registry version; highlighted when it's an update
  kColDescription,
  kColCount,
};

// Per-item string on the Category column carrying the row's plugin name, so
// CategoryItem::operator< can break ties alphabetically WITHIN a category
// (see the class comment). Stored via setData at row insertion time in
// rebuildTable().
constexpr int kCategoryNameKeyRole = Qt::UserRole + 2;

// Sort compound for the Category column: primary key = category text,
// secondary key = plugin name (case-insensitive). Grouping by category with an
// alphabetical run inside each group is the PJ3-era default sort Davide asked
// to restore. Applied uniformly for both ascending and descending — descending
// flips both keys (reverse-category, then reverse-name within), which keeps
// Category a single toggle rather than a stateful multi-dimension picker.
class CategoryItem : public QTableWidgetItem {
 public:
  using QTableWidgetItem::QTableWidgetItem;
  [[nodiscard]] bool operator<(const QTableWidgetItem& other) const override {
    const int cat_cmp = QString::compare(text(), other.text(), Qt::CaseInsensitive);
    if (cat_cmp != 0) {
      return cat_cmp < 0;
    }
    return QString::compare(
               data(kCategoryNameKeyRole).toString(), other.data(kCategoryNameKeyRole).toString(),
               Qt::CaseInsensitive) < 0;
  }
};

// Sort item for the two version columns (Installed / Marketplace). Plain
// QTableWidgetItem sorts its display text lexicographically, which orders
// "10.0.0" before "9.0.0"; compareSemver gives the numeric ordering the columns
// need. The em-dash placeholder for a not-installed row (empty semver) sorts
// below every real version.
class VersionItem : public QTableWidgetItem {
 public:
  using QTableWidgetItem::QTableWidgetItem;
  [[nodiscard]] bool operator<(const QTableWidgetItem& other) const override {
    return compareSemver(semver(text()), semver(other.text())) < 0;
  }

 private:
  // Maps the em-dash placeholder to an empty string, which compareSemver treats
  // as numeric zero — so uncategorized/not-installed rows land at the bottom.
  static std::string semver(const QString& display) {
    return display == u"—"_s ? std::string{} : display.toStdString();
  }
};

// Row-height floor: a comfortable minimum so rows read as clearly separated
// even when their text is short.
constexpr int kMinRowHeight = 44;

// Per-item fill QColor for the Marketplace-version cell highlight, or an invalid
// QColor for no highlight (set in rebuildTable, read by MarketplaceCellDelegate).
// The colour differs by reason — Destructive pink for an available update,
// Emphasis amber for an incompatible plugin.
constexpr int kHighlightColorRole = Qt::UserRole + 1;

// Paints the Marketplace-version cell in a per-item fill colour. A delegate is
// required because the app-wide QSS rule `QTableView::item { background-color: … }`
// unconditionally overrides any per-item setBackground(), so the highlight has to
// be drawn here (the delegate owns the cell's paint) rather than via item brushes.
class MarketplaceCellDelegate : public QStyledItemDelegate {
 public:
  MarketplaceCellDelegate(QColor fg, QObject* parent) : QStyledItemDelegate(parent), fg_(std::move(fg)) {}

  void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
    const QColor fill = index.data(kHighlightColorRole).value<QColor>();
    const bool selected = (option.state & QStyle::State_Selected) != 0;
    if (!fill.isValid() || selected) {
      QStyledItemDelegate::paint(painter, option, index);  // normal cells: let the QSS style paint
      return;
    }
    // Highlighted, unselected: fill the per-item background and draw the text
    // ourselves (calling the base would let the QSS repaint the cell backdrop
    // over our fill).
    painter->save();
    painter->fillRect(option.rect, fill);
    painter->setPen(fg_);
    const QRect text_rect = option.rect.adjusted(6, 0, -6, 0);
    // Respect the item's own alignment (e.g. the centered version columns);
    // fall back to left-aligned when the item sets none.
    const QVariant item_align = index.data(Qt::TextAlignmentRole);
    const int align = item_align.isValid() ? item_align.toInt() : (Qt::AlignLeft | Qt::AlignVCenter);
    painter->drawText(text_rect, align | Qt::TextWordWrap, index.data().toString());
    painter->restore();
  }

 private:
  QColor fg_;
};

bool installedStatesEqual(const QMap<QString, InstalledExtension>& lhs, const QMap<QString, InstalledExtension>& rhs) {
  if (lhs.size() != rhs.size()) {
    return false;
  }
  for (auto it = lhs.cbegin(); it != lhs.cend(); ++it) {
    const auto rhs_it = rhs.find(it.key());
    if (rhs_it == rhs.cend()) {
      return false;
    }
    const InstalledExtension& a = it.value();
    const InstalledExtension& b = rhs_it.value();
    if (a.id != b.id || a.version != b.version || a.enabled != b.enabled) {
      return false;
    }
  }
  return true;
}

}  // namespace

MarketplaceWindow::MarketplaceWindow(const QUrl& registry_url, QWidget* parent)
    : Dialog(parent), ui_(new Ui::MarketplaceWindow) {
  download_mgr_ = new DownloadManager(this);
  registry_mgr_ = new RegistryManager(this);
  ext_mgr_ = new ExtensionManager(
      download_mgr_, PlatformUtils::extensionsDir(), PlatformUtils::pendingDir(), /*sink*/ {}, this);
  registry_url_ = registry_url;
  // applyPendingUninstalls/applyPendingInstalls already ran in ExtensionManager::initComponents().
  finishConstruction(nullptr);
}

MarketplaceWindow::MarketplaceWindow(ExtensionManager* ext_mgr, const QUrl& registry_url, QWidget* parent)
    : Dialog(parent), ui_(new Ui::MarketplaceWindow) {
  registry_mgr_ = new RegistryManager(this);
  ext_mgr_ = ext_mgr;
  registry_url_ = registry_url;
  finishConstruction(nullptr);
}

MarketplaceWindow::MarketplaceWindow(
    ExtensionManager* ext_mgr, const QUrl& registry_url, const QMap<QString, InstalledExtension>& installed,
    QWidget* parent)
    : Dialog(parent), ui_(new Ui::MarketplaceWindow) {
  registry_mgr_ = new RegistryManager(this);
  ext_mgr_ = ext_mgr;
  initial_snapshot_provided_ = true;
  registry_url_ = registry_url;
  finishConstruction(&installed);
}

void MarketplaceWindow::finishConstruction(const QMap<QString, InstalledExtension>* installed) {
  setupUi();
  setupSignals();
  if (installed != nullptr) {
    ext_mgr_->setInstalledExtensions(*installed);
  }
  updateDiagnosticsButton();
  showLatestDiagnostic();
  registry_mgr_->fetchRegistry(registry_url_);
}

MarketplaceWindow::~MarketplaceWindow() {
  delete ui_;
}

// ─── UI Setup ────────────────────────────────────────────────────────────────

void MarketplaceWindow::setupUi() {
  // Canonical chrome: build the .ui onto a child body under PJ::Dialog's title
  // bar (its content area already owns a zero-margin layout).
  setDialogTitle(tr("PlotJuggler Marketplace"));
  auto* body = new QWidget;
  ui_->setupUi(body);
  contentLayout()->addWidget(body);
  content_widget_ = body;  // exposed via contentWidget() for embedding

  ui_->update_all_btn_->setFixedWidth(90);
  ui_->update_all_btn_->setEnabled(false);

  // Pin the toolbar-row pill height to the Install/Update button post-QSS
  // rendered size so the strip reads as one row. QPushButton grows via the QSS
  // `padding: ${space_comfortable}` which doesn't feed sizeHint on QStyleSheet
  // widgets (padding without a border), so the actual height is only known
  // after the first layout pass — deferred to the next event-loop tick.
  QMetaObject::invokeMethod(
      this,
      [this]() {
        const int actual_h = ui_->install_local_btn_->height();
        for (CheckButton* pill :
             {ui_->filter_data_loader_, ui_->filter_data_streamer_, ui_->filter_parser_, ui_->filter_toolbox_}) {
          pill->setFixedHeight(actual_h);
        }
      },
      Qt::QueuedConnection);

  // The marketplace doesn't link pj_app_core, so it can't pipe icons
  // through LoadSvg's recolor. Pick the theme-appropriate variant
  // directly from the resource bundle.
  const bool dark_theme = QSettings().value(QStringLiteral("StyleSheet::theme"), QStringLiteral("light")).toString() !=
                          QStringLiteral("light");
  ui_->settings_btn_->setIcon(QIcon(
      dark_theme ? QStringLiteral(":/resources/svg/settings_cog_dark.svg")
                 : QStringLiteral(":/resources/svg/settings_cog_light.svg")));
  // The canonical Search provides the (self-retinting) magnifying glass and a
  // themed clear "x"; it sits on the toolbar surface, so use the standalone tone.
  ui_->search_edit_->setVariant(Search::Variant::kStandalone);

  // Scroll-area background comes from the central stylesheet
  // (#scroll_area_ rule binds it to ${dark_background}).

  connect(ui_->search_edit_, &Search::textChanged, this, &MarketplaceWindow::onSearchChanged);
  // "Compatible" hides host-incompatible plugins and is ON by default (R2). Set
  // it before wiring signals so this initial state emits no toggled/filter pass.
  ui_->filter_compatible_->setChecked(true);
  for (CheckButton* toggle :
       {ui_->filter_compatible_, ui_->filter_installed_, ui_->filter_data_loader_, ui_->filter_data_streamer_,
        ui_->filter_parser_, ui_->filter_toolbox_}) {
    connect(toggle, &CheckButton::toggled, this, &MarketplaceWindow::onFilterToggled);
  }
  connect(ui_->settings_btn_, &QPushButton::clicked, this, &MarketplaceWindow::pluginPreferencesRequested);
  connect(ui_->install_local_btn_, &QPushButton::clicked, this, &MarketplaceWindow::onInstallLocalClicked);
  connect(ui_->update_all_btn_, &QPushButton::clicked, this, &MarketplaceWindow::onUpdateAllClicked);
  connect(ui_->diagnostics_btn_, &QPushButton::clicked, this, &MarketplaceWindow::onDiagnosticsClicked);

  // Plugin table: fixed columns (Name/Installed/Marketplace versions) sized to
  // their content, Description stretches to take the rest. Rows are read-only and
  // whole-row selectable; the Marketplace-version cell is highlighted when it is
  // a newer version than the installed one. All actions — install/update/
  // uninstall and enable/disable — live in the details footer, not in the cells.
  auto* table = ui_->plugin_table_;
  table->setColumnCount(kColCount);
  table->setHorizontalHeaderLabels({tr("Name"), tr("Category"), tr("Installed"), tr("Marketplace"), tr("Description")});
  table->verticalHeader()->setVisible(false);
  table->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table->setSelectionBehavior(QAbstractItemView::SelectRows);
  table->setSelectionMode(QAbstractItemView::SingleSelection);
  // Grid + alternating rows give clear visual separation between plugins; a
  // minimum row height keeps short-text rows from looking cramped.
  table->setShowGrid(true);
  table->setWordWrap(true);
  table->setAlternatingRowColors(true);
  // Sorting is on so the header click reorders rows. We restrict click-to-sort
  // to Name and Category — clicks on other columns are intercepted below and
  // reset back to the previous Name/Category sort. rebuildTable() toggles
  // sortingEnabled around the row insertion.
  table->setSortingEnabled(true);
  table->sortByColumn(kColCategory, Qt::AscendingOrder);
  table->verticalHeader()->setMinimumSectionSize(kMinRowHeight);
  auto* header = table->horizontalHeader();
  header->setSectionResizeMode(kColName, QHeaderView::ResizeToContents);
  header->setSectionResizeMode(kColCategory, QHeaderView::ResizeToContents);
  header->setSectionResizeMode(kColInstalledVersion, QHeaderView::ResizeToContents);
  header->setSectionResizeMode(kColMarketplaceVersion, QHeaderView::ResizeToContents);
  header->setSectionResizeMode(kColDescription, QHeaderView::Stretch);
  // Name, Category, Installed and Marketplace are sortable; a click on
  // Description (long free-form text) is intercepted and reverts to the last
  // valid sort state (default: Name ascending).
  connect(header, &QHeaderView::sortIndicatorChanged, this, [this, header](int section, Qt::SortOrder order) {
    if (section == kColName || section == kColCategory || section == kColInstalledVersion ||
        section == kColMarketplaceVersion) {
      last_sort_column_ = section;
      last_sort_order_ = order;
      return;
    }
    QSignalBlocker blocker(header);
    ui_->plugin_table_->sortByColumn(last_sort_column_, last_sort_order_);
  });

  // Delegate that lights up updatable rows in the "update" tone (the per-item
  // setBackground path is defeated by the app-wide QTableView::item QSS). The
  // The per-item fill colour is set in rebuildTable (Destructive pink for an
  // update, Emphasis amber for an incompatible plugin), painted at reduced
  // alpha as a subtle tint. Standard body ink reads cleanly on either.
  table->setItemDelegate(new MarketplaceCellDelegate(theme::text(theme::appTheme()), table));

  // Selecting a row updates the bottom "Details" footer with that plugin.
  connect(table, &QTableWidget::itemSelectionChanged, this, [this]() {
    const QModelIndexList rows = ui_->plugin_table_->selectionModel()->selectedRows();
    if (rows.isEmpty()) {
      return;
    }
    QTableWidgetItem* name = ui_->plugin_table_->item(rows.first().row(), kColName);
    footer_ext_id_ = (name != nullptr) ? name->data(Qt::UserRole).toString() : QString{};
    updateDetailFooter();
  });

  // Footer enable/disable toggle (the standard palette ToggleSwitch — Accent
  // "on" track). Hidden for non-installed plugins; acts on the plugin currently
  // shown in the footer; the change persists and takes effect on next restart.
  ui_->detail_enable_toggle_->setVisible(false);
  connect(ui_->detail_enable_toggle_, &ToggleSwitch::toggled, this, [this](bool checked) {
    if (footer_ext_id_.isEmpty()) {
      return;
    }
    ext_mgr_->setEnabled(footer_ext_id_, checked);
    installations_changed_ = true;
    setStatus(
        (checked ? tr("Extension %1 will be enabled after restart") : tr("Extension %1 will be disabled after restart"))
            .arg(footer_ext_id_));
    rebuildTable();  // keep the table's Enabled column in sync
  });

  // Floor + initial size: wide enough for the four fixed columns plus a roomy
  // Description, without the two-pane minimum the old master-detail view needed.
  setMinimumSize(900, 520);
  resize(1080, 600);

  // Canonical overlay pill scrollbars for the extension list / detail scroll
  // areas just built (their ranges update live as the registry loads).
  attachPillScrollbars(this);
}

// ─── Signal wiring ───────────────────────────────────────────────────────────

void MarketplaceWindow::setupSignals() {
  // A local archive whose id is already installed needs a decision, and the
  // manager cannot take it: this is UI policy. Replacement is staged, so the
  // wording promises "after restart" rather than an immediate swap.
  ext_mgr_->setReplaceConfirmation(
      [this](const QString& id, const QString& installed_version, const QString& archive_version) {
        const QString text = (installed_version == archive_version)
                                 ? tr("\"%1\" is already installed at version %2, the same version the archive "
                                      "carries.\n\nReplace it? The replacement is applied the next time PlotJuggler "
                                      "starts.")
                                       .arg(id, installed_version)
                                 : tr("\"%1\" is already installed at version %2. The archive carries version "
                                      "%3.\n\nReplace it? The replacement is applied the next time PlotJuggler "
                                      "starts.")
                                       .arg(id, installed_version, archive_version);
        const int choice = MessageBox::question(
            this, tr("Replace installed extension?"), text,
            {{tr("Replace"), MessageBox::kPrimaryRole}, {tr("Cancel"), MessageBox::kCancelRole}});
        return choice == 0;
      });

  // RegistryManager
  connect(registry_mgr_, &RegistryManager::fetchStarted, this, [this]() { setInfoStatus("Loading registry..."); });

  connect(registry_mgr_, &RegistryManager::fetchFinished, this, [this](bool success) {
    if (!success) {
      setStatus("Failed to load registry", true);
      return;
    }
    // A successful refresh is a strong "things are working" signal; let it
    // override any old sticky error so progress messages aren't suppressed.
    clearStickyStatus();
    registry_extensions_ = registry_mgr_->compatibleExtensions(PlatformUtils::currentPlatform());
    rebuildExtensionList();
    applyFilters();
    setInfoStatus("Ready — " + QString::number(extensions_.size()) + " extensions loaded");
  });

  connect(ext_mgr_, &ExtensionManager::installPendingRestart, this, [this](const QString& id) {
    // Staging finishes the active install just like installFinished does; clear
    // the busy marker so the card flips to "Needs Restart" (not a stuck
    // "Installing" badge) and processInstallQueue() can advance to the next item.
    if (id == active_install_id_) {
      active_install_id_.clear();
    }
    installations_changed_ = true;
    local_install_in_flight_ = false;
    ui_->progress_bar_->setVisible(false);
    status_error_sticky_ = false;
    refreshAfterInstalledChange();
    setStatus(QString("Extension %1 staged — will be active after restart").arg(id));
    processInstallQueue();
  });

  connect(ext_mgr_, &ExtensionManager::uninstallPendingRestart, this, [this](const QString& id) {
    ui_->progress_bar_->setVisible(false);
    status_error_sticky_ = false;
    refreshAfterInstalledChange();
    setStatus(QString("Extension %1 staged — will be uninstalled after restart").arg(id));
  });

  connect(ext_mgr_, &ExtensionManager::downgradePendingRestart, this, [this](const QString& id) {
    ui_->progress_bar_->setVisible(false);
    status_error_sticky_ = false;
    refreshAfterInstalledChange();
    setStatus(QString("Extension %1 will revert to its bundled version after restart").arg(id));
  });

  connect(registry_mgr_, &RegistryManager::fetchError, this, [this](const QString& error) {
    setStatus("Registry error: " + error, true);
  });

  // ExtensionManager
  connect(ext_mgr_, &ExtensionManager::installStarted, this, [this](const QString& id) {
    active_install_id_ = id;
    ui_->progress_bar_->setValue(0);
    ui_->progress_bar_->setRange(0, 100);
    ui_->progress_bar_->setVisible(true);
    showInstallProgress();
    rebuildTable();  // repaint so the active card shows the "Installing" badge
  });

  connect(ext_mgr_, &ExtensionManager::installProgress, this, [this](const QString& /*id*/, int percent) {
    ui_->progress_bar_->setValue(percent);
  });

  // Post-download phases (verifying, extracting) do not report byte-level
  // progress, so we flip the bar to indeterminate/busy mode and update the
  // status label with the current phase.
  connect(
      ext_mgr_, &ExtensionManager::installPhase, this, [this](const QString& /*id*/, DownloadManager::WorkPhase phase) {
        ui_->progress_bar_->setRange(0, 0);
        QString verb;
        switch (phase) {
          case DownloadManager::WorkPhase::Verifying:
            verb = u"Verifying"_s;
            break;
          case DownloadManager::WorkPhase::Extracting:
            verb = u"Extracting"_s;
            break;
        }
        showInstallProgress(verb);
      });

  connect(ext_mgr_, &ExtensionManager::installFinished, this, [this](const QString& id, bool success) {
    // Only clear the busy marker if this is the finish of the install we
    // actually started. A failure from a call that never reached
    // installStarted (rejected by an ExtensionManager guard, e.g.
    // unsupported platform) also emits installFinished with success=false;
    // in that case active_install_id_ still points at the install that IS
    // in flight and must stay set until it completes.
    if (id == active_install_id_) {
      active_install_id_.clear();
    }
    ui_->progress_bar_->setVisible(false);
    if (success) {
      installations_changed_ = true;
    }
    refreshAfterInstalledChange();
    const bool was_sideload = std::exchange(local_install_in_flight_, false);
    if (success) {
      status_error_sticky_ = false;
      setStatus(installedStatusText(id, was_sideload));
    }
    // On failure the status was already set by installError — do not overwrite it.
    processInstallQueue();
  });

  connect(ext_mgr_, &ExtensionManager::installError, this, [this](const QString& /*id*/, const QString& error) {
    ui_->progress_bar_->setVisible(false);
    setStatus("Installation failed: " + error, true);
    // Queue advance lives in installFinished only — installError + installFinished both
    // fire from emitInstallFailure, so advancing here would double-pop the queue.
  });

  connect(ext_mgr_, &ExtensionManager::uninstallFinished, this, [this](const QString& id, bool success) {
    if (success) {
      status_error_sticky_ = false;
      installations_changed_ = true;
      // Read the name BEFORE the repaint: uninstalling a local-only extension
      // drops its row from extensions_, leaving nothing to look the name up in.
      QString name = id;
      for (const auto& ext : extensions_) {
        if (ext.id == id) {
          name = ext.name;
          break;
        }
      }
      refreshAfterInstalledChange();
      setStatus("Uninstalled " + name);
    }
    // On failure the status was already set by uninstallError — do not overwrite it.
  });

  connect(ext_mgr_, &ExtensionManager::uninstallError, this, [this](const QString& /*id*/, const QString& error) {
    setStatus("Uninstall failed: " + error, true);
  });

  connect(
      ext_mgr_, &ExtensionManager::diagnosticReported, this,
      [this](const QString& /*id*/, const QString& message, bool is_error) {
        updateDiagnosticsButton();
        // Seen live while the window is open — mark it surfaced so a later
        // re-open doesn't show it again as if it were new.
        ext_mgr_->markDiagnosticsSurfaced();
        if (is_error) {
          setStatus("Marketplace diagnostic: " + message, true);
        }
      });
}

// ─── Table Population ─────────────────────────────────────────

void MarketplaceWindow::rebuildTable(bool preserve_scroll) {
  auto* table = ui_->plugin_table_;
  const int saved_scroll = table->verticalScrollBar()->value();

  // Silence selection changes while tearing down + repopulating (setRowCount(0)
  // clears the selection); the deliberate selectRow() at the end fires exactly
  // one itemSelectionChanged that refreshes the footer. Also switch sorting off
  // while inserting rows — otherwise QTableWidget would resort after each row
  // (O(n²) inserts, and the intermediate order breaks the row-index bookkeeping
  // above).
  table->blockSignals(true);
  const bool sorting_was_enabled = table->isSortingEnabled();
  table->setSortingEnabled(false);
  table->setRowCount(0);

  // Marketplace-cell highlight fills (subtle tints over the cell surface): the
  // Destructive pink for an available update, the Emphasis amber for an
  // incompatible plugin — the same amber the footer's incompatibility notice
  // uses.
  const auto fw = theme::appTheme();
  QColor update_fill = theme::destructive(theme::Destructive::Nominal, fw);
  update_fill.setAlphaF(0.22);
  QColor incompatible_fill = theme::interaction(theme::Variant::Emphasis, theme::State::Nominal, fw);
  incompatible_fill.setAlphaF(0.22);

  const auto installed = ext_mgr_->installedExtensions();
  for (const Extension& ext : filtered_) {
    const int row = table->rowCount();
    table->insertRow(row);

    const bool is_installed = installed.contains(ext.id);
    const bool has_update = ext_mgr_->hasUpdate(ext);
    const auto compat = ext_mgr_->hostCompatibility(ext);

    // Name, with the full description as tooltip. Plain QTableWidgetItem — the
    // Name column sorts by case-insensitive text.
    auto* name_item = new QTableWidgetItem(ext.name);
    name_item->setToolTip(ext.description);
    name_item->setData(Qt::UserRole, ext.id);
    table->setItem(row, kColName, name_item);

    // Category — the registry `category` value verbatim (data_loader,
    // data_stream, message_parser, toolbox). CategoryItem breaks category ties
    // alphabetically by the plugin name it stores in kCategoryNameKeyRole, so
    // the default sort reads as "grouped by category, alphabetical within".
    auto* category_item = new CategoryItem(ext.category);
    category_item->setToolTip(ext.category);
    category_item->setData(kCategoryNameKeyRole, ext.name);
    table->setItem(row, kColCategory, category_item);

    // Installed version (an em dash when not installed), centered. Asked of the
    // manager rather than read off the snapshot: a core plugin the startup seed
    // refreshed cannot be re-scanned to its new version within this process.
    auto* installed_item = new VersionItem(is_installed ? ext_mgr_->installedVersion(ext.id) : u"\u2014"_s);
    installed_item->setTextAlignment(Qt::AlignCenter);
    table->setItem(row, kColInstalledVersion, installed_item);

    // Marketplace (registry) version \u2014 the version an update would move to. Only
    // THIS cell is highlighted (MarketplaceCellDelegate paints it), not the whole
    // row. Incompatible wins over update: an incompatible plugin's cell is
    // amber (matching the footer notice) even when it also has an update;
    // otherwise an available update paints it pink.
    auto* market_item = new VersionItem(ext.version);
    market_item->setTextAlignment(Qt::AlignCenter);
    if (!compat.ok) {
      market_item->setData(kHighlightColorRole, incompatible_fill);
      market_item->setToolTip(compat.reason);
    } else if (has_update) {
      market_item->setData(kHighlightColorRole, update_fill);
      market_item->setToolTip(tr("Update available: v%1").arg(ext.version));
    }
    table->setItem(row, kColMarketplaceVersion, market_item);

    // Description: full text, wraps within the stretched column. No tooltip —
    // the cell already renders the whole string, so a tooltip repeating it
    // adds nothing and covers the row on hover.
    auto* desc_item = new QTableWidgetItem(ext.description);
    table->setItem(row, kColDescription, desc_item);
  }

  // Re-enable sorting AFTER all rows are populated and sort by the last
  // user-chosen Name/Category column so the visible order is deterministic
  // across rebuilds. resizeRowsToContents follows so heights match the sorted
  // rows.
  if (sorting_was_enabled) {
    table->setSortingEnabled(true);
    table->sortByColumn(last_sort_column_, last_sort_order_);
  }

  table->resizeRowsToContents();
  // resizeRowsToContents sizes to text; lift any row below the readability floor.
  for (int row = 0; row < table->rowCount(); ++row) {
    if (table->rowHeight(row) < kMinRowHeight) {
      table->setRowHeight(row, kMinRowHeight);
    }
  }

  table->blockSignals(false);

  // Restore the selection by ext id (preserved across rebuilds) so the details
  // footer stays on the same plugin through install/update repaints; fall back
  // to the first row. selectRow() fires itemSelectionChanged → updateDetailFooter().
  if (table->rowCount() > 0) {
    int target = 0;
    for (int row = 0; row < table->rowCount(); ++row) {
      if (table->item(row, kColName)->data(Qt::UserRole).toString() == footer_ext_id_) {
        target = row;
        break;
      }
    }
    table->selectRow(target);
  } else {
    footer_ext_id_.clear();
    updateDetailFooter();
  }

  // Enable "Update All" if ANY loaded extension has an update — not just the
  // filtered subset shown — so the button's reach matches its action. A staged
  // update keeps its old installed version until restart (hasUpdate stays true),
  // so exclude already-pending extensions to avoid re-staging what is queued.
  // Incompatible updates are excluded: their new version needs a newer host, so
  // install() would reject them — Update All must not appear to offer what it
  // can't do (it disables entirely when every pending update is incompatible).
  bool any_updatable = false;
  for (const Extension& ext : extensions_) {
    if (ext_mgr_->hasUpdate(ext) && ext_mgr_->hostCompatibility(ext).ok && !ext_mgr_->hasPendingInstall(ext.id) &&
        !ext_mgr_->hasPendingUninstall(ext.id)) {
      any_updatable = true;
      break;
    }
  }
  // Stay disabled while anything is in flight, not just while the queue is
  // non-empty: processInstallQueue() pops the last item before its install
  // finishes, so update_queue_ empties while active_install_id_ is still
  // downloading (not yet staged, so it still counts as updatable). Without the
  // active_install_id_ check the button would re-enable mid-batch and a click
  // would re-dispatch the in-flight update.
  ui_->update_all_btn_->setEnabled(any_updatable && update_queue_.isEmpty() && active_install_id_.isEmpty());

  if (preserve_scroll) {
    QTimer::singleShot(
        0, this, [this, saved_scroll]() { ui_->plugin_table_->verticalScrollBar()->setValue(saved_scroll); });
  }
}

// ─── Details footer ───────────────────────────────────────────

void MarketplaceWindow::updateDetailFooter() {
  // Tear down the previous action row (widgets + the leading stretch).
  QLayoutItem* old_item = nullptr;
  while ((old_item = ui_->detail_buttons_layout->takeAt(0)) != nullptr) {
    if (old_item->widget() != nullptr) {
      old_item->widget()->deleteLater();
    }
    delete old_item;
  }

  const Extension* ext = nullptr;
  for (const Extension& e : extensions_) {
    if (e.id == footer_ext_id_) {
      ext = &e;
      break;
    }
  }
  if (ext == nullptr) {
    ui_->detail_text_->clear();
    ui_->detail_title_->clear();
    ui_->detail_enable_toggle_->setVisible(false);
    return;
  }

  const auto installed = ext_mgr_->installedExtensions();
  // Empty when not installed; see installedVersion for why the
  // snapshot's own version can lag a seed-refreshed core plugin.
  const QString installed_version = ext_mgr_->installedVersion(ext->id);
  const auto esc = [](const QString& s) { return s.toHtmlEscaped(); };

  // Title (plugin name) — a real header-row label so the enable toggle can sit
  // at its height. The green toggle is shown only for installed plugins.
  ui_->detail_title_->setText(ext->name);
  {
    const bool title_installed = installed.contains(ext->id);
    ui_->detail_enable_toggle_->setVisible(title_installed);
    QSignalBlocker block(ui_->detail_enable_toggle_);
    ui_->detail_enable_toggle_->setChecked(title_installed && ext_mgr_->isEnabled(ext->id), /*animate=*/false);
  }

  QString html;

  // When the selected plugin is incompatible with this host, lead the footer with
  // a prominent notice carrying the reason, in the palette's Emphasis (amber)
  // tone — the same amber the Marketplace-version cell uses. Emphasis is a fill
  // family (no legible text ink), so tint the notice's background rather than its
  // text and keep the standard body ink on top. When the plugin is ALSO outdated
  // (an installed version with a newer — but incompatible — registry version), the
  // notice states both facts: the update exists but is blocked, and why (R6).
  if (const auto compat = ext_mgr_->hostCompatibility(*ext); !compat.ok) {
    QColor tint = theme::interaction(theme::Variant::Emphasis, theme::State::Nominal, theme::appTheme());
    tint.setAlphaF(0.30);
    const QString message =
        ext_mgr_->hasUpdate(*ext)
            ? tr("⚠ Update to v%1 available, but blocked — %2").arg(esc(ext->version), esc(compat.reason))
            : tr("⚠ Incompatible — %1").arg(esc(compat.reason));
    html +=
        u"<p style='margin:0 0 6px 0; padding:3px 6px; font-weight:700; "
        u"background-color:rgba(%1,%2,%3,%4);'>%5</p>"_s.arg(tint.red())
            .arg(tint.green())
            .arg(tint.blue())
            .arg(tint.alphaF())
            .arg(message);
  }

  // Metadata line (publisher/author • category • license • requires PJ •
  // installed).
  QStringList meta;
  if (!ext->publisher.isEmpty()) {
    meta << esc(ext->publisher);
  } else if (!ext->author.isEmpty()) {
    meta << esc(ext->author);
  }
  if (!ext->category.isEmpty()) {
    meta << esc(ext->category);
  }
  if (!ext->license.isEmpty()) {
    meta << esc(ext->license);
  }
  if (!ext->min_plotjuggler_version.isEmpty()) {
    meta << u"requires PJ %1+"_s.arg(esc(ext->min_plotjuggler_version));
  }
  if (!installed_version.isEmpty()) {
    meta << u"installed: v%1"_s.arg(esc(installed_version));
  }
  if (!meta.isEmpty()) {
    html += u"<p style='margin:0 0 6px 0;'>%1</p>"_s.arg(meta.join(u"  •  "_s));
  }

  // Description.
  if (!ext->description.isEmpty()) {
    html += u"<p style='margin:0 0 6px 0;'>%1</p>"_s.arg(esc(ext->description));
  }

  // Changelog: newest version first, matching the marketplace spec's examples.
  // The map is keyed by version string, so its natural order is lexicographic
  // ("1.10.0" before "1.9.0"); sort the keys by semver instead.
  if (!ext->changelog.isEmpty()) {
    QStringList versions = ext->changelog.keys();
    std::sort(versions.begin(), versions.end(), [](const QString& a, const QString& b) {
      return compareSemver(a.toStdString(), b.toStdString()) > 0;
    });
    html += u"<p style='margin:0 0 2px 0;'><b>Changelog</b></p><ul style='margin:0 0 0 -20px;'>"_s;
    for (const QString& version : versions) {
      html += u"<li><b>%1</b> — %2</li>"_s.arg(esc(version), esc(ext->changelog.value(version)));
    }
    html += u"</ul>"_s;
  }

  ui_->detail_text_->setHtml(html);

  // ── Action row (same order as the old detail panel): primary action ·
  //    Uninstall/Downgrade · stretch · Visit Website. ──
  const QString ext_id = ext->id;
  const bool is_installed = installed.contains(ext_id);
  const bool has_update = ext_mgr_->hasUpdate(*ext);
  const bool has_newer_local = ext_mgr_->hasNewerInstalledVersion(*ext);
  const bool needs_restart = ext_mgr_->hasPendingInstall(ext_id) || ext_mgr_->hasPendingUninstall(ext_id);
  const bool in_update_queue =
      std::any_of(update_queue_.begin(), update_queue_.end(), [&](const Extension& e) { return e.id == ext_id; });
  const bool installing = ext_id == active_install_id_ || pending_clicks_.contains(ext_id) || in_update_queue;
  const bool is_bundled = ext_mgr_->isBundled(ext_id);

  // Leading stretch pushes the whole cluster to the right edge.
  ui_->detail_buttons_layout->addStretch();

  // Primary action / status: colour-coded per state via the #extButton*/#extBadge*
  // QSS rules. Uninstall/Downgrade/Visit Website below stay standard buttons.
  // Every footer button carries mpFooterButton so the stylesheet gives them one
  // shared geometry: a min-width floor plus the snug vertical padding that sets
  // their height. Pinned in QSS rather than via setFixedWidth because these are
  // QSS-styled buttons, and QStyleSheetStyle governs their geometry — a
  // widget-level fixed width is ignored. min-width is a floor, not a clamp, so a
  // long label (Downgrade names the version) still grows to its natural width
  // while keeping the same height as its neighbours.
  const auto mark_footer_button = [](QPushButton* button) { button->setProperty("mpFooterButton", true); };
  auto* action = new QPushButton;
  mark_footer_button(action);
  if (installing) {
    action->setText(tr("Installing"));
    action->setObjectName("extBadgeInstalling");
    action->setEnabled(false);
  } else if (needs_restart) {
    action->setText(tr("Needs Restart"));
    action->setObjectName("extBadgeNeedsRestart");
    action->setEnabled(false);
  } else if (has_update) {
    action->setText(tr("Update"));
    action->setObjectName("extButtonUpdate");
    connect(action, &QPushButton::clicked, this, [this, ext_id]() { onActionButtonClicked(ext_id); });
  } else if (has_newer_local) {
    action->setText(tr("Local newer"));
    action->setObjectName("extBadgeLocalNewer");
    action->setEnabled(false);
  } else if (is_installed) {
    action->setText(tr("Installed"));
    action->setObjectName("extBadgeInstalled");
    action->setEnabled(false);
  } else {
    action->setText(tr("Install"));
    action->setObjectName("extButtonInstall");
    connect(action, &QPushButton::clicked, this, [this, ext_id]() { onActionButtonClicked(ext_id); });
  }
  // An incompatible plugin can't be installed/updated: disable the actionable
  // primary button (Install/Update — the state badges are already disabled) and
  // surface the reason on hover. install() would refuse it anyway; this stops the
  // click before it starts.
  if (action->isEnabled()) {
    if (const auto compat = ext_mgr_->hostCompatibility(*ext); !compat.ok) {
      action->setEnabled(false);
      action->setToolTip(compat.reason);
    }
  }
  ui_->detail_buttons_layout->addWidget(action);

  // Uninstall / Downgrade-to-bundled (installed, not mid-operation), with the
  // same bundled logic the old detail dialog used. Standard button style too.
  if (is_installed && !installing && !needs_restart) {
    const QString bundled_version = ext_mgr_->bundledVersion(ext_id);
    const int installed_vs_bundled =
        is_bundled ? compareSemver(installed_version.toStdString(), bundled_version.toStdString()) : 0;
    if (is_bundled && installed_vs_bundled <= 0) {
      // Core plugin at its bundled version: shown but locked.
      auto* uninstall = new QPushButton(tr("Uninstall"));
      mark_footer_button(uninstall);
      uninstall->setEnabled(false);
      uninstall->setToolTip(tr("This extension ships with the application and cannot be uninstalled"));
      ui_->detail_buttons_layout->addWidget(uninstall);
    } else if (is_bundled) {
      // Core plugin updated above bundled: offer revert-to-bundled.
      auto* downgrade = new QPushButton(tr("Downgrade to bundled v%1").arg(bundled_version));
      mark_footer_button(downgrade);
      downgrade->setToolTip(tr("Reverts to the bundled version v%1 on the next launch").arg(bundled_version));
      connect(downgrade, &QPushButton::clicked, this, [this, ext_id]() {
        clearStickyStatus();
        ext_mgr_->downgradeToBundled(ext_id);
      });
      ui_->detail_buttons_layout->addWidget(downgrade);
    } else {
      auto* uninstall = new QPushButton(tr("Uninstall"));
      mark_footer_button(uninstall);
      connect(uninstall, &QPushButton::clicked, this, [this, ext_id]() { onUninstallButtonClicked(ext_id); });
      ui_->detail_buttons_layout->addWidget(uninstall);
    }
  }

  // Visit Website / repository (right edge).
  const QString url = !ext->website.isEmpty() ? ext->website : ext->repository;
  if (!url.isEmpty()) {
    auto* web = new QPushButton(tr("Visit Website"));
    mark_footer_button(web);
    connect(web, &QPushButton::clicked, this, [url]() { QDesktopServices::openUrl(QUrl(url)); });
    ui_->detail_buttons_layout->addWidget(web);
  }
}

// ─── Filtering ────────────────────────────────────────────────────────────────

bool MarketplaceWindow::rebuildExtensionList() {
  QStringList previous_ids;
  previous_ids.reserve(extensions_.size());
  for (const Extension& ext : extensions_) {
    previous_ids << ext.id;
  }

  extensions_ = registry_extensions_;

  // An installed id the registry does not list has no row to reuse, so build one
  // from what the plugin declares about itself. The registry-sourced fields
  // (author, license, website, changelog, platforms…) stay empty; every consumer
  // guards them with isEmpty(), and the footer's primary action is disabled for an
  // installed extension anyway, so no install path can be driven off this row.
  const auto installed = ext_mgr_->installedExtensions();
  for (auto it = installed.cbegin(); it != installed.cend(); ++it) {
    if (std::any_of(registry_extensions_.cbegin(), registry_extensions_.cend(), [&](const Extension& ext) {
          return ext.id == it.key();
        })) {
      continue;
    }
    const InstalledExtension& record = it.value();
    Extension local;
    local.id = record.id;
    local.name = record.name;
    local.description = record.description;
    local.category = record.category;
    local.version = record.version;
    extensions_.append(local);
  }

  QStringList current_ids;
  current_ids.reserve(extensions_.size());
  for (const Extension& ext : extensions_) {
    current_ids << ext.id;
  }
  return current_ids != previous_ids;
}

void MarketplaceWindow::refreshAfterInstalledChange() {
  if (rebuildExtensionList()) {
    applyFilters();
  } else {
    rebuildTable();
  }
}

void MarketplaceWindow::applyFilters() {
  // Trim before matching: leading/trailing whitespace is not meaningful in a
  // search term, and an untrimmed space makes contains() miss every plugin
  // whose name/description doesn't embed that exact space (a stray space →
  // empty list).
  const QString search = ui_->search_edit_->text().trimmed().toLower();

  // Category values are the strings the published registry actually ships in each
  // extension's "category" field, NOT the button labels and NOT the vocabulary in
  // the marketplace spec doc (§5.2 lists data_streamer/parser/bundle; the live
  // registry uses data_stream/message_parser and ships no bundles). A value that
  // does not appear in the registry silently matches nothing, so these must be
  // checked against real registry data rather than the spec.
  QStringList active_categories;
  if (ui_->filter_data_loader_->isChecked()) {
    active_categories << u"data_loader"_s;
  }
  if (ui_->filter_data_streamer_->isChecked()) {
    active_categories << u"data_stream"_s;
  }
  if (ui_->filter_parser_->isChecked()) {
    active_categories << u"message_parser"_s;
  }
  if (ui_->filter_toolbox_->isChecked()) {
    active_categories << u"toolbox"_s;
  }
  const bool installed_only = ui_->filter_installed_->isChecked();
  // R2: hide plugins incompatible with this host (on by default). R2a: never hide
  // one that's already installed — even incompatible/outdated/disabled — so the
  // user can always see and manage what they have.
  const bool compatible_only = ui_->filter_compatible_->isChecked();

  // The categories the four toggles can represent. A plugin whose category is
  // one of these obeys its toggle; a plugin whose category falls outside this
  // set (empty, or an unmodeled/future value) has no toggle to govern it.
  static const QStringList kToggleableCategories = {
      u"data_loader"_s, u"data_stream"_s, u"message_parser"_s, u"toolbox"_s};

  filtered_.clear();
  for (const auto& ext : extensions_) {
    // Category checkboxes are additive: a toggleable category is shown only
    // while its toggle is checked (all four checked = every such category
    // visible; all four unchecked = none). A category outside the toggleable
    // set is never hidden here — it belongs to no toggle, so filtering it out
    // would make it permanently unreachable (a registry plugin with no category
    // could never be found to install, an installed one never uninstalled).
    if (kToggleableCategories.contains(ext.category) && !active_categories.contains(ext.category)) {
      continue;
    }
    if (installed_only && !ext_mgr_->isInstalled(ext.id)) {
      continue;
    }
    if (compatible_only && !ext_mgr_->hostCompatibility(ext).ok && !ext_mgr_->isInstalled(ext.id)) {
      continue;
    }
    if (!search.isEmpty()) {
      bool match = ext.name.toLower().contains(search) || ext.description.toLower().contains(search);
      if (!match) {
        for (const auto& tag : ext.tags) {
          if (tag.toLower().contains(search)) {
            match = true;
            break;
          }
        }
      }
      if (!match) {
        continue;
      }
    }
    filtered_.append(ext);
  }

  rebuildTable(/*preserve_scroll=*/false);
  setInfoStatus(QString::number(filtered_.size()) + " of " + QString::number(extensions_.size()) + " extensions shown");
}

void MarketplaceWindow::setStatus(const QString& msg, bool is_error) {
  if (!is_error && status_error_sticky_) {
    return;
  }
  status_error_sticky_ = is_error;
  ui_->status_label_->setText(msg);
  // The error tone is keyed off objectName via the
  // QLabel#marketplaceStatusError rule in resources/stylesheet_*.qss.
  // Clearing the objectName restores the inherited default text style.
  ui_->status_label_->setObjectName(is_error ? u"marketplaceStatusError"_s : QString{});
  ui_->status_label_->style()->unpolish(ui_->status_label_);
  ui_->status_label_->style()->polish(ui_->status_label_);
}

void MarketplaceWindow::clearStickyStatus() {
  status_error_sticky_ = false;
}

QString MarketplaceWindow::queueSuffix() const {
  const int queued = pending_clicks_.size() + update_queue_.size();
  if (queued == 0) {
    return {};
  }
  return u"  ·  "_s + QString::number(queued) + u" queued"_s;
}

void MarketplaceWindow::setInfoStatus(const QString& msg) {
  if (!active_install_id_.isEmpty()) {
    // An install owns the status line; keep it showing the live progress rather
    // than letting a filter/refresh/registry-load message desync it from the
    // still-moving progress bar.
    showInstallProgress();
    return;
  }
  setStatus(msg);
}

void MarketplaceWindow::showInstallProgress(const QString& verb) {
  if (active_install_id_.isEmpty()) {
    return;
  }
  QString name = active_install_id_;
  for (const auto& ext : extensions_) {
    if (ext.id == active_install_id_) {
      name = ext.name;
      break;
    }
  }
  setStatus(verb + u" "_s + name + u"…"_s + queueSuffix());
}

void MarketplaceWindow::showLatestDiagnostic() {
  // Surface a diagnostic on window open ONLY if one arrived that hasn't been
  // shown yet (e.g. a staged-promotion failure that happened at startup while
  // no window was open). Re-showing the latest unconditionally on every re-open
  // resurrected a stale error the user had already moved past.
  if (!ext_mgr_->hasUnsurfacedDiagnostics()) {
    return;
  }
  const ExtensionDiagnostic& diagnostic = ext_mgr_->diagnostics().back();
  ext_mgr_->markDiagnosticsSurfaced();
  setStatus("Marketplace diagnostic: " + diagnostic.message, diagnostic.is_error);
}

void MarketplaceWindow::updateDiagnosticsButton() {
  const int count = ext_mgr_->diagnostics().size();
  ui_->diagnostics_btn_->setVisible(count > 0);
  ui_->diagnostics_btn_->setText(count > 1 ? QString("Details (%1)").arg(count) : "Details");
}

// ─── Slots ────────────────────────────────────────────────────────────────────

void MarketplaceWindow::onSearchChanged(const QString& /*text*/) {
  // Filtering is a user action like Refresh/Install: clear a sticky error so the
  // "K of N shown" count applyFilters() emits isn't suppressed by setStatus().
  clearStickyStatus();
  applyFilters();
}
void MarketplaceWindow::onFilterToggled() {
  clearStickyStatus();
  applyFilters();
}

void MarketplaceWindow::onInstallLocalClicked() {
  const QString path =
      FileDialog::getOpenFileName(this, tr("Install plugin from local ZIP"), QString(), tr("Plugin package (*.zip)"));
  if (path.isEmpty()) {
    return;  // cancelled
  }
  clearStickyStatus();
  local_install_in_flight_ = true;
  ext_mgr_->installFromLocalZip(path);
}

QString MarketplaceWindow::installedStatusText(const QString& id, bool from_file) const {
  // A sideloaded id is by definition absent from the registry list, so its name and
  // version have to come from the installed snapshot instead.
  //
  // No restart is promised here: this runs on installFinished, which a sideload
  // only reaches on the fresh-install branch — the one that writes straight to the
  // extensions dir and is picked up by the host's catalog reload, exactly like a
  // fresh registry install. Replacing an installed id is staged instead and
  // reports through installPendingRestart, which owns the restart wording.
  if (from_file) {
    const QString version = ext_mgr_->installedVersion(id);
    return version.isEmpty() ? QString("Installed %1 from file").arg(id)
                             : QString("Installed %1 v%2 from file").arg(id, version);
  }
  for (const auto& ext : extensions_) {
    if (ext.id == id) {
      return "Installed " + ext.name + " v" + ext.version;
    }
  }
  return "Installed " + id;
}

void MarketplaceWindow::showEvent(QShowEvent* event) {
  activateEmbedded();
  Dialog::showEvent(event);
}

void MarketplaceWindow::activateEmbedded() {
  if (ext_mgr_ == nullptr) {
    return;
  }
  bool state_changed = false;
  if (initial_snapshot_provided_) {
    initial_snapshot_provided_ = false;
    state_changed = true;
  } else {
    const auto before = ext_mgr_->installedExtensions();
    ext_mgr_->refreshInstalledFromDisk();
    if (!installedStatesEqual(ext_mgr_->installedExtensions(), before)) {
      installations_changed_ = true;
      state_changed = true;
    }
  }
  // Composing is unconditional even though repainting is not: this is the only
  // entry point guaranteed to run, so a session whose registry never loads would
  // otherwise never build the local-only rows and would show an empty list.
  if (rebuildExtensionList()) {
    applyFilters();
  } else if (state_changed) {
    rebuildTable();
  }
  updateDiagnosticsButton();
  showLatestDiagnostic();
}

void MarketplaceWindow::onActionButtonClicked(const QString& ext_id) {
  // If another install/update is already in flight, queue this click and let
  // processInstallQueue() dispatch it when the current one completes.
  // Otherwise ExtensionManager::install() would reject with
  // "Install of X is already in progress" — its single-install-at-a-time
  // model is intentional, we just hide it behind a queue at the UI layer.
  if (!active_install_id_.isEmpty()) {
    const bool in_update_queue =
        std::any_of(update_queue_.begin(), update_queue_.end(), [&](const Extension& e) { return e.id == ext_id; });
    if (ext_id == active_install_id_ || pending_clicks_.contains(ext_id) || in_update_queue) {
      return;  // deduplicate — running, queued by a click, or already in the Update All batch
    }
    pending_clicks_.append(ext_id);
    showInstallProgress();  // keep the active install visible; reflect the new queue depth
    rebuildTable();         // repaint so the queued card shows the "Installing" badge
    return;
  }

  // Resolve against extensions_ (all loaded), not filtered_: a queued click
  // dispatched by processInstallQueue() must still be found even if the user
  // changed the search/category filter and it is no longer in the visible set.
  for (const auto& ext : extensions_) {
    if (ext.id != ext_id) {
      continue;
    }
    clearStickyStatus();
    if (ext_mgr_->hasUpdate(ext)) {
      ext_mgr_->update(ext);
    } else if (ext_mgr_->hasNewerInstalledVersion(ext)) {
      setStatus("Installed version is newer than registry version", true);
    } else if (!ext_mgr_->isInstalled(ext.id)) {
      ext_mgr_->install(ext);
    }
    return;
  }
}

void MarketplaceWindow::onUninstallButtonClicked(const QString& ext_id) {
  clearStickyStatus();
  ext_mgr_->uninstall(ext_id);
}

void MarketplaceWindow::onUpdateAllClicked() {
  clearStickyStatus();
  update_queue_.clear();
  // Iterate every loaded extension, not just the currently filtered/searched
  // subset: "Update All" means all updatable extensions, regardless of the
  // active category filter or search term. Skip extensions already staged for
  // restart: a staged update leaves the installed version unchanged (so
  // hasUpdate() stays true) but re-queuing it would just re-download and
  // re-stage the same payload.
  for (const auto& ext : extensions_) {
    // Skip an update already in flight or queued by an individual click: the
    // one currently downloading (active_install_id_) has not staged yet, so
    // hasPendingInstall() is still false — re-queuing it would dispatch update()
    // a second time once it stages and be rejected with "already staged". This
    // mirrors the dedup in onActionButtonClicked.
    if (ext.id == active_install_id_ || pending_clicks_.contains(ext.id)) {
      continue;
    }
    // Skip incompatible updates — install() would reject them (see the enable
    // guard in rebuildTable, which keeps the button in step with this queue).
    if (ext_mgr_->hasUpdate(ext) && ext_mgr_->hostCompatibility(ext).ok && !ext_mgr_->hasPendingInstall(ext.id) &&
        !ext_mgr_->hasPendingUninstall(ext.id)) {
      update_queue_.append(ext);
    }
  }
  if (update_queue_.isEmpty()) {
    return;
  }
  ui_->update_all_btn_->setEnabled(false);
  setStatus("Updating " + QString::number(update_queue_.size()) + " extensions...");
  rebuildTable();  // repaint so all queued cards show the "Installing" badge
  processInstallQueue();
}

void MarketplaceWindow::onDiagnosticsClicked() {
  Dialog dlg(this);
  dlg.setDialogTitle(tr("Marketplace Diagnostics"));
  dlg.resize(640, 360);

  auto* body = new QWidget;
  auto* layout = new QVBoxLayout(body);
  auto* text = new QPlainTextEdit(body);
  text->setReadOnly(true);

  QStringList lines;
  for (const ExtensionDiagnostic& diagnostic : ext_mgr_->diagnostics()) {
    const QString level = diagnostic.is_error ? "ERROR" : "INFO";
    const QString id = diagnostic.id.isEmpty() ? "-" : diagnostic.id;
    lines.append(QString("[%1] %2 %3: %4")
                     .arg(diagnostic.timestamp.toLocalTime().toString(Qt::ISODate), level, id, diagnostic.message));
  }
  text->setPlainText(lines.isEmpty() ? "No diagnostics." : lines.join('\n'));
  layout->addWidget(text);

  auto* close_row = new QHBoxLayout;
  close_row->addStretch();
  auto* close_button = new QPushButton(tr("Close"), body);
  close_row->addWidget(close_button);
  connect(close_button, &QPushButton::clicked, &dlg, &QDialog::reject);
  layout->addLayout(close_row);
  dlg.contentLayout()->addWidget(body);
  dlg.exec();
}

void MarketplaceWindow::processInstallQueue() {
  // Wait until the current install/update finishes before dispatching the
  // next one — ExtensionManager only runs one at a time.
  if (!active_install_id_.isEmpty()) {
    return;
  }
  // Individual button clicks (pending_clicks_) run ahead of Update All
  // (update_queue_) so an explicit user click on a card is not stuck
  // behind a bulk-update batch that was already in flight.
  if (!pending_clicks_.isEmpty()) {
    const QString next_id = pending_clicks_.takeFirst();
    onActionButtonClicked(next_id);
    // If the dispatch didn't actually start an install (e.g. the extension is
    // already installed or is "local newer" by now), no installFinished will
    // fire to advance the queue — keep draining so one dead entry can't stall
    // the rest.
    if (active_install_id_.isEmpty()) {
      processInstallQueue();
    }
    return;
  }
  if (!update_queue_.isEmpty()) {
    ext_mgr_->update(update_queue_.takeFirst());
  }
}

}  // namespace PJ
