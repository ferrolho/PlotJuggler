// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_widgets/passes/shadow_map_pass.h"

#include "pj_scene3d_core/shadow_camera.h"  // kShadowMapSize
#include "pj_scene3d_widgets/gl/gl_functions.h"
#include "pj_scene3d_widgets/scene_look_defaults.h"

namespace pj::scene3d {
namespace {

// Caster-side acne suppression: values shared with the browser pipeline via
// scene_look_defaults.h. Conservative defaults finalized against screenshots;
// tuning knobs deliberately stay out of the UI.
constexpr float kPolygonOffsetFactor = look::kShadowSlopeScaledDepthBias;
constexpr float kPolygonOffsetUnits = look::kShadowDepthBiasUnits;

}  // namespace

void ShadowMapPass::ensure() {
  if (ready_ && depth_.id() != 0U) {
    return;
  }
  fbo_.bind();
  // Single-sample 32-bit float depth, NEAREST (averaging depth corrupts the compare).
  depth_.allocate(GL_DEPTH_COMPONENT32F, GL_DEPTH_COMPONENT, GL_FLOAT, kShadowMapSize, kShadowMapSize);
  withGlFunctions([this](auto& functions) {
    functions.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depth_.id(), 0);
    // Depth-only: no color attachment. glDrawBuffers(GL_NONE) (the plural form is in
    // both QOpenGLFunctions_4_5_Core and QOpenGLExtraFunctions, unlike the singular
    // desktop-only glDrawBuffer) keeps the FBO complete with no color target. The
    // read buffer is irrelevant to completeness for a render-only target.
    const GLenum none_buffer = GL_NONE;
    functions.glDrawBuffers(1, &none_buffer);
  });
  ready_ = fbo_.checkComplete();
}

void ShadowMapPass::begin() {
  if (!ready_) {
    return;
  }
  fbo_.bind();
  withGlFunctions([](auto& functions) {
    functions.glViewport(0, 0, kShadowMapSize, kShadowMapSize);
    functions.glEnable(GL_DEPTH_TEST);
    functions.glDepthMask(GL_TRUE);
    functions.glClear(GL_DEPTH_BUFFER_BIT);
    functions.glEnable(GL_POLYGON_OFFSET_FILL);
    functions.glPolygonOffset(kPolygonOffsetFactor, kPolygonOffsetUnits);
  });
}

void ShadowMapPass::end() {
  withGlFunctions([](auto& functions) { functions.glDisable(GL_POLYGON_OFFSET_FILL); });
}

GLuint ShadowMapPass::depthTextureId() const noexcept {
  return depth_.id();
}

void ShadowMapPass::releaseGL() {
  fbo_ = gl::Framebuffer{};
  depth_ = gl::Texture{};
  ready_ = false;
}

}  // namespace pj::scene3d
