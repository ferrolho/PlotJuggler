// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <rhi/qrhi.h>

#include <QEvent>
#include <QFile>
#include <QGuiApplication>
#include <QMatrix4x4>
#include <QMouseEvent>
#include <QPalette>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <functional>
#include <glm/gtc/quaternion.hpp>
#include <iterator>
#include <limits>
#include <utility>

#include "pj_scene3d_core/tf/tf_connections.h"
#include "pj_scene3d_widgets/passes/grid_geometry.h"
#include "pj_scene3d_widgets/scene_look_defaults.h"
#include "pj_scene3d_widgets/scene_view_widget.h"
#include "pj_widgets/FrameworkTokens.h"

// The resource object lives in a static archive.  Explicit initialization both
// retains it at link time and keeps Q_INIT_RESOURCE's generated symbol lookup in
// the global namespace.
static void initializeScene3dWasmResources() {
  Q_INIT_RESOURCE(scene3d_wasm_shaders);
}

namespace pj::scene3d {
namespace {

constexpr quint32 kInitialVertexBufferBytes = 64U * 1024U;

QColor sceneBackdropColor() {
  const QColor window_background = QGuiApplication::palette().color(QPalette::Window);
  const auto theme = PJ::theme::themeFor(window_background.lightness() >= 128);
  return PJ::theme::surface(PJ::theme::Surface::DataBackdrop, theme);
}

QShader loadShader(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    qWarning("SceneViewWidget WASM: failed to open shader %s", qPrintable(path));
    return {};
  }
  return QShader::fromSerialized(file.readAll());
}

QMatrix4x4 toQMatrix(const glm::mat4& matrix) {
  // glm stores columns; QMatrix4x4's scalar constructor is row-major.
  return QMatrix4x4(
      matrix[0][0], matrix[1][0], matrix[2][0], matrix[3][0], matrix[0][1], matrix[1][1], matrix[2][1], matrix[3][1],
      matrix[0][2], matrix[1][2], matrix[2][2], matrix[3][2], matrix[0][3], matrix[1][3], matrix[2][3], matrix[3][3]);
}

bool cameraStatesEqual(const CameraState& left, const CameraState& right) {
  return left.focal == right.focal && left.radius == right.radius && left.azimuth == right.azimuth &&
         left.elevation == right.elevation && left.fov_y == right.fov_y && left.ortho_scale == right.ortho_scale &&
         left.perspective == right.perspective;
}

uint64_t hashTransform(const Transform& transform, uint64_t seed) {
  const glm::dmat4 matrix = transform.matrix();
  uint64_t hash = seed;
  for (int column = 0; column < 4; ++column) {
    for (int row = 0; row < 4; ++row) {
      uint64_t bits = 0;
      std::memcpy(&bits, &matrix[column][row], sizeof(bits));
      hash = (hash ^ bits) * 0x100000001b3ULL;
    }
  }
  return hash;
}

}  // namespace

SceneViewWidget::SceneViewWidget(QWidget* parent) : QRhiWidget(parent) {
  static const bool resources_initialized = [] {
    initializeScene3dWasmResources();
    return true;
  }();
  (void)resources_initialized;

  // QRhiWidget's API is immutable after platform resources exist.  Set it while
  // the widget is still parentless; SceneDockWidget reparents only after this
  // constructor returns.
  setApi(Api::OpenGL);
  setSampleCount(4);
  setObjectName(QStringLiteral("scene3dRhiCanvas"));
  setMinimumSize(320, 240);
  setFocusPolicy(Qt::StrongFocus);
  setMouseTracking(true);
  connect(this, &QRhiWidget::renderFailed, this, []() { qWarning("SceneViewWidget WASM: QRhi render failed"); });
}

SceneViewWidget::~SceneViewWidget() {
  releaseResources();
}

void SceneViewWidget::setTransformBuffer(std::shared_ptr<TransformBuffer> tf) {
  if (tf_ == tf) {
    return;
  }
  tf_ = std::move(tf);
  last_frames_revision_ = ~uint64_t{0};
  refreshAvailableFrames();
  update();
}

void SceneViewWidget::setTrackerTime(PJ::Timepoint time) {
  if (render_time_ == time) {
    return;
  }
  render_time_ = time;
  refreshAvailableFrames();
  applyFollow();
  update();
}

