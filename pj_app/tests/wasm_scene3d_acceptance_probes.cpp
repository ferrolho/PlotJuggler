// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "wasm_scene3d_acceptance_probes.h"

#include <emscripten.h>

#include <QApplication>
#include <QComboBox>
#include <QImage>
#include <QPointer>
#include <QToolButton>
#include <QUrl>
#include <algorithm>
#include <cstdint>
#include <vector>

#include "MainWindow.h"
#include "pj_scene3d_widgets/Scene3DDockWidget.h"
#include "pj_scene3d_widgets/scene_view_widget.h"
#include "pj_widgets/RealSlider.h"

using namespace Qt::StringLiterals;

namespace {

struct ObservedView {
  quint64 identity = 0;
  QPointer<pj::scene3d::SceneViewWidget> view;
};

QRect geometryRelativeTo(PJ::MainWindow* main_window, QWidget* control) {
  if (control == nullptr) {
    return {-1, -1, 0, 0};
  }
  const QPoint position = main_window->isAncestorOf(control)
                              ? control->mapTo(main_window, QPoint(0, 0))
                              : main_window->mapFromGlobal(control->mapToGlobal(QPoint(0, 0)));
  return {position, control->size()};
}

std::pair<quint64, quint64> framebufferDigest(const QImage& image) {
  quint64 hash = 1469598103934665603ULL;
  quint64 rgb_sum = 0;
  for (int y = 0; y < image.height(); ++y) {
    for (int x = 0; x < image.width(); ++x) {
      const QRgb pixel = image.pixel(x, y);
      hash = (hash ^ pixel) * 1099511628211ULL;
      rgb_sum += static_cast<quint64>(qRed(pixel) + qGreen(pixel) + qBlue(pixel));
    }
  }
  return {hash, rgb_sum};
}

extern "C" EMSCRIPTEN_KEEPALIVE void pj_wasm_test_report_scene3d_foundation() {
  static std::vector<ObservedView> observed_views;
  static quint64 next_identity = 1;
  static quint64 sequence = 0;

  for (QWidget* widget : QApplication::topLevelWidgets()) {
    auto* main_window = qobject_cast<PJ::MainWindow*>(widget);
    if (main_window == nullptr) {
      continue;
    }

    QList<PJ::Scene3DDockWidget*> docks;
    for (auto* dock : main_window->findChildren<PJ::Scene3DDockWidget*>()) {
      if (dock != nullptr && dock->isVisibleTo(main_window)) {
        docks.push_back(dock);
      }
    }
    std::sort(docks.begin(), docks.end(), [main_window](const auto* lhs, const auto* rhs) {
      const QPoint lhs_position = lhs->mapTo(main_window, QPoint(0, 0));
      const QPoint rhs_position = rhs->mapTo(main_window, QPoint(0, 0));
      return lhs_position.x() != rhs_position.x() ? lhs_position.x() < rhs_position.x()
                                                  : lhs_position.y() < rhs_position.y();
    });

    const quint64 current_sequence = ++sequence;
    auto* slider = main_window->findChild<PJ::RealSlider*>(u"timeSlider"_s);
    const QRect slider_geometry = geometryRelativeTo(main_window, slider);
    qInfo(
        "PJ_WASM_SCENE3D_FOUNDATION_SUMMARY sequence=%llu docks=%lld slider=%d,%d,%dx%d "
        "value=%.17g range=%.17g,%.17g enabled=%d",
        static_cast<unsigned long long>(current_sequence), static_cast<long long>(docks.size()), slider_geometry.x(),
        slider_geometry.y(), slider_geometry.width(), slider_geometry.height(),
        slider != nullptr ? slider->getValue() : 0.0, slider != nullptr ? slider->getMinimum() : 0.0,
        slider != nullptr ? slider->getMaximum() : 0.0, slider != nullptr && slider->isEnabled() ? 1 : 0);

    for (qsizetype index = 0; index < docks.size(); ++index) {
      auto* dock = docks[index];
      auto* view = dock->sceneView();
      quint64 identity = 0;
      if (view != nullptr) {
        const auto observed = std::find_if(
            observed_views.cbegin(), observed_views.cend(), [view](const auto& entry) { return entry.view == view; });
        if (observed != observed_views.cend()) {
          identity = observed->identity;
        } else {
          identity = next_identity++;
          observed_views.push_back({identity, view});
        }
      }

      QStringList frame_names;
      for (const auto& frame : dock->availableFrames()) {
        frame_names.push_back(QString::fromStdString(frame.name));
      }
      const QByteArray encoded_frames = QUrl::toPercentEncoding(frame_names.join(u','));
      const QByteArray encoded_fixed = QUrl::toPercentEncoding(dock->currentFixedFrame());
      const QByteArray encoded_follow = QUrl::toPercentEncoding(dock->currentFollowFrame());
      auto* fixed_combo = dock->findChild<QComboBox*>(u"scene3dFixedFrameCombo"_s);
      auto* camera_combo = dock->findChild<QComboBox*>(u"scene3dCameraModelCombo"_s);
      auto* home_button = dock->findChild<QToolButton*>(u"cameraHomeButton"_s);
      const QRect view_geometry = geometryRelativeTo(main_window, view);
      const QRect fixed_geometry = geometryRelativeTo(main_window, fixed_combo);
      const QRect camera_geometry = geometryRelativeTo(main_window, camera_combo);
      const QRect home_geometry = geometryRelativeTo(main_window, home_button);
      const auto camera = view != nullptr ? view->camera().state() : pj::scene3d::CameraState{};
      const QImage framebuffer = view != nullptr ? view->grabFramebuffer() : QImage{};
      const auto [frame_hash, rgb_sum] = framebufferDigest(framebuffer);

      qInfo(
          "PJ_WASM_SCENE3D_FOUNDATION_DOCK sequence=%llu index=%lld identity=%llu "
          "view=%d,%d,%dx%d visible=%d frames=%s fixed=%s auto=%d follow=%s model=%d "
          "camera=%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g vertices=%d,%d resolved=%d "
          "fixed_control=%d,%d,%dx%d camera_control=%d,%d,%dx%d home=%d,%d,%dx%d "
          "frame_hash=%llu rgb_sum=%llu framebuffer=%dx%d",
          static_cast<unsigned long long>(current_sequence), static_cast<long long>(index),
          static_cast<unsigned long long>(identity), view_geometry.x(), view_geometry.y(), view_geometry.width(),
          view_geometry.height(), view != nullptr && view->isVisibleTo(main_window) ? 1 : 0, encoded_frames.constData(),
          encoded_fixed.constData(), dock->isAutoRootMode() ? 1 : 0, encoded_follow.constData(),
          view != nullptr ? static_cast<int>(view->cameraModel()) : -1, static_cast<double>(camera.focal.x),
          static_cast<double>(camera.focal.y), static_cast<double>(camera.focal.z), static_cast<double>(camera.radius),
          static_cast<double>(camera.azimuth), static_cast<double>(camera.elevation),
          static_cast<double>(camera.ortho_scale), view != nullptr ? view->lastLineVertexCountForTest() : 0,
          view != nullptr ? view->lastTriangleVertexCountForTest() : 0,
          view != nullptr ? view->lastResolvedFrameCountForTest() : 0, fixed_geometry.x(), fixed_geometry.y(),
          fixed_geometry.width(), fixed_geometry.height(), camera_geometry.x(), camera_geometry.y(),
          camera_geometry.width(), camera_geometry.height(), home_geometry.x(), home_geometry.y(),
          home_geometry.width(), home_geometry.height(), static_cast<unsigned long long>(frame_hash),
          static_cast<unsigned long long>(rgb_sum), framebuffer.width(), framebuffer.height());
    }
    return;
  }
  qWarning("PJ_WASM_SCENE3D_FOUNDATION_FAILED no main window");
}

// clang-format off
EM_JS(void, installScene3dFoundationProbe, (), {
  globalThis.pjWasmReportScene3DFoundationProbe = () =>
      Module._pj_wasm_test_report_scene3d_foundation();
});
// clang-format on

}  // namespace

namespace PJ {

void installWasmScene3dAcceptanceProbes() {
  installScene3dFoundationProbe();
}

}  // namespace PJ
