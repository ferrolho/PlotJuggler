// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_core/voxel_grid_budget.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

namespace pj::scene3d {
namespace {

TEST(VoxelGridBudget, DimensionsAcceptExactLayerLimit) {
  EXPECT_TRUE(browserVoxelDimensionsFit(256, 128, 128));
  EXPECT_FALSE(browserVoxelDimensionsFit(257, 128, 128));
}

TEST(VoxelGridBudget, DimensionsRejectDegenerateAndOverflowingProducts) {
  EXPECT_FALSE(browserVoxelDimensionsFit(0, 1, 1));
  EXPECT_FALSE(browserVoxelDimensionsFit(1, 0, 1));
  EXPECT_FALSE(browserVoxelDimensionsFit(1, 1, 0));
  EXPECT_FALSE(browserVoxelDimensionsFit(
      std::numeric_limits<std::uint64_t>::max(), std::numeric_limits<std::uint64_t>::max(), 2));
}

TEST(VoxelGridBudget, PayloadBoundaryIsInclusive) {
  EXPECT_TRUE(browserVoxelPayloadFits(kBrowserMaxVoxelWireBytes));
  EXPECT_FALSE(browserVoxelPayloadFits(kBrowserMaxVoxelWireBytes + 1U));
}

TEST(VoxelGridBudget, AggregateConsumptionCannotUnderflow) {
  std::uint64_t remaining = 10;
  EXPECT_FALSE(tryConsumeBrowserVoxels(11, remaining));
  EXPECT_EQ(remaining, 10U);
  EXPECT_TRUE(tryConsumeBrowserVoxels(10, remaining));
  EXPECT_EQ(remaining, 0U);
}

}  // namespace
}  // namespace pj::scene3d