void SceneViewWidget::setFixedFrame(const std::string& frame) {
  if (fixed_frame_ == frame) {
    return;
  }
  fixed_frame_ = frame;
  follow_seeded_ = false;
  update();
  emit presentationChanged();
}

void SceneViewWidget::setFollowFrame(const std::string& frame) {
  if (follow_frame_ == frame) {
    return;
  }
  follow_frame_ = frame;
  follow_seeded_ = false;
  applyFollow();
  update();
}

void SceneViewWidget::applyFollow() {
  const std::string fixed = effectiveFixedFrame();
  if (follow_frame_.empty() || tf_ == nullptr || fixed.empty()) {
    return;
  }
  const auto transform = tf_->tryLookupTransform(fixed, follow_frame_, render_time_);
  if (!transform) {
    follow_seeded_ = false;
    return;
  }
  if (!follow_seeded_) {
    follow_previous_origin_ = transform->t;
    follow_seeded_ = true;
    return;
  }
  const glm::dvec3 delta = transform->t - follow_previous_origin_;
  follow_previous_origin_ = transform->t;
  if (delta != glm::dvec3(0.0)) {
    camera_->followShift(glm::vec3(delta));
  }
}

void SceneViewWidget::recenterOnFollowFrame() {
  const std::string fixed = effectiveFixedFrame();
  if (follow_frame_.empty() || tf_ == nullptr || fixed.empty()) {
    return;
  }
  const auto transform = tf_->tryLookupTransform(fixed, follow_frame_, render_time_);
  if (!transform) {
    return;
  }
  const CameraState before = camera_->state();
  camera_->followShift(glm::vec3(transform->t) - before.focal);
  follow_previous_origin_ = transform->t;
  follow_seeded_ = true;
  update();
  emitPresentationIfChanged(before);
}

uint64_t SceneViewWidget::followRenderKey(PJ::Timepoint time) const {
  const std::string fixed = effectiveFixedFrame();
  if (follow_frame_.empty() || tf_ == nullptr || fixed.empty()) {
    return 0;
  }
  const auto transform = tf_->tryLookupTransform(fixed, follow_frame_, time);
  if (!transform) {
    return 0;
  }
  uint64_t hash = 1469598103934665603ULL;
  for (const double component : {transform->t.x, transform->t.y, transform->t.z}) {
    uint64_t bits = 0;
    std::memcpy(&bits, &component, sizeof(bits));
    hash = (hash ^ bits) * 1099511628211ULL;
  }
  return hash == 0 ? 1U : hash;
}

void SceneViewWidget::setCameraModel(CameraModel model) {
  if (camera_model_ == model) {
    return;
  }
  const CameraState state = camera_->state();
  std::unique_ptr<ICamera> replacement;
  switch (model) {
    case CameraModel::kOrbit:
      replacement = std::make_unique<OrbitCamera>();
      break;
    case CameraModel::kXyOrbit:
      replacement = std::make_unique<XYOrbitCamera>();
      break;
    case CameraModel::kFly:
      replacement = std::make_unique<FlyCamera>();
      break;
    case CameraModel::kTopDownOrtho:
      replacement = std::make_unique<TopDownOrthoCamera>();
      break;
  }
  replacement->adoptState(state);
  replacement->setSceneBounds(scene_bounds_);
  camera_ = std::move(replacement);
  camera_model_ = model;
  update();
  emit presentationChanged();
}

void SceneViewWidget::resetCamera() {
  const CameraState before = camera_->state();
  camera_->reset();
  update();
  emitPresentationIfChanged(before);
}

void SceneViewWidget::setSceneBounds(const AABB& bounds) {
  scene_bounds_ = bounds;
  camera_->setSceneBounds(bounds);
}

void SceneViewWidget::setGridStyle(GridStyle style) {
  if (grid_style_ == style) {
    return;
  }
  grid_style_ = style;
  update();
  emit presentationChanged();
}

void SceneViewWidget::setGridDivisions(int divisions) {
  divisions = std::clamp(divisions, 1, 200);
  if (grid_divisions_ == divisions) {
    return;
  }
  grid_divisions_ = divisions;
  update();
  emit presentationChanged();
}

