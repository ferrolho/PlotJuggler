// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>

namespace pj::scene3d {

// The one per-view budget primitive behind every family's
// tryConsumeBrowserX wrapper: subtract `requested` from `remaining` when it
// fits, refuse (leaving `remaining` untouched) when it does not. Never refund
// within a frame — callers check placement before consuming.
[[nodiscard]] constexpr bool tryConsumeBrowserBudget(std::uint64_t requested, std::uint64_t& remaining) noexcept {
  if (requested > remaining) {
    return false;
  }
  remaining -= requested;
  return true;
}

}  // namespace pj::scene3d
