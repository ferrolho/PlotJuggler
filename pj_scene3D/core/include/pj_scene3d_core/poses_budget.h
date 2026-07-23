// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>

namespace pj::scene3d {

// Browser-only retained-memory limits. A full pose triad expands to three
// 80-byte PoseTriadInstances, so 100k poses / 300k arms is about 24 MiB in the
// CPU staging vector and another 24 MiB in the QRhi instance buffer. Native
// rendering does not consume these constants and remains unconstrained.
inline constexpr std::uint64_t kBrowserMaxPosesPerLayer = 100'000;
inline constexpr std::uint64_t kBrowserMaxPoseWireBytesPerLayer = 32ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kBrowserMaxPoseArmsPerView = 300'000;

[[nodiscard]] constexpr bool browserPoseCountFits(std::uint64_t poses) noexcept {
  return poses <= kBrowserMaxPosesPerLayer;
}

[[nodiscard]] constexpr bool browserPosePayloadFits(std::uint64_t wire_bytes) noexcept {
  return wire_bytes <= kBrowserMaxPoseWireBytesPerLayer;
}

[[nodiscard]] constexpr bool tryConsumeBrowserPoseArms(std::uint64_t requested, std::uint64_t& remaining) noexcept {
  if (requested > remaining) {
    return false;
  }
  remaining -= requested;
  return true;
}

}  // namespace pj::scene3d
