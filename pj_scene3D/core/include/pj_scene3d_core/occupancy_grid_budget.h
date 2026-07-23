// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>

#include "pj_scene3d_core/browser_budget.h"

namespace pj::scene3d {

// Browser-only occupancy-grid limits. At the per-layer boundary the
// reconstructor retains the live grid, a pristine base, and at most 16 MiB of
// snapshots; QRhi retains one R8 texel per cell. Native rendering does not use
// these constants and keeps its existing limits and behavior.
inline constexpr std::uint64_t kBrowserMaxOccupancyGridCells = 8ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kBrowserMaxOccupancyWireBytes = 32ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kBrowserMaxOccupancyUpdateWindowBytes = 32ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kBrowserOccupancySnapshotBudgetBytes = 16ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kBrowserMaxOccupancyTextureDimension = 8192ULL;
inline constexpr std::uint64_t kBrowserMaxOccupancyCellsPerView = 16ULL * 1024ULL * 1024ULL;

[[nodiscard]] constexpr bool browserOccupancyDimensionsFit(std::uint64_t width, std::uint64_t height) noexcept {
  return width > 0U && height > 0U && width <= kBrowserMaxOccupancyTextureDimension &&
         height <= kBrowserMaxOccupancyTextureDimension && width * height <= kBrowserMaxOccupancyGridCells;
}

[[nodiscard]] constexpr bool browserOccupancyPayloadFits(std::uint64_t wire_bytes) noexcept {
  return wire_bytes <= kBrowserMaxOccupancyWireBytes;
}

[[nodiscard]] constexpr bool browserOccupancyUpdateWindowFits(std::uint64_t decoded_bytes) noexcept {
  return decoded_bytes <= kBrowserMaxOccupancyUpdateWindowBytes;
}

[[nodiscard]] constexpr bool tryConsumeBrowserOccupancyCells(
    std::uint64_t requested, std::uint64_t& remaining) noexcept {
  return tryConsumeBrowserBudget(requested, remaining);
}

}  // namespace pj::scene3d
