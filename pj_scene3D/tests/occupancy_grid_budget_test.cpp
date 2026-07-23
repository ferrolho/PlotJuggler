// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_core/occupancy_grid_budget.h"

#include <gtest/gtest.h>

namespace pj::scene3d {
namespace {

TEST(OccupancyGridBrowserBudget, LayerLimitsAcceptExactBoundariesOnly) {
  EXPECT_TRUE(browserOccupancyDimensionsFit(8192U, 1024U));
  EXPECT_FALSE(browserOccupancyDimensionsFit(8192U, 1025U));
  EXPECT_FALSE(browserOccupancyDimensionsFit(8193U, 1U));
  EXPECT_FALSE(browserOccupancyDimensionsFit(0U, 1024U));
  EXPECT_TRUE(browserOccupancyPayloadFits(kBrowserMaxOccupancyWireBytes));
  EXPECT_FALSE(browserOccupancyPayloadFits(kBrowserMaxOccupancyWireBytes + 1U));
  EXPECT_TRUE(browserOccupancyUpdateWindowFits(kBrowserMaxOccupancyUpdateWindowBytes));
  EXPECT_FALSE(browserOccupancyUpdateWindowFits(kBrowserMaxOccupancyUpdateWindowBytes + 1U));
}

TEST(OccupancyGridBrowserBudget, ViewBudgetAggregatesWithoutUnsignedUnderflow) {
  std::uint64_t remaining = kBrowserMaxOccupancyCellsPerView;
  EXPECT_TRUE(tryConsumeBrowserOccupancyCells(kBrowserMaxOccupancyGridCells, remaining));
  EXPECT_EQ(remaining, kBrowserMaxOccupancyGridCells);
  EXPECT_TRUE(tryConsumeBrowserOccupancyCells(kBrowserMaxOccupancyGridCells, remaining));
  EXPECT_EQ(remaining, 0U);
  EXPECT_FALSE(tryConsumeBrowserOccupancyCells(1U, remaining));
  EXPECT_EQ(remaining, 0U);
}

}  // namespace
}  // namespace pj::scene3d
