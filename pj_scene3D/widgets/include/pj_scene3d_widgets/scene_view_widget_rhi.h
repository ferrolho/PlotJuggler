#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QList>
#include <QPoint>
#include <QRhiWidget>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "pj_base/time.hpp"
#include "pj_scene3d_core/camera/camera.h"
#include "pj_scene3d_core/tf/tf_buffer.h"
#include "pj_scene3d_widgets/passes/grid_geometry.h"

class QEvent;
class QMouseEvent;
class QWheelEvent;
class QRhi;
class QRhiBuffer;
class QRhiCommandBuffer;
class QRhiGraphicsPipeline;
class QRhiShaderResourceBindings;

namespace pj::scene3d {

// WebAssembly product implementation of the public SceneViewWidget contract.
// The native class with the same name remains a QOpenGLWidget in the other
// platform branch of scene_view_widget.h.  This class owns only QRhi resources;
// camera and TF semantics continue to come from pj_scene3d_core.
class SceneViewWidget : public QRhiWidget {
  Q_OBJECT

 public:
  using GridStyle = pj::scene3d::GridStyle;
  enum class CameraModel { kOrbit, kXyOrbit, kFly, kTopDownOrtho };

  explicit SceneViewWidget(QWidget* parent = nullptr);
  ~SceneViewWidget() override;

  void setTransformBuffer(std::shared_ptr<TransformBuffer> tf);
  void setTrackerTime(PJ::Timepoint time);
  void setFixedFrame(const std::string& frame);
  [[nodiscard]] const std::string& fixedFrame() const {
    return fixed_frame_;
  }

  void setFollowFrame(const std::string& frame);
  [[nodiscard]] const std::string& followFrame() const {
    return follow_frame_;
  }
  [[nodiscard]] uint64_t followRenderKey(PJ::Timepoint time) const;
  void recenterOnFollowFrame();

  [[nodiscard]] ICamera& camera() {
    return *camera_;
  }
  [[nodiscard]] const ICamera& camera() const {
    return *camera_;
  }
  void setCameraModel(CameraModel model);
  [[nodiscard]] CameraModel cameraModel() const {
    return camera_model_;
  }
  void resetCamera();
  void setSceneBounds(const AABB& bounds);

  void setGridStyle(GridStyle style);
  [[nodiscard]] GridStyle gridStyle() const {
    return grid_style_;
  }
  void setGridDivisions(int divisions);
  [[nodiscard]] int gridDivisions() const {
    return grid_divisions_;
  }
  void setGridExtentMetres(float extent_m);
  [[nodiscard]] float gridExtentMetres() const {
    return grid_extent_m_;
  }
  void setGridVisible(bool visible);
  [[nodiscard]] bool gridVisible() const {
    return grid_visible_;
  }

  void setAxesVisible(bool visible);
  [[nodiscard]] bool axesVisible() const {
    return axes_visible_;
  }
  void setGizmoSize(float length_m);
  [[nodiscard]] float gizmoSize() const {
    return gizmo_size_m_;
  }
  void setGizmoOpacity(float opacity);
  [[nodiscard]] float gizmoOpacity() const {
    return gizmo_opacity_;
  }
  void setTfConnectionsVisible(bool visible);
  [[nodiscard]] bool tfConnectionsVisible() const {
    return tf_connections_visible_;
  }

  void refreshAvailableFrames();
  [[nodiscard]] uint64_t tfRenderKey(PJ::Timepoint time) const;

  // Observation-only browser acceptance seams.  They report the last submitted
  // product frame; no test method mutates the camera, TF buffer, or renderer.
  [[nodiscard]] int lastLineVertexCountForTest() const {
    return last_line_vertex_count_;
  }
  [[nodiscard]] int lastTriangleVertexCountForTest() const {
    return last_triangle_vertex_count_;
  }
  [[nodiscard]] int lastResolvedFrameCountForTest() const {
    return last_resolved_frame_count_;
  }

 signals:
  void framesChanged(const QList<FrameRow>& frames);
  void presentationChanged();

 protected:
  void initialize(QRhiCommandBuffer* command_buffer) override;
  void render(QRhiCommandBuffer* command_buffer) override;
  void releaseResources() override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void wheelEvent(QWheelEvent* event) override;
  void changeEvent(QEvent* event) override;

 private:
  struct Vertex {
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
    float r = 0.0F;
    float g = 0.0F;
    float b = 0.0F;
    float a = 1.0F;
  };

  void applyFollow();
  [[nodiscard]] std::string effectiveFixedFrame() const;
  void buildGeometry(const glm::dvec3& render_origin);
  void appendLine(const glm::dvec3& a, const glm::dvec3& b, const glm::vec4& color);
  void appendTriangle(const glm::dvec3& a, const glm::dvec3& b, const glm::dvec3& c, const glm::vec4& color);
  bool ensureVertexBuffer(QRhi* owner, QRhiBuffer*& buffer, quint32& capacity, quint32 required_bytes);
  void emitPresentationIfChanged(const CameraState& before);

  std::shared_ptr<TransformBuffer> tf_;
  PJ::Timepoint render_time_{};
  std::string fixed_frame_;
  std::string follow_frame_;
  glm::dvec3 follow_previous_origin_{0.0};
  bool follow_seeded_ = false;

  std::unique_ptr<ICamera> camera_{std::make_unique<OrbitCamera>()};
  CameraModel camera_model_ = CameraModel::kOrbit;
  AABB scene_bounds_{};

  GridStyle grid_style_ = GridStyle::kLines;
  float grid_extent_m_ = 10.0F;
  int grid_divisions_ = 10;
  bool grid_visible_ = true;
  bool axes_visible_ = true;
  float gizmo_size_m_ = 0.15F;
  float gizmo_opacity_ = 1.0F;
  bool tf_connections_visible_ = true;

  uint64_t last_frames_revision_ = ~uint64_t{0};
  QList<FrameRow> last_frame_list_;
  mutable std::vector<std::string> tf_render_key_frames_;

  QRhi* resource_rhi_ = nullptr;
  QRhiBuffer* uniform_buffer_ = nullptr;
  QRhiBuffer* line_buffer_ = nullptr;
  QRhiBuffer* triangle_buffer_ = nullptr;
  QRhiShaderResourceBindings* shader_resources_ = nullptr;
  QRhiGraphicsPipeline* line_pipeline_ = nullptr;
  QRhiGraphicsPipeline* triangle_pipeline_ = nullptr;
  quint32 line_buffer_capacity_ = 0;
  quint32 triangle_buffer_capacity_ = 0;
  std::vector<Vertex> line_vertices_;
  std::vector<Vertex> triangle_vertices_;
  int last_line_vertex_count_ = 0;
  int last_triangle_vertex_count_ = 0;
  int last_resolved_frame_count_ = 0;

  QPoint last_mouse_position_;
  Qt::MouseButton active_button_ = Qt::NoButton;
  bool camera_gesture_changed_ = false;
};

}  // namespace pj::scene3d