void SceneViewWidget::setGridExtentMetres(float extent_m) {
  extent_m = std::clamp(extent_m, 0.01F, 1.0e7F);
  if (grid_extent_m_ == extent_m) {
    return;
  }
  grid_extent_m_ = extent_m;
  update();
  emit presentationChanged();
}

void SceneViewWidget::setGridVisible(bool visible) {
  if (grid_visible_ == visible) {
    return;
  }
  grid_visible_ = visible;
  update();
  emit presentationChanged();
}

void SceneViewWidget::setAxesVisible(bool visible) {
  if (axes_visible_ == visible) {
    return;
  }
  axes_visible_ = visible;
  update();
  emit presentationChanged();
}

void SceneViewWidget::setGizmoSize(float length_m) {
  length_m = std::clamp(length_m, 0.001F, 1.0e5F);
  if (gizmo_size_m_ == length_m) {
    return;
  }
  gizmo_size_m_ = length_m;
  update();
  emit presentationChanged();
}

void SceneViewWidget::setGizmoOpacity(float opacity) {
  opacity = std::clamp(opacity, 0.0F, 1.0F);
  if (gizmo_opacity_ == opacity) {
    return;
  }
  gizmo_opacity_ = opacity;
  update();
  emit presentationChanged();
}

void SceneViewWidget::setTfConnectionsVisible(bool visible) {
  if (tf_connections_visible_ == visible) {
    return;
  }
  tf_connections_visible_ = visible;
  update();
  emit presentationChanged();
}

void SceneViewWidget::refreshAvailableFrames() {
  QList<FrameRow> rows;
  if (tf_ != nullptr) {
    const uint64_t revision = tf_->revision();
    if (revision == last_frames_revision_) {
      return;
    }
    last_frames_revision_ = revision;
    for (FrameRow& row : tf_->getFrameHierarchy()) {
      rows.append(std::move(row));
    }
  } else {
    last_frames_revision_ = ~uint64_t{0};
  }
  if (rows != last_frame_list_) {
    last_frame_list_ = rows;
    emit framesChanged(rows);
  }
}

uint64_t SceneViewWidget::tfRenderKey(PJ::Timepoint time) const {
  const std::string fixed = effectiveFixedFrame();
  if (tf_ == nullptr || fixed.empty() || (!axes_visible_ && !tf_connections_visible_)) {
    return 0;
  }
  tf_->getAllFrames(tf_render_key_frames_);
  uint64_t combined = 0;
  for (const std::string& frame : tf_render_key_frames_) {
    uint64_t hash = std::hash<std::string>{}(frame);
    if (const auto transform = tf_->tryLookupTransform(fixed, frame, time); transform) {
      hash = hashTransform(*transform, hash);
    } else {
      hash ^= 0xD15C0FFEEULL;
    }
    combined ^= hash ^ (hash >> 29U);
  }
  return combined;
}

std::string SceneViewWidget::effectiveFixedFrame() const {
  if (!fixed_frame_.empty()) {
    return fixed_frame_;
  }
  if (tf_ == nullptr) {
    return {};
  }
  const std::vector<FrameRow> hierarchy = tf_->getFrameHierarchy();
  return hierarchy.empty() ? std::string{} : hierarchy.front().name;
}

void SceneViewWidget::appendLine(const glm::dvec3& a, const glm::dvec3& b, const glm::vec4& color) {
  const auto vertex = [&color](const glm::dvec3& point) {
    return Vertex{
        static_cast<float>(point.x),
        static_cast<float>(point.y),
        static_cast<float>(point.z),
        color.r,
        color.g,
        color.b,
        color.a};
  };
  line_vertices_.push_back(vertex(a));
  line_vertices_.push_back(vertex(b));
}

void SceneViewWidget::appendTriangle(
    const glm::dvec3& a, const glm::dvec3& b, const glm::dvec3& c, const glm::vec4& color) {
  const auto vertex = [&color](const glm::dvec3& point) {
    return Vertex{
        static_cast<float>(point.x),
        static_cast<float>(point.y),
        static_cast<float>(point.z),
        color.r,
        color.g,
        color.b,
        color.a};
  };
  triangle_vertices_.push_back(vertex(a));
  triangle_vertices_.push_back(vertex(b));
  triangle_vertices_.push_back(vertex(c));
}

