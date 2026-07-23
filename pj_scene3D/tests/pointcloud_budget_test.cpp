// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_core/pointcloud_budget.h"

#include <gtest/gtest.h>

#include <limits>

namespace pj::scene3d {
namespace {

TEST(PointCloudBrowserBudget, InputLimitsAcceptTheirExactBoundaryOnly) {
  EXPECT_TRUE(browserPointCountFits(kBrowserMaxPointsPerCloud));
  EXPECT_FALSE(browserPointCountFits(kBrowserMaxPointsPerCloud + 1U));
  EXPECT_TRUE(browserPointPayloadFits(kBrowserMaxWireBytesPerCloud));
  EXPECT_FALSE(browserPointPayloadFits(kBrowserMaxWireBytesPerCloud + 1U));
}

TEST(PointCloudBrowserBudget, ViewBudgetAggregatesWithoutUnsignedUnderflow) {
  std::uint64_t remaining = kBrowserMaxPointVerticesPerView;
  EXPECT_TRUE(tryConsumeBrowserPointVertices(400'000U, remaining));
  EXPECT_EQ(remaining, 600'000U);
  EXPECT_TRUE(tryConsumeBrowserPointVertices(600'000U, remaining));
  EXPECT_EQ(remaining, 0U);
  EXPECT_FALSE(tryConsumeBrowserPointVertices(1U, remaining));
  EXPECT_EQ(remaining, 0U);
}

TEST(PointCloudBrowserBudget, DepthDimensionsWidenBeforeApplyingSharedPointLimit) {
  std::uint64_t pixels = 0;
  EXPECT_TRUE(browserDepthDimensionsFit(1000U, 1000U, pixels));
  EXPECT_EQ(pixels, kBrowserMaxPointsPerCloud);
  EXPECT_FALSE(browserDepthDimensionsFit(1001U, 1000U, pixels));
  EXPECT_EQ(pixels, 1'001'000U);
  EXPECT_FALSE(browserDepthDimensionsFit(0U, 1000U, pixels));
  EXPECT_EQ(pixels, 0U);
  EXPECT_FALSE(browserDepthDimensionsFit(
      std::numeric_limits<std::uint32_t>::max(), std::numeric_limits<std::uint32_t>::max(), pixels));
  EXPECT_GT(pixels, std::numeric_limits<std::uint32_t>::max());
}

}  // namespace
}  // namespace pj::scene3d
