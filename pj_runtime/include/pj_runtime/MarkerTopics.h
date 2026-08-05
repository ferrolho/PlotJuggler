// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <string_view>

#include "pj_base/builtin/plot_markers.hpp"  // sdk::kMarkerObjectTopicPrefix

namespace PJ {

/// True if `object_topic` names a marker set (any marker topic, preview or not).
/// The single host-side predicate every consumer of ObjectStore topics uses to
/// treat marker snapshots as reserved internal state — excluded from dataset
/// time-bounds, the curve tree, playback focus, refill pruning, and the generic
/// dataset-merge fold (markers are merged set-aware by the marker service, not
/// folded as opaque object entries, because they publish at a sentinel timestamp
/// with single-entry retention). Centralizes what was an inline
/// starts_with(sdk::kMarkerObjectTopicPrefix) scattered across those consumers.
[[nodiscard]] inline bool isMarkerObjectTopic(std::string_view object_topic) {
  return object_topic.starts_with(sdk::kMarkerObjectTopicPrefix);
}

}  // namespace PJ