void SceneViewWidget::buildGeometry(const glm::dvec3& render_origin) {
  line_vertices_.clear();
  triangle_vertices_.clear();
  last_resolved_frame_count_ = 0;

  if (grid_visible_) {
    if (grid_style_ == GridStyle::kFilledCells) {
      const std::vector<GridVertex> cells = buildCheckerboardCells(grid_extent_m_, grid_divisions_);
      for (std::size_t index = 0; index + 2 < cells.size(); index += 3) {
        const glm::vec4 color =
            cells[index].parity < 0.5F ? glm::vec4(look::kGridCellToneA, 1.0F) : glm::vec4(look::kGridCellToneB, 1.0F);
        appendTriangle(
            glm::dvec3(cells[index].pos) - render_origin, glm::dvec3(cells[index + 1].pos) - render_origin,
            glm::dvec3(cells[index + 2].pos) - render_origin, color);
      }
    }
    for (const GridVertex& vertex : buildGridLines(grid_extent_m_, grid_divisions_)) {
      const glm::dvec3 point = glm::dvec3(vertex.pos) - render_origin;
      line_vertices_.push_back(
          Vertex{
              static_cast<float>(point.x), static_cast<float>(point.y), static_cast<float>(point.z),
              look::kGridLineColor.r, look::kGridLineColor.g, look::kGridLineColor.b, 1.0F});
    }
  }

  const std::string fixed = effectiveFixedFrame();
  if (tf_ != nullptr && !fixed.empty()) {
    if (tf_connections_visible_) {
      std::vector<TfConnectionSegment> segments;
      buildTfConnectionSegments(*tf_, fixed, render_time_, segments, render_origin);
      for (const TfConnectionSegment& segment : segments) {
        appendLine(glm::dvec3(segment.child), glm::dvec3(segment.parent), glm::vec4(look::kTfConnectionColor, 1.0F));
      }
    }
    if (axes_visible_) {
      for (const std::string& frame : tf_->getAllFrames()) {
        const auto transform = tf_->tryLookupTransform(fixed, frame, render_time_);
        if (!transform) {
          continue;
        }
        ++last_resolved_frame_count_;
        const glm::dvec3 origin = transform->t - render_origin;
        const double size = static_cast<double>(gizmo_size_m_);
        appendLine(
            origin, (*transform * glm::dvec3(size, 0.0, 0.0)) - render_origin,
            glm::vec4(look::kAxisTriadX, gizmo_opacity_));
        appendLine(
            origin, (*transform * glm::dvec3(0.0, size, 0.0)) - render_origin,
            glm::vec4(look::kAxisTriadY, gizmo_opacity_));
        appendLine(
            origin, (*transform * glm::dvec3(0.0, 0.0, size)) - render_origin,
            glm::vec4(look::kAxisTriadZ, gizmo_opacity_));
      }
    }
  }

  last_line_vertex_count_ = static_cast<int>(line_vertices_.size());
  last_triangle_vertex_count_ = static_cast<int>(triangle_vertices_.size());
}

bool SceneViewWidget::ensureVertexBuffer(QRhi* owner, QRhiBuffer*& buffer, quint32& capacity, quint32 required_bytes) {
  if (required_bytes == 0) {
    return true;
  }
  if (buffer != nullptr && capacity >= required_bytes) {
    return true;
  }
  quint32 next_capacity = std::max(capacity, kInitialVertexBufferBytes);
  while (next_capacity < required_bytes && next_capacity <= std::numeric_limits<quint32>::max() / 2U) {
    next_capacity *= 2U;
  }
  if (next_capacity < required_bytes) {
    return false;
  }
  delete buffer;
  buffer = owner->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::VertexBuffer, next_capacity);
  if (!buffer->create()) {
    delete buffer;
    buffer = nullptr;
    capacity = 0;
    return false;
  }
  capacity = next_capacity;
  return true;
}

