// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "pj_scene3d_widgets/passes/voxel_grid_render_pass.h"

#include <QLoggingCategory>
#include <QString>
#include <algorithm>
#include <array>
#include <cstdint>
#include <glm/glm.hpp>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

#include "pj_scene3d_core/cube_draw_policy.h"
#include "pj_scene3d_core/scene_entities_decode.h"  // poseToMat4
#include "pj_scene3d_core/tf/transform.h"
#include "pj_scene3d_widgets/cube_mesh.h"  // shared CubeVertex / kCubeVertices / kCubeIndices / kCubeEdgeGlsl
#include "pj_scene3d_widgets/gl/gl_functions.h"

namespace pj::scene3d {
namespace {

Q_LOGGING_CATEGORY(lcVoxelPass, "pj.scene3d.voxel_grid_pass")

// Vertex shader: one instance per voxel. Derives (cx,ry,sz) from gl_InstanceID,
// samples the value from a 3D texture, applies the draw predicate (culled voxels
// collapse to an out-of-clip degenerate so they rasterize nothing), and places a
// unit cube scaled by cell_size at the voxel centre in the grid's local frame.
constexpr std::string_view kVertSrc = R"(#version 450 core
layout(location = 0) in vec3 in_corner_pos;     // unit cube corner (+/-0.5)
layout(location = 1) in vec3 in_corner_normal;   // outward face normal (grid-local axes)

uniform mat4 u_model;       // grid-local -> fixed-frame (TF * poseToMat4(origin))
// Folded on the CPU. Written as `u_proj * u_view * world` these are mat4xmat4
// products the driver is not obliged to hoist out of a shader that runs once per
// voxel CORNER — 24 times per voxel, for every voxel in the lattice.
uniform mat4 u_viewproj;         // proj * view
uniform mat3 u_normal_to_view;   // mat3(view * model), for the corner normals
uniform vec3 u_cell_size;   // metric voxel size (x,y,z)
uniform ivec3 u_dims;       // column_count, row_count, slice_count
uniform sampler3D u_volume; // R32F (scalar) or RGBA8 (rgba) — float sampler either way
uniform int  u_value_kind;  // 0 = scalar, 1 = rgba
uniform int  u_draw_mode;   // 0 all, 1 nonzero, 2 threshold, 3 range
uniform float u_threshold;
uniform float u_range_lo;   // predicate range (mode 3)
uniform float u_range_hi;
uniform float u_color_lo;   // colormap normalization range
uniform float u_color_hi;

out vec3 v_view_normal;
out float v_normalized;
out vec3 v_color;
out vec3 v_local;  // unit-cube corner, for the fragment-shader edge outline

bool predicate(int mode, float value) {
  if (mode == 0) return true;
  if (mode == 1) return value != 0.0;
  if (mode == 2) return value >= u_threshold;
  return value >= u_range_lo && value <= u_range_hi;
}

void main() {
  v_local = in_corner_pos;
  int w = u_dims.x;
  int h = u_dims.y;
  int id = gl_InstanceID;
  int cx = id % w;
  int ry = (id / w) % h;
  int sz = id / (w * h);
  vec4 texel = texelFetch(u_volume, ivec3(cx, ry, sz), 0);

  bool draw;
  if (u_value_kind == 1) {
    v_color = texel.rgb;
    v_normalized = 0.0;
    draw = (u_draw_mode == 0) ? true : (texel.a > 0.001);
  } else {
    float value = texel.r;
    v_color = vec3(0.0);
    float span = max(u_color_hi - u_color_lo, 1e-9);
    v_normalized = clamp((value - u_color_lo) / span, 0.0, 1.0);
    draw = predicate(u_draw_mode, value);
  }

  if (!draw) {
    gl_Position = vec4(2.0, 2.0, 2.0, 1.0);  // outside NDC -> clipped (degenerate)
    v_view_normal = vec3(0.0, 0.0, 1.0);
    return;
  }

  vec3 local_center = (vec3(cx, ry, sz) + 0.5) * u_cell_size;
  vec3 local_vertex = local_center + in_corner_pos * u_cell_size;
  vec4 world = u_model * vec4(local_vertex, 1.0);
  gl_Position = u_viewproj * world;
  v_view_normal = u_normal_to_view * in_corner_normal;
}
)";

// Fragment shader head + shared colormap GLSL (PJ::colormapGlsl) + tail, same
// split the pointcloud cube pass uses so the hand-tuned LUT polynomials live in
// exactly one place.
constexpr std::string_view kFragHead = R"(#version 450 core
in vec3 v_view_normal;
in float v_normalized;
in vec3 v_color;
in vec3 v_local;
out vec4 frag_color;

uniform int  u_value_kind;   // 0 = scalar (colormap), 1 = rgba (direct)
uniform int  u_colormap_id;  // 0=turbo, 1=viridis, 2=plasma, 3=grayscale
uniform bool u_invert;
uniform float u_opacity;
)";

