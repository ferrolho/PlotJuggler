// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <limits>
#include <optional>

namespace pj::scene3d {

// Browser-only mesh/model limits. Native Scene3D keeps its existing Assimp and
// OpenGL behavior; these constants bound decoded model memory and QRhi work in
// the Emscripten implementation.
inline constexpr std::uint64_t kBrowserMaxModelSourceBytes = 32ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kBrowserMaxModelVerticesPerMesh = 1'000'000;
inline constexpr std::uint64_t kBrowserMaxModelIndicesPerMesh = 3'000'000;
inline constexpr std::uint64_t kBrowserMaxModelSubmeshesPerMesh = 4'096;
inline constexpr std::uint64_t kBrowserMaxModelMaterialsPerMesh = 4'096;
inline constexpr std::uint64_t kBrowserMaxModelNodesPerMesh = 16'384;
inline constexpr std::uint64_t kBrowserMaxModelNodeDepth = 256;
inline constexpr std::uint64_t kBrowserMaxModelEncodedTextureBytesPerMesh = 32ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kBrowserMaxModelTexturePixelsPerMesh = 16ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kBrowserMaxModelTexturePixelsPerLayer = 32ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kBrowserMaxModelStateBytesPerLayer = 64ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kBrowserMaxModelRetainedBytesPerMesh = 128ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kBrowserMaxModelRetainedBytesPerLayer = 256ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kBrowserMaxModelDrawsPerLayer = 4'096;
inline constexpr std::uint64_t kBrowserMaxModelDrawsPerView = 8'192;
inline constexpr std::uint64_t kBrowserMaxModelTrianglesPerLayer = 2'000'000;
inline constexpr std::uint64_t kBrowserMaxModelTrianglesPerView = 4'000'000;
inline constexpr std::uint64_t kBrowserMaxModelSnapshotCacheBytes = 64ULL * 1024ULL * 1024ULL;

[[nodiscard]] constexpr bool browserModelSourceFits(std::uint64_t bytes) noexcept {
  return bytes <= kBrowserMaxModelSourceBytes;
}

[[nodiscard]] constexpr bool browserModelMeshCountsFit(
    std::uint64_t vertices, std::uint64_t indices, std::uint64_t submeshes) noexcept {
  return vertices <= kBrowserMaxModelVerticesPerMesh && indices <= kBrowserMaxModelIndicesPerMesh &&
         submeshes <= kBrowserMaxModelSubmeshesPerMesh;
}

[[nodiscard]] constexpr bool browserModelStructureFits(
    std::uint64_t materials, std::uint64_t nodes, std::uint64_t node_depth) noexcept {
  return materials <= kBrowserMaxModelMaterialsPerMesh && nodes <= kBrowserMaxModelNodesPerMesh &&
         node_depth <= kBrowserMaxModelNodeDepth;
}

[[nodiscard]] constexpr std::optional<std::uint64_t> browserModelRetainedBytes(
    std::uint64_t vertices, std::uint64_t vertex_stride, std::uint64_t indices, std::uint64_t submeshes,
    std::uint64_t submesh_stride, std::uint64_t encoded_texture_bytes) noexcept {
  std::uint64_t total = encoded_texture_bytes;
  const auto add_product = [&total](std::uint64_t count, std::uint64_t stride) constexpr {
    if (count != 0U && stride > std::numeric_limits<std::uint64_t>::max() / count) {
      return false;
    }
    const std::uint64_t bytes = count * stride;
    if (bytes > std::numeric_limits<std::uint64_t>::max() - total) {
      return false;
    }
    total += bytes;
    return true;
  };
  if (!add_product(vertices, vertex_stride) || !add_product(indices, sizeof(std::uint32_t)) ||
      !add_product(submeshes, submesh_stride) ||
      // The QRhi path retains a six-index edge expansion for every input
      // triangle so wireframe can be toggled without re-importing the mesh.
      !add_product(indices, 2U * sizeof(std::uint32_t))) {
    return std::nullopt;
  }
  return total;
}

[[nodiscard]] constexpr bool tryConsumeBrowserModelTexturePixels(
    std::uint64_t pixels, std::uint64_t& remaining_pixels) noexcept {
  if (pixels > remaining_pixels) {
    return false;
  }
  remaining_pixels -= pixels;
  return true;
}

[[nodiscard]] constexpr bool tryConsumeBrowserModelTexturePixels(
    std::uint64_t pixels, std::uint64_t& remaining_mesh_pixels, std::uint64_t& remaining_layer_pixels) noexcept {
  if (pixels > remaining_mesh_pixels || pixels > remaining_layer_pixels) {
    return false;
  }
  remaining_mesh_pixels -= pixels;
  remaining_layer_pixels -= pixels;
  return true;
}

[[nodiscard]] constexpr bool tryConsumeBrowserModels(
    std::uint64_t draws, std::uint64_t triangles, std::uint64_t& remaining_draws,
    std::uint64_t& remaining_triangles) noexcept {
  if (draws > remaining_draws || triangles > remaining_triangles) {
    return false;
  }
  remaining_draws -= draws;
  remaining_triangles -= triangles;
  return true;
}

}  // namespace pj::scene3d
