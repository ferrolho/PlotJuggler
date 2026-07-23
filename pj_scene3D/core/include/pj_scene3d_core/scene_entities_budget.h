// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <limits>
#include <optional>

#include "pj_scene3d_core/browser_budget.h"

namespace pj::scene3d {

// Browser-only SceneEntities limits. Procedural solids stay instanced, while
// line and triangle primitives retain double-precision vertices until the view
// has subtracted its render origin. The two independent view budgets prevent a
// marker-heavy topic from allocating unbounded CPU staging and QRhi buffers.
// Native rendering does not consume these constants.
inline constexpr std::uint64_t kBrowserMaxMarkerWireBytes = 32ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kBrowserMaxMarkerInstancesPerLayer = 50'000;
inline constexpr std::uint64_t kBrowserMaxMarkerInstancesPerView = 100'000;
inline constexpr std::uint64_t kBrowserMaxMarkerStreamVerticesPerLayer = 500'000;
inline constexpr std::uint64_t kBrowserMaxMarkerStreamVerticesPerView = 1'000'000;
inline constexpr std::uint64_t kBrowserMaxMarkerFramesPerLayer = 4'096;
inline constexpr std::uint64_t kBrowserMaxMarkerRetainedBytesPerLayer = 64ULL * 1024ULL * 1024ULL;

[[nodiscard]] constexpr bool browserMarkerWirePayloadFits(std::uint64_t bytes) noexcept {
  return bytes <= kBrowserMaxMarkerWireBytes;
}

[[nodiscard]] constexpr bool browserMarkerLayerCountsFit(
    std::uint64_t instances, std::uint64_t stream_vertices, std::uint64_t frames) noexcept {
  return instances <= kBrowserMaxMarkerInstancesPerLayer &&
         stream_vertices <= kBrowserMaxMarkerStreamVerticesPerLayer && frames <= kBrowserMaxMarkerFramesPerLayer;
}

[[nodiscard]] constexpr bool browserMarkerRetainedBytesFit(std::uint64_t bytes) noexcept {
  return bytes <= kBrowserMaxMarkerRetainedBytesPerLayer;
}

// Solid cubes submit one fill instance. Normal (non-wireframe) rendering also
// submits one instance for the twelve-edge overlay, whereas explicit wireframe
// uses only the original cube instance. Keep this renderer expansion inside the
// same per-layer and per-view budgets that bound the staging buffer.
[[nodiscard]] constexpr std::optional<std::uint64_t> browserMarkerSubmittedInstanceCount(
    std::uint64_t logical_instances, std::uint64_t cube_instances, bool wireframe) noexcept {
  if (wireframe) {
    return logical_instances;
  }
  if (cube_instances > std::numeric_limits<std::uint64_t>::max() - logical_instances) {
    return std::nullopt;
  }
  return logical_instances + cube_instances;
}

[[nodiscard]] constexpr bool tryConsumeBrowserMarkerInstances(
    std::uint64_t requested, std::uint64_t& remaining) noexcept {
  return tryConsumeBrowserBudget(requested, remaining);
}

[[nodiscard]] constexpr bool tryConsumeBrowserMarkerStreamVertices(
    std::uint64_t requested, std::uint64_t& remaining) noexcept {
  return tryConsumeBrowserBudget(requested, remaining);
}

}  // namespace pj::scene3d