constexpr std::string_view kFragTail = R"(
void main() {
  // The shared key light from cube_mesh.h, not a copy of it: a voxel and a point-cloud
  // cube sitting side by side have to be lit identically.
  float shading = cubeLambert(normalize(v_view_normal));

  vec3 base;
  if (u_value_kind == 1) {
    base = v_color;
  } else {
    float t = u_invert ? 1.0 - v_normalized : v_normalized;
    base = sampleColormap(u_colormap_id, t);
  }
  // Darker outline on each voxel's faces, so adjacent same-coloured voxels stay legible.
  // Written as a SCALE (the same form the cube pass uses) against the shared strength
  // constant — mix(b, b*0.4, e) is exactly b*(1 - 0.6*e).
  base *= 1.0 - kCubeEdgeDarken * cubeEdgeFactor(v_local);
  // Colormaps/colours are display-referred sRGB; the scene FBO is linear. Linearize.
  base = pow(max(base, vec3(0.0)), vec3(2.2));
  frag_color = vec4(base * shading, u_opacity);
}
)";

std::string makeFragSrc() {
  return std::string(kFragHead) + std::string(PJ::colormapGlsl()) + std::string(kCubeLightingGlsl) +
         std::string(kCubeEdgeGlsl) + std::string(kFragTail);
}

}  // namespace

VoxelGridRenderPass::VoxelGridRenderPass() = default;
VoxelGridRenderPass::~VoxelGridRenderPass() = default;

void VoxelGridRenderPass::initializeGL() {
  if (initialized_) {
    return;
  }
  initialized_ = true;
  static const std::string frag_src = makeFragSrc();
  auto result = gl::Program::fromSources(kVertSrc, frag_src);
  if (auto* program = std::get_if<gl::Program>(&result)) {
    program_ = std::make_unique<gl::Program>(std::move(*program));
  } else {
    qCCritical(lcVoxelPass) << "VoxelGrid shader build failed:" << QString::fromStdString(std::get<std::string>(result))
                            << "— voxel grids will not render on this GL context.";
    return;  // render() no-ops (program_ stays null)
  }
  cube_vbo_.uploadStatic(
      GL_ARRAY_BUFFER, kCubeVertices.data(), static_cast<GLsizeiptr>(kCubeVertices.size() * sizeof(CubeVertex)));
  withGlFunctions([this](auto& functions) {
    vao_.bind();
    cube_vbo_.bind(GL_ARRAY_BUFFER);
    functions.glEnableVertexAttribArray(0U);
    functions.glVertexAttribPointer(0U, 3, GL_FLOAT, GL_FALSE, static_cast<GLsizei>(sizeof(CubeVertex)), nullptr);
    functions.glEnableVertexAttribArray(1U);
    functions.glVertexAttribPointer(
        1U, 3, GL_FLOAT, GL_FALSE, static_cast<GLsizei>(sizeof(CubeVertex)), reinterpret_cast<const void*>(12));
    // The element buffer binding is captured into the bound VAO's state.
    cube_ebo_.uploadStatic(
        GL_ELEMENT_ARRAY_BUFFER, kCubeIndices.data(), static_cast<GLsizeiptr>(kCubeIndices.size() * sizeof(uint16_t)));
    vao_.unbind();
  });
}

