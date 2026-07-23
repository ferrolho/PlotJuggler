// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_core/scene_entities_budget.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

namespace pj::scene3d {
namespace {

TEST(SceneEntitiesBudget, LayerBoundariesAreInclusive) {
  EXPECT_TRUE(browserMarkerWirePayloadFits(kBrowserMaxMarkerWireBytes));
  EXPECT_FALSE(browserMarkerWirePayloadFits(kBrowserMaxMarkerWireBytes + 1U));
  EXPECT_TRUE(browserMarkerLayerCountsFit(
      kBrowserMaxMarkerInstancesPerLayer, kBrowserMaxMarkerStreamVerticesPerLayer, kBrowserMaxMarkerFramesPerLayer));
  EXPECT_FALSE(browserMarkerLayerCountsFit(kBrowserMaxMarkerInstancesPerLayer + 1U, 0, 0));
  EXPECT_FALSE(browserMarkerLayerCountsFit(0, kBrowserMaxMarkerStreamVerticesPerLayer + 1U, 0));
  EXPECT_FALSE(browserMarkerLayerCountsFit(0, 0, kBrowserMaxMarkerFramesPerLayer + 1U));
  EXPECT_TRUE(browserMarkerRetainedBytesFit(kBrowserMaxMarkerRetainedBytesPerLayer));
  EXPECT_FALSE(browserMarkerRetainedBytesFit(kBrowserMaxMarkerRetainedBytesPerLayer + 1U));
}

TEST(SceneEntitiesBudget, AggregateConsumptionCannotUnderflow) {
  std::uint64_t instances = 7;
  EXPECT_FALSE(tryConsumeBrowserMarkerInstances(8, instances));
  EXPECT_EQ(instances, 7U);
  EXPECT_TRUE(tryConsumeBrowserMarkerInstances(7, instances));
  EXPECT_EQ(instances, 0U);

  std::uint64_t vertices = 11;
  EXPECT_FALSE(tryConsumeBrowserMarkerStreamVertices(12, vertices));
  EXPECT_EQ(vertices, 11U);
  EXPECT_TRUE(tryConsumeBrowserMarkerStreamVertices(11, vertices));
  EXPECT_EQ(vertices, 0U);
}

TEST(SceneEntitiesBudget, SubmittedInstancesIncludeNormalCubeEdgeOverlay) {
  const auto at_normal_limit = browserMarkerSubmittedInstanceCount(25'000, 25'000, false);
  ASSERT_TRUE(at_normal_limit.has_value());
  EXPECT_EQ(*at_normal_limit, kBrowserMaxMarkerInstancesPerLayer);
  EXPECT_TRUE(browserMarkerLayerCountsFit(*at_normal_limit, 0, 1));

  const auto above_normal_limit = browserMarkerSubmittedInstanceCount(25'001, 25'001, false);
  ASSERT_TRUE(above_normal_limit.has_value());
  EXPECT_FALSE(browserMarkerLayerCountsFit(*above_normal_limit, 0, 1));

  const auto wireframe =
      browserMarkerSubmittedInstanceCount(kBrowserMaxMarkerInstancesPerLayer, kBrowserMaxMarkerInstancesPerLayer, true);
  ASSERT_TRUE(wireframe.has_value());
  EXPECT_EQ(*wireframe, kBrowserMaxMarkerInstancesPerLayer);
  EXPECT_TRUE(browserMarkerLayerCountsFit(*wireframe, 0, 1));

  EXPECT_FALSE(browserMarkerSubmittedInstanceCount(std::numeric_limits<std::uint64_t>::max(), 1, false).has_value());
}

}  // namespace
}  // namespace pj::scene3d
