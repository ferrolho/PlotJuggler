// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_core/model_budget.h"

#include <gtest/gtest.h>

#include <limits>

namespace pj::scene3d {

TEST(ModelBudgetTest, SourceAndMeshBoundariesAreInclusive) {
  EXPECT_TRUE(browserModelSourceFits(kBrowserMaxModelSourceBytes));
  EXPECT_FALSE(browserModelSourceFits(kBrowserMaxModelSourceBytes + 1U));
  EXPECT_TRUE(browserModelMeshCountsFit(
      kBrowserMaxModelVerticesPerMesh, kBrowserMaxModelIndicesPerMesh, kBrowserMaxModelSubmeshesPerMesh));
  EXPECT_FALSE(browserModelMeshCountsFit(kBrowserMaxModelVerticesPerMesh + 1U, 0U, 0U));
  EXPECT_FALSE(browserModelMeshCountsFit(0U, kBrowserMaxModelIndicesPerMesh + 1U, 0U));
  EXPECT_FALSE(browserModelMeshCountsFit(0U, 0U, kBrowserMaxModelSubmeshesPerMesh + 1U));
  EXPECT_TRUE(browserModelStructureFits(
      kBrowserMaxModelMaterialsPerMesh, kBrowserMaxModelNodesPerMesh, kBrowserMaxModelNodeDepth));
  EXPECT_FALSE(browserModelStructureFits(kBrowserMaxModelMaterialsPerMesh + 1U, 0U, 0U));
  EXPECT_FALSE(browserModelStructureFits(0U, kBrowserMaxModelNodesPerMesh + 1U, 0U));
  EXPECT_FALSE(browserModelStructureFits(0U, 0U, kBrowserMaxModelNodeDepth + 1U));
}

TEST(ModelBudgetTest, RetainedByteAccountingRejectsOverflow) {
  const auto normal = browserModelRetainedBytes(10U, 64U, 30U, 2U, 24U, 128U);
  ASSERT_TRUE(normal.has_value());
  // Includes the retained two-edge-index expansion for every triangle index.
  EXPECT_EQ(*normal, 1'176U);
  EXPECT_FALSE(browserModelRetainedBytes(std::numeric_limits<std::uint64_t>::max(), 2U, 0U, 0U, 0U, 0U).has_value());
  EXPECT_FALSE(browserModelRetainedBytes(0U, 0U, std::numeric_limits<std::uint64_t>::max(), 0U, 0U, 0U).has_value());
}

TEST(ModelBudgetTest, DecodedTexturePixelConsumptionIsAggregateAndTransactional) {
  std::uint64_t remaining = kBrowserMaxModelTexturePixelsPerMesh;
  EXPECT_TRUE(tryConsumeBrowserModelTexturePixels(6U * 1024U * 1024U, remaining));
  EXPECT_EQ(remaining, 10U * 1024U * 1024U);
  EXPECT_FALSE(tryConsumeBrowserModelTexturePixels(10U * 1024U * 1024U + 1U, remaining));
  EXPECT_EQ(remaining, 10U * 1024U * 1024U);
  EXPECT_TRUE(tryConsumeBrowserModelTexturePixels(remaining, remaining));
  EXPECT_EQ(remaining, 0U);
}

TEST(ModelBudgetTest, DecodedTextureConsumptionChecksMeshAndLayerTransactionally) {
  std::uint64_t mesh_remaining = 8U;
  std::uint64_t layer_remaining = 6U;
  EXPECT_FALSE(tryConsumeBrowserModelTexturePixels(7U, mesh_remaining, layer_remaining));
  EXPECT_EQ(mesh_remaining, 8U);
  EXPECT_EQ(layer_remaining, 6U);
  EXPECT_TRUE(tryConsumeBrowserModelTexturePixels(6U, mesh_remaining, layer_remaining));
  EXPECT_EQ(mesh_remaining, 2U);
  EXPECT_EQ(layer_remaining, 0U);
}

TEST(ModelBudgetTest, FiveSlotMeshPreflightRollsBackWhenLastMapExceedsLayerBudget) {
  std::uint64_t committed_layer_remaining = 10U;
  std::uint64_t candidate_layer_remaining = committed_layer_remaining;
  std::uint64_t mesh_remaining = 16U;
  for (int slot = 0; slot < 4; ++slot) {
    ASSERT_TRUE(tryConsumeBrowserModelTexturePixels(2U, mesh_remaining, candidate_layer_remaining));
  }
  EXPECT_EQ(candidate_layer_remaining, 2U);
  EXPECT_FALSE(tryConsumeBrowserModelTexturePixels(3U, mesh_remaining, candidate_layer_remaining));
  EXPECT_EQ(candidate_layer_remaining, 2U);
  EXPECT_EQ(mesh_remaining, 8U);
  EXPECT_EQ(committed_layer_remaining, 10U);
}

TEST(ModelBudgetTest, AggregateConsumptionIsTransactional) {
  std::uint64_t draws = 8U;
  std::uint64_t triangles = 20U;
  EXPECT_FALSE(tryConsumeBrowserModels(9U, 1U, draws, triangles));
  EXPECT_EQ(draws, 8U);
  EXPECT_EQ(triangles, 20U);
  EXPECT_TRUE(tryConsumeBrowserModels(3U, 7U, draws, triangles));
  EXPECT_EQ(draws, 5U);
  EXPECT_EQ(triangles, 13U);
}

}  // namespace pj::scene3d
