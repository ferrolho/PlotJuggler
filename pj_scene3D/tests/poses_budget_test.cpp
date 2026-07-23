// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_core/poses_budget.h"

#include <gtest/gtest.h>

namespace pj::scene3d {
namespace {

TEST(PosesBrowserBudget, LayerLimitsAcceptTheirExactBoundaryOnly) {
  EXPECT_TRUE(browserPoseCountFits(kBrowserMaxPosesPerLayer));
  EXPECT_FALSE(browserPoseCountFits(kBrowserMaxPosesPerLayer + 1U));
  EXPECT_TRUE(browserPosePayloadFits(kBrowserMaxPoseWireBytesPerLayer));
  EXPECT_FALSE(browserPosePayloadFits(kBrowserMaxPoseWireBytesPerLayer + 1U));
}

TEST(PosesBrowserBudget, ViewArmBudgetAggregatesWithoutUnsignedUnderflow) {
  std::uint64_t remaining = kBrowserMaxPoseArmsPerView;
  EXPECT_TRUE(tryConsumeBrowserPoseArms(90'000U, remaining));
  EXPECT_EQ(remaining, 210'000U);
  EXPECT_TRUE(tryConsumeBrowserPoseArms(210'000U, remaining));
  EXPECT_EQ(remaining, 0U);
  EXPECT_FALSE(tryConsumeBrowserPoseArms(1U, remaining));
  EXPECT_EQ(remaining, 0U);
}

}  // namespace
}  // namespace pj::scene3d
