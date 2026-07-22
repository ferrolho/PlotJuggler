// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_widgets/Dialog.h"

#include <QApplication>
#include <QEvent>
#include <QLayout>
#include <QMouseEvent>
#include <QShowEvent>
#include <QSize>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWindow>
#include <algorithm>

#include "pj_widgets/Scrollbar.h"
#include "pj_widgets/SvgUtil.h"
#include "ui_Dialog.h"

namespace PJ {

namespace {

// Hit-test band width around the dialog edge that triggers a resize.
constexpr int kResizeMargin = 6;

Qt::CursorShape cursorForEdges(Qt::Edges edges) {
  switch (static_cast<int>(edges)) {
    case Qt::TopEdge | Qt::LeftEdge:
    case Qt::BottomEdge | Qt::RightEdge:
      return Qt::SizeFDiagCursor;
    case Qt::TopEdge | Qt::RightEdge:
    case Qt::BottomEdge | Qt::LeftEdge:
      return Qt::SizeBDiagCursor;
    case Qt::TopEdge:
    case Qt::BottomEdge:
      return Qt::SizeVerCursor;
    case Qt::LeftEdge:
    case Qt::RightEdge:
      return Qt::SizeHorCursor;
    default:
      return Qt::ArrowCursor;
  }
}

}  // namespace

Dialog::Dialog(QWidget* parent) : QDialog(parent), ui_(new Ui::Dialog) {
  ui_->setupUi(this);

  // Frameless + no system shadow so the WM-drawn chrome doesn't overrule
  // the app's title-bar style. WA_StyledBackground lets the QSS rule on
  // QDialog (or the dialogTitleBar) actually paint.
  setWindowFlag(Qt::FramelessWindowHint, true);
  setWindowFlag(Qt::NoDropShadowWindowHint, true);
  setAttribute(Qt::WA_StyledBackground, true);

  applyIcons();
  // Canonical chrome sizing from the shared defaults (matches the main window).
  // A host with live metrics re-applies via setChromeMetrics().
  setChromeMetrics(ChromeMetrics{});
  connect(ui_->buttonClose, &QToolButton::clicked, this, &QDialog::reject);

  // Application-wide event filter so we can swap the cursor and start
  // a system resize from any widget inside the dialog's edge band.
  // Same pattern MainWindow uses; gated to widgets whose window is us.
  qApp->installEventFilter(this);
}

Dialog::~Dialog() {
  delete ui_;
}

void Dialog::setChromeMetrics(const ChromeMetrics& metrics) {
  const int bar_height = metrics.titleBarHeight();
  ui_->dialogTitleBar->setMinimumHeight(bar_height);
  ui_->dialogTitleBar->setMaximumHeight(bar_height);
  // Close button + icon track the same button extent / icon size the main-window
  // chrome buttons use, so the whole chrome reads identically.
  const int button_extent = metrics.icon_size + metrics.icon_padding;
  ui_->buttonClose->setMinimumSize(button_extent, button_extent);
  ui_->buttonClose->setMaximumSize(button_extent, button_extent);
  ui_->buttonClose->setIconSize(QSize(metrics.icon_size, metrics.icon_size));
  if (auto* layout = ui_->titleBarLayout) {
    layout->setContentsMargins(
        metrics.layout_padding, metrics.layout_padding, metrics.layout_padding, metrics.layout_padding);
    layout->setSpacing(metrics.layout_spacing);
  }
}

void Dialog::setDialogTitle(const QString& title) {
  ui_->dialogTitleLabel->setText(title);
  // "[*]" renders empty yet stops Qt appending the " — PlotJuggler 4" title suffix.
  setWindowTitle(title + "[*]");
}

QString Dialog::dialogTitle() const {
  return ui_->dialogTitleLabel->text();
}

void Dialog::setCloseButtonVisible(bool visible) {
  ui_->buttonClose->setVisible(visible);
}

QWidget* Dialog::contentWidget() const {
  return ui_->dialogContent;
}

QLayout* Dialog::contentLayout() const {
  return ui_->dialogContent->layout();
}

void Dialog::showEvent(QShowEvent* event) {
  QDialog::showEvent(event);
  if (!scroll_pills_attached_) {
    scroll_pills_attached_ = true;
    attachPillScrollbars(contentWidget());
  }
}

void Dialog::applyIcons() {
  ui_->buttonClose->setIcon(loadSvg(":/resources/svg/close_windows_light.svg", currentTheme()));
}

void Dialog::mousePressEvent(QMouseEvent* event) {
  // Drag the dialog when the press lands on the title-bar background or
  // the title label itself. Clicks on the close button or anywhere in
  // the content area fall through to default handling.
  if (event->button() == Qt::LeftButton && ui_->dialogTitleBar->geometry().contains(event->position().toPoint())) {
    QWidget* hit = ui_->dialogTitleBar->childAt(ui_->dialogTitleBar->mapFrom(this, event->position().toPoint()));
    if (hit == nullptr || hit == ui_->dialogTitleLabel) {
      if (auto* h = windowHandle()) {
        const bool system_move_started = h->startSystemMove();
#ifdef Q_OS_WASM
        if (!system_move_started) {
          // Qt-wasm's QPA implements no system move; drive the drag manually
          // from the app-wide event filter until release. Desktop platforms
          // keep their native behavior even when startSystemMove declines.
          manual_move_active_ = true;
          manual_press_global_ = event->globalPosition().toPoint();
          manual_press_geometry_ = geometry();
        }
#else
        Q_UNUSED(system_move_started)
#endif
        event->accept();
        return;
      }
    }
  }
  QDialog::mousePressEvent(event);
}

void Dialog::applyManualDrag(const QPoint& global_pos) {
  const QPoint delta = global_pos - manual_press_global_;
  if (manual_move_active_) {
    move(manual_press_geometry_.topLeft() + delta);
    return;
  }
  const QSize min_size = minimumSizeHint().expandedTo(minimumSize());
  const QSize max_size = maximumSize();
  QRect target = manual_press_geometry_;
  // Grow/shrink only the pressed edges, keeping the opposite edge anchored.
  // Clamping happens edge-wise so an over-shrunk drag pins the moving edge
  // at the size limit instead of pushing the anchored edge around.
  if (manual_resize_edges_ & Qt::LeftEdge) {
    target.setLeft(
        std::clamp(
            manual_press_geometry_.left() + delta.x(), target.right() + 1 - max_size.width(),
            target.right() + 1 - min_size.width()));
  } else if (manual_resize_edges_ & Qt::RightEdge) {
    target.setRight(
        std::clamp(
            manual_press_geometry_.right() + delta.x(), target.left() - 1 + min_size.width(),
            target.left() - 1 + max_size.width()));
  }
  if (manual_resize_edges_ & Qt::TopEdge) {
    target.setTop(
        std::clamp(
            manual_press_geometry_.top() + delta.y(), target.bottom() + 1 - max_size.height(),
            target.bottom() + 1 - min_size.height()));
  } else if (manual_resize_edges_ & Qt::BottomEdge) {
    target.setBottom(
        std::clamp(
            manual_press_geometry_.bottom() + delta.y(), target.top() - 1 + min_size.height(),
            target.top() - 1 + max_size.height()));
  }
  setGeometry(target);
}

Qt::Edges Dialog::edgesAtPoint(const QPoint& pos) const {
  Qt::Edges edges;
  if (pos.x() <= kResizeMargin) {
    edges |= Qt::LeftEdge;
  } else if (pos.x() >= width() - kResizeMargin) {
    edges |= Qt::RightEdge;
  }
  if (pos.y() <= kResizeMargin) {
    edges |= Qt::TopEdge;
  } else if (pos.y() >= height() - kResizeMargin) {
    edges |= Qt::BottomEdge;
  }
  return edges;
}

bool Dialog::eventFilter(QObject* watched, QEvent* event) {
  const QEvent::Type type = event->type();
  if (type != QEvent::MouseMove && type != QEvent::MouseButtonPress && type != QEvent::MouseButtonRelease) {
    return QDialog::eventFilter(watched, event);
  }
  auto* widget = qobject_cast<QWidget*>(watched);
  if (widget == nullptr || widget->window() != this) {
    return QDialog::eventFilter(watched, event);
  }
  if (isMaximized() || isFullScreen()) {
    return QDialog::eventFilter(watched, event);
  }
  auto* mouse_event = static_cast<QMouseEvent*>(event);
  const QPoint global_pos = mouse_event->globalPosition().toPoint();

  if (type == QEvent::MouseButtonRelease) {
    if (manual_resize_edges_ != 0 || manual_move_active_) {
      manual_resize_edges_ = {};
      manual_move_active_ = false;
      return true;
    }
    return false;
  }
  if (type == QEvent::MouseMove) {
    if (manual_resize_edges_ != 0 || manual_move_active_) {
      applyManualDrag(global_pos);
      return true;
    }
    const Qt::Edges hover_edges = edgesAtPoint(mapFromGlobal(global_pos));
    if (hover_edges != 0) {
      setCursor(cursorForEdges(hover_edges));
    } else {
      unsetCursor();
    }
    return false;
  }
  // MouseButtonPress
  const Qt::Edges edges = edgesAtPoint(mapFromGlobal(global_pos));
  if (mouse_event->button() != Qt::LeftButton || edges == 0) {
    return false;
  }
  if (auto* handle = windowHandle()) {
    const bool system_resize_started = handle->startSystemResize(edges);
#ifdef Q_OS_WASM
    if (!system_resize_started) {
      // Qt-wasm's QPA implements no system resize (and frameless windows get
      // none of its DOM resize handles); drive the geometry manually until
      // release. Desktop platforms keep their native behavior.
      manual_resize_edges_ = edges;
      manual_press_global_ = global_pos;
      manual_press_geometry_ = geometry();
    }
#else
    Q_UNUSED(system_resize_started)
#endif
    return true;
  }
  return false;
}

}  // namespace PJ