void SceneViewWidget::initialize(QRhiCommandBuffer* /*command_buffer*/) {
  QRhi* current_rhi = rhi();
  if (current_rhi == nullptr || renderTarget() == nullptr) {
    return;
  }
  if (resource_rhi_ != current_rhi) {
    releaseResources();
    resource_rhi_ = current_rhi;
  }
  if (line_pipeline_ != nullptr && triangle_pipeline_ != nullptr) {
    return;
  }

  const QShader vertex_shader = loadShader(QStringLiteral(":/scene3d_wasm/scene.vert.qsb"));
  const QShader fragment_shader = loadShader(QStringLiteral(":/scene3d_wasm/scene.frag.qsb"));
  if (!vertex_shader.isValid() || !fragment_shader.isValid()) {
    return;
  }

  uniform_buffer_ = current_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, 64);
  if (!uniform_buffer_->create()) {
    releaseResources();
    return;
  }
  shader_resources_ = current_rhi->newShaderResourceBindings();
  shader_resources_->setBindings({
      QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::VertexStage, uniform_buffer_),
  });
  if (!shader_resources_->create()) {
    releaseResources();
    return;
  }

  QRhiVertexInputLayout layout;
  layout.setBindings({QRhiVertexInputBinding(sizeof(Vertex))});
  layout.setAttributes({
      QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float3, offsetof(Vertex, x)),
      QRhiVertexInputAttribute(0, 1, QRhiVertexInputAttribute::Float4, offsetof(Vertex, r)),
  });
  QRhiGraphicsPipeline::TargetBlend blend;
  blend.enable = true;
  blend.srcColor = QRhiGraphicsPipeline::SrcAlpha;
  blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
  blend.srcAlpha = QRhiGraphicsPipeline::One;
  blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;

  const auto create_pipeline = [&](QRhiGraphicsPipeline::Topology topology) {
    QRhiGraphicsPipeline* pipeline = current_rhi->newGraphicsPipeline();
    pipeline->setTopology(topology);
    pipeline->setShaderStages({
        {QRhiShaderStage::Vertex, vertex_shader},
        {QRhiShaderStage::Fragment, fragment_shader},
    });
    pipeline->setVertexInputLayout(layout);
    pipeline->setShaderResourceBindings(shader_resources_);
    pipeline->setTargetBlends({blend});
    pipeline->setCullMode(QRhiGraphicsPipeline::None);
    pipeline->setDepthTest(true);
    pipeline->setDepthWrite(true);
    pipeline->setDepthOp(QRhiGraphicsPipeline::LessOrEqual);
    pipeline->setSampleCount(sampleCount());
    pipeline->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
    if (!pipeline->create()) {
      delete pipeline;
      return static_cast<QRhiGraphicsPipeline*>(nullptr);
    }
    return pipeline;
  };
  line_pipeline_ = create_pipeline(QRhiGraphicsPipeline::Lines);
  triangle_pipeline_ = create_pipeline(QRhiGraphicsPipeline::Triangles);
  if (line_pipeline_ == nullptr || triangle_pipeline_ == nullptr) {
    releaseResources();
  }
}