void VoxelGridRenderPass::setGrid(VoxelGridUpload upload) {
  frame_id_ = std::move(upload.frame_id);
  origin_ = upload.origin;
  cell_size_ = upload.cell_size;
  cols_ = upload.column_count;
  rows_ = upload.row_count;
  slices_ = upload.slice_count;
  kind_ = upload.kind;
  scalar_ = std::move(upload.scalar);
  rgba_ = std::move(upload.rgba);
  const uint64_t count = static_cast<uint64_t>(cols_) * rows_ * slices_;
  const bool have_payload = kind_ == VoxelValueKind::kScalar ? scalar_.size() == count : rgba_.size() == count * 4U;
  has_grid_ = count > 0U && have_payload;
  pending_full_ = has_grid_;
}

void VoxelGridRenderPass::clearGrid() {
  has_grid_ = false;
}

void VoxelGridRenderPass::releaseGL() {
  program_.reset();
  vao_ = gl::VertexArray{};
  cube_vbo_ = gl::Buffer{};
  cube_ebo_ = gl::Buffer{};
  volume_ = gl::Texture3D{};
  initialized_ = false;
  // CPU payload survives; re-arm a full re-upload so render() repopulates the
  // volume after initializeGL rebuilds the resources in the new context.
  pending_full_ = has_grid_;
}

void VoxelGridRenderPass::uploadPending() {
  // Guard against drivers' 3D-texture size cap; an over-large grid can't be a
  // single 3D texture (a tiled/compute path is a documented follow-up).
  GLint max_dim = 0;
  withGlFunctions([&max_dim](auto& functions) { functions.glGetIntegerv(GL_MAX_3D_TEXTURE_SIZE, &max_dim); });
  if (max_dim > 0 && (cols_ > static_cast<uint32_t>(max_dim) || rows_ > static_cast<uint32_t>(max_dim) ||
                      slices_ > static_cast<uint32_t>(max_dim))) {
    qCWarning(lcVoxelPass) << "VoxelGrid" << cols_ << "x" << rows_ << "x" << slices_ << "exceeds GL_MAX_3D_TEXTURE_SIZE"
                           << max_dim << "— not rendered.";
    has_grid_ = false;
    pending_full_ = false;
    return;
  }
  // glDrawElementsInstanced takes a signed GLsizei instance count. A grid with
  // more voxels than GLsizei can hold (a >2.1B-voxel RGBA8 grid is reachable under
  // GL_MAX_3D_TEXTURE_SIZE) would wrap negative and silently draw nothing — refuse.
  if (static_cast<uint64_t>(cols_) * rows_ * slices_ > static_cast<uint64_t>(std::numeric_limits<GLsizei>::max())) {
    qCWarning(lcVoxelPass) << "VoxelGrid voxel count" << (static_cast<uint64_t>(cols_) * rows_ * slices_)
                           << "exceeds GLsizei max — not rendered.";
    has_grid_ = false;
    pending_full_ = false;
    return;
  }

  if (kind_ == VoxelValueKind::kScalar) {
    auto [lo, hi] = std::minmax_element(scalar_.begin(), scalar_.end());
    auto_lo_ = scalar_.empty() ? 0.0f : *lo;
    auto_hi_ = scalar_.empty() ? 1.0f : *hi;
    volume_.upload(GL_R32F, GL_RED, GL_FLOAT, cols_, rows_, slices_, scalar_.data());
  } else {
    auto_lo_ = 0.0f;
    auto_hi_ = 1.0f;
    volume_.upload(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, cols_, rows_, slices_, rgba_.data());
  }
  pending_full_ = false;
}

