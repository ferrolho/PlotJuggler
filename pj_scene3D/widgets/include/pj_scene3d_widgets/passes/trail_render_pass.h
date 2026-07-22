#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <cstddef>
#include <cstdint>
#include <glm/glm.hpp>
#include <memory>
#include <vector>

#include "pj_scene3d_widgets/gl/buffer.h"
#include "pj_scene3d_widgets/gl/program.h"
#include "pj_scene3d_widgets/gl/vertex_array.h"
#include "pj_scene3d_widgets/render_pass.h"  // ViewParams

namespace pj::scene3d {

// Draws a time-sorted trail polyline as a screen-space RIBBON (a triangle
// strip, two expanded vertices per point) in two flat-color draws over ONE
// vertex buffer: points [0, past_count) in the past color and
// [past_count-1, N) in the future color (the boundary pair is shared so the
// ribbon stays continuous). Scrubbing the playhead therefore changes only the
// two draw ranges — the buffer re-uploads ONLY when the polyline, the
// camera-relative render origin, or the GL context changes: CPU points are absolute doubles,
// downcast to float AFTER subtracting the origin (large-coordinate rule).
// The ribbon exists because core GL caps real lines at 1 px (glLineWidth > 1
// is GL_INVALID_VALUE on strict core contexts); the expansion happens CPU-side
// at upload, the pixel-width offset in the vertex shader. Not an IRenderPass:
// the split draw needs custom arguments, so the owning TrailLayer sequences it
// directly (the PosesRenderPass shape).
class TrailRenderPass {
 public:
  void initializeGL();
  void releaseGL();

  // Replaces the CPU-side polyline (absolute fixed-frame positions). Always
  // bumps the upload revision; callers gate on real changes.
  void setPoints(std::vector<glm::dvec3> points);

  // past_count = number of points with stamp <= tracker time (0..size()).
  // width_px is honored exactly at any size (see the class comment for the
  // ribbon mechanics). draw_past / draw_future independently hide the two
  // halves (the per-trail eye toggles); the split index semantics are unchanged.
  void render(
      const ViewParams& view_params, const glm::dvec3& render_origin, std::size_t past_count,
      const glm::vec3& past_color, const glm::vec3& future_color, float width_px, bool draw_past, bool draw_future);

 private:
  void uploadIfNeeded(const glm::dvec3& render_origin);

  bool initialized_ = false;
  std::unique_ptr<gl::Program> program_;
  gl::VertexArray vao_;
  gl::Buffer vbo_;
  std::vector<glm::dvec3> points_;     // absolute world, double
  std::vector<float> upload_scratch_;  // interleaved ribbon: pos(3) dir(3) side(1) x 2 per point
  uint64_t revision_ = 0;
  uint64_t uploaded_revision_ = 0;
  glm::dvec3 uploaded_origin_{0.0};
  bool uploaded_valid_ = false;
};

}  // namespace pj::scene3d