void SceneViewWidget::render(QRhiCommandBuffer* command_buffer) {
  QRhi* current_rhi = rhi();
  QRhiRenderTarget* target = renderTarget();
  if (current_rhi == nullptr || target == nullptr) {
    return;
  }

  const glm::dvec3 render_origin(camera_->state().focal);
  buildGeometry(render_origin);
  const quint32 line_bytes = static_cast<quint32>(line_vertices_.size() * sizeof(Vertex));
  const quint32 triangle_bytes = static_cast<quint32>(triangle_vertices_.size() * sizeof(Vertex));
  const bool buffers_ready =
      ensureVertexBuffer(current_rhi, line_buffer_, line_buffer_capacity_, line_bytes) &&
      ensureVertexBuffer(current_rhi, triangle_buffer_, triangle_buffer_capacity_, triangle_bytes);

  QRhiResourceUpdateBatch* updates = current_rhi->nextResourceUpdateBatch();
  if (buffers_ready && line_bytes != 0) {
    updates->updateDynamicBuffer(line_buffer_, 0, line_bytes, line_vertices_.data());
  }
  if (buffers_ready && triangle_bytes != 0) {
    updates->updateDynamicBuffer(triangle_buffer_, 0, triangle_bytes, triangle_vertices_.data());
  }

  const QSize output_size = target->pixelSize();
  const float aspect = output_size.height() > 0
                           ? static_cast<float>(output_size.width()) / static_cast<float>(output_size.height())
                           : 1.0F;
  const glm::mat4 view_projection = camera_->projMatrix(aspect) * camera_->viewMatrixRelativeTo(render_origin);
  const QMatrix4x4 matrix = current_rhi->clipSpaceCorrMatrix() * toQMatrix(view_projection);
  if (uniform_buffer_ != nullptr) {
    updates->updateDynamicBuffer(uniform_buffer_, 0, 64, matrix.constData());
  }

  command_buffer->beginPass(target, sceneBackdropColor(), {1.0F, 0}, updates);
  if (buffers_ready && shader_resources_ != nullptr) {
    const QRhiViewport viewport(
        0.0F, 0.0F, static_cast<float>(output_size.width()), static_cast<float>(output_size.height()));
    if (triangle_bytes != 0 && triangle_pipeline_ != nullptr) {
      command_buffer->setGraphicsPipeline(triangle_pipeline_);
      command_buffer->setViewport(viewport);
      command_buffer->setShaderResources(shader_resources_);
      const QRhiCommandBuffer::VertexInput input(triangle_buffer_, 0);
      command_buffer->setVertexInput(0, 1, &input);
      command_buffer->draw(static_cast<quint32>(triangle_vertices_.size()));
    }
    if (line_bytes != 0 && line_pipeline_ != nullptr) {
      command_buffer->setGraphicsPipeline(line_pipeline_);
      command_buffer->setViewport(viewport);
      command_buffer->setShaderResources(shader_resources_);
      const QRhiCommandBuffer::VertexInput input(line_buffer_, 0);
      command_buffer->setVertexInput(0, 1, &input);
      command_buffer->draw(static_cast<quint32>(line_vertices_.size()));
    }
  }
  command_buffer->endPass();
}

void SceneViewWidget::releaseResources() {
  delete line_pipeline_;
  delete triangle_pipeline_;
  delete shader_resources_;
  delete line_buffer_;
  delete triangle_buffer_;
  delete uniform_buffer_;
  line_pipeline_ = nullptr;
  triangle_pipeline_ = nullptr;
  shader_resources_ = nullptr;
  line_buffer_ = nullptr;
  triangle_buffer_ = nullptr;
  uniform_buffer_ = nullptr;
  line_buffer_capacity_ = 0;
  triangle_buffer_capacity_ = 0;
  resource_rhi_ = nullptr;
}

void SceneViewWidget::mousePressEvent(QMouseEvent* event) {
  last_mouse_position_ = event->position().toPoint();
  active_button_ = event->button();
  camera_gesture_changed_ = false;
  event->accept();
}

void SceneViewWidget::mouseReleaseEvent(QMouseEvent* event) {
  if (event->button() == active_button_) {
    active_button_ = Qt::NoButton;
    if (camera_gesture_changed_) {
      camera_gesture_changed_ = false;
      emit presentationChanged();
    }
  }
  event->accept();
}

void SceneViewWidget::mouseMoveEvent(QMouseEvent* event) {
  const QPoint position = event->position().toPoint();
  const float delta_x = static_cast<float>(position.x() - last_mouse_position_.x());
  const float delta_y = static_cast<float>(position.y() - last_mouse_position_.y());
  last_mouse_position_ = position;
  if (active_button_ == Qt::LeftButton) {
    camera_->rotate(delta_x, delta_y);
  } else if (active_button_ == Qt::RightButton || active_button_ == Qt::MiddleButton) {
    camera_->pan(delta_x, delta_y);
  } else {
    return;
  }
  camera_gesture_changed_ = true;
  update();
  event->accept();
}

void SceneViewWidget::wheelEvent(QWheelEvent* event) {
  const float ticks = static_cast<float>(event->angleDelta().y()) / 120.0F;
  if (ticks == 0.0F) {
    return;
  }
  const CameraState before = camera_->state();
  camera_->zoomToCursor(ticks, glm::vec2(event->position().x(), event->position().y()), width(), height());
  update();
  emitPresentationIfChanged(before);
  event->accept();
}

void SceneViewWidget::changeEvent(QEvent* event) {
  QRhiWidget::changeEvent(event);
  if (event->type() == QEvent::PaletteChange) {
    update();
  }
}

void SceneViewWidget::emitPresentationIfChanged(const CameraState& before) {
  if (!cameraStatesEqual(before, camera_->state())) {
    emit presentationChanged();
  }
}

}  // namespace pj::scene3d
