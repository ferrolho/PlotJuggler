// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>

#include "pj_scene3d_core/browser_budget.h"

namespace pj::scene3d {

// Browser point-cloud limits live in the GL/Qt-free core so the exact boundary
// and aggregate subtraction are desktop-unit-testable. Native rendering does
// not consume these constants and retains its existing unconstrained behavior.
inline constexpr std::uint64_t kBrowserMaxPointsPerCloud = 1'000'000;
inline constexpr std::uint64_t kBrowserMaxWireBytesPerCloud = 64ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kBrowserMaxPointVerticesPerView = 1'000'000;

[[nodiscard]] constexpr bool browserPointCountFits(std::uint64_t declared_points) noexcept {
  return declared_points <= kBrowserMaxPointsPerCloud;
}

[[nodiscard]] constexpr bool browserPointPayloadFits(std::uint64_t wire_bytes) noexcept {
  return wire_bytes <= kBrowserMaxWireBytesPerCloud;
}

// DepthCloud contributes one candidate point per depth pixel and shares the
// point renderer's retained/view budgets. Widen before multiplication so
// hostile uint32 dimensions cannot wrap a smaller accepted count.
[[nodiscard]] constexpr bool browserDepthDimensionsFit(
    std::uint32_t width, std::uint32_t height, std::uint64_t& pixel_count) noexcept {
  pixel_count = static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
  return width != 0U && height != 0U && browserPointCountFits(pixel_count);
}

// Consume only on success. In particular, a rejected request leaves `remaining`
// unchanged and cannot wrap an unsigned counter below zero.
[[nodiscard]] constexpr bool tryConsumeBrowserPointVertices(
    std::uint64_t requested, std::uint64_t& remaining) noexcept {
  return tryConsumeBrowserBudget(requested, remaining);
}

}  // namespace pj::scene3d