void VoxelGridRenderPass::render(const ViewParams& view_params, const FrameContext& frame_ctx) {
  if (!visible_ || !has_grid_) {
    return;
  }
  initializeGL();
  if (program_ == nullptr) {
    return;
  }
  if (pending_full_) {
    uploadPending();
    if (!has_grid_) {
      return;  // upload refused (over-cap)
    }
  }

  const auto transform = frame_ctx.lookup(frame_id_);
  if (!transform) {
    return;  // grid's frame can't resolve to the fixed frame — orphan, skip
  }
  const glm::mat4 model = glm::mat4(transform->matrix()) * poseToMat4(origin_);

  // The same whole-cloud decision the point-cloud pass makes, from the same code in
  // pj_scene3d_core. Today this pass can only draw the 24-vertex solid, so what it gains
  // is the FRUSTUM REJECT — which it had none of, while paying 24 vertex invocations per
  // lattice cell for every cell in the grid, on screen or not. When the fan and the sprite
  // land here the capability flags are the only thing that changes.
  const glm::vec3 grid_extent(
      static_cast<float>(cols_) * cell_size_.x, static_cast<float>(rows_) * cell_size_.y,
      static_cast<float>(slices_) * cell_size_.z);
  const glm::vec3 half_cell = cell_size_ * 0.5f;

  CubeCloudView cloud_view;
  // Voxel CENTRES, matching the vertex shader's (index + 0.5) * cell_size placement.
  cloud_view.centre_bounds = AABB{half_cell, grid_extent - half_cell, true};
  cloud_view.clip_from_cloud = view_params.proj * view_params.view * model;
  if (const auto eye_render = eyeInRenderSpace(view_params); eye_render.has_value()) {
    cloud_view.eye_in_cloud = glm::vec3(glm::inverse(model) * glm::vec4(*eye_render, 1.0f));
  }
  // Cells may be non-cubic; the largest axis is the conservative inflation.
  cloud_view.cube_size_m = std::max({cell_size_.x, cell_size_.y, cell_size_.z});
  constexpr CubeDrawCapabilities kSolidOnly{/*sprite=*/false, /*fan=*/false, /*solid=*/true};
  if (chooseCubeDrawMode(cloud_view, kSolidOnly) == CubeDrawMode::kSkip) {
    return;
  }

  const float color_lo = auto_range_ ? auto_lo_ : manual_lo_;
  const float color_hi = auto_range_ ? auto_hi_ : manual_hi_;
  const bool opaque = opacity_ >= 0.999f;

  program_->use();
  program_->setMat4("u_model", model);
  program_->setMat4("u_viewproj", view_params.proj * view_params.view);
  program_->setMat3("u_normal_to_view", glm::mat3(view_params.view * model));
  program_->setVec3("u_cell_size", cell_size_);
  program_->setInt("u_value_kind", kind_ == VoxelValueKind::kRgba ? 1 : 0);
  program_->setInt("u_draw_mode", static_cast<int>(draw_mode_));
  program_->setFloat("u_threshold", threshold_);
  program_->setFloat("u_range_lo", manual_lo_);
  program_->setFloat("u_range_hi", manual_hi_);
  program_->setFloat("u_color_lo", color_lo);
  program_->setFloat("u_color_hi", color_hi);
  program_->setInt("u_colormap_id", static_cast<int>(colormap_));
  program_->setInt("u_invert", 0);
  program_->setFloat("u_opacity", opacity_);
  program_->setInt("u_volume", 0);
  program_->setIVec3("u_dims", glm::ivec3(static_cast<int>(cols_), static_cast<int>(rows_), static_cast<int>(slices_)));
  volume_.bind(0);
  vao_.bind();

  const auto instance_count = static_cast<GLsizei>(static_cast<uint64_t>(cols_) * rows_ * slices_);
  withGlFunctions([opaque, instance_count](auto& functions) {
    // The scene's ambient blend state: coverage-union, per the OccupancyGrid pass's
    // H.10 contract. Restored after the draw so the HUD and later passes are intact.
    auto set_coverage_blend = [&functions]() {
      functions.glEnable(GL_BLEND);
      functions.glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    };
    // Opaque voxel data draws without blending so covered samples reset the scene
    // FBO's tonemap-coverage alpha to "grade me" (same as point clouds).
    if (opaque) {
      functions.glDisable(GL_BLEND);
    } else {
      set_coverage_blend();
    }
    // The shared cube is a closed CCW-outward solid, so its back faces can never be
    // seen: culling them halves the rasterization and fragment work for a dense
    // grid. Paired with a disable — the engine's ambient state has culling off (the
    // same contract the marker pass keeps).
    functions.glEnable(GL_CULL_FACE);
    functions.glCullFace(GL_BACK);
    functions.glFrontFace(GL_CCW);
    functions.glDrawElementsInstanced(
        GL_TRIANGLES, static_cast<GLsizei>(kCubeIndices.size()), GL_UNSIGNED_SHORT, nullptr, instance_count);
    functions.glDisable(GL_CULL_FACE);
    set_coverage_blend();
  });
  vao_.unbind();
  unuseProgram();
}

}  // namespace pj::scene3d
