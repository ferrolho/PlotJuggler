// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>

#include "pj_scene3d_core/browser_budget.h"

namespace pj::scene3d {

// Browser-only voxel limits. A scalar layer retains one float per voxel on the
// CPU and one R32F texel on the GPU; RGBA uses the same four bytes on each side.
// Four million voxels therefore cost about 32 MiB across the retained pack and
// volume texture, excluding the zero-copy source payload. Native rendering does
// not consume these constants and keeps its existing behavior.
inline constexpr std::uint64_t kBrowserMaxVoxelsPerLayer = 4ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kBrowserMaxVoxelWireBytes = 32ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kBrowserMaxVoxelsPerView = 8ULL * 1024ULL * 1024ULL;

[[nodiscard]] constexpr bool browserVoxelDimensionsFit(
    std::uint64_t columns, std::uint64_t rows, std::uint64_t slices) noexcept {
  if (columns == 0U || rows == 0U || slices == 0U || columns > kBrowserMaxVoxelsPerLayer) {
    return false;
  }
  if (rows > kBrowserMaxVoxelsPerLayer / columns) {
    return false;
  }
  const std::uint64_t columns_rows = columns * rows;
  return slices <= kBrowserMaxVoxelsPerLayer / columns_rows;
}

[[nodiscard]] constexpr bool browserVoxelPayloadFits(std::uint64_t wire_bytes) noexcept {
  return wire_bytes <= kBrowserMaxVoxelWireBytes;
}

[[nodiscard]] constexpr bool tryConsumeBrowserVoxels(std::uint64_t requested, std::uint64_t& remaining) noexcept {
  return tryConsumeBrowserBudget(requested, remaining);
}

}  // namespace pj::scene3d
