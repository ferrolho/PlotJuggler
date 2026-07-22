// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QLineF>
#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include "pj_plotting/PlotSampleReducer.h"

namespace PJ {
namespace {

PlotSampleReduction reduce(const std::vector<QPointF>& points, std::size_t budget, std::size_t buckets) {
  return reducePlotSamples(points.size(), budget, buckets, [&points](std::size_t index) { return points.at(index); });
}

std::vector<std::size_t> indices(const PlotSampleReduction& reduction) {
  std::vector<std::size_t> result;
  result.reserve(reduction.samples.size());
  for (const ReducedPlotSample& sample : reduction.samples) {
    result.push_back(sample.source_index);
  }
  return result;
}

void annotateEuclidean(PlotSampleReduction& reduction, const std::vector<QPointF>& points) {
  annotatePlotSamplePathOffsets(
      reduction, [&points](std::size_t index) { return points.at(index); },
      [](const QPointF& from, const QPointF& to) { return QLineF(from, to).length(); });
}

TEST(PlotSampleReducer, KeepsFiniteRunsDisconnectedWithoutReduction) {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const std::vector<QPointF> points{{0.0, 1.0}, {1.0, 2.0}, {nan, nan}, {3.0, 4.0}, {4.0, 5.0}};

  const PlotSampleReduction reduction = reduce(points, 10, 10);

  EXPECT_FALSE(reduction.reduced);
  EXPECT_EQ(indices(reduction), (std::vector<std::size_t>{0, 1, 3, 4}));
  ASSERT_EQ(reduction.samples.size(), 4U);
  EXPECT_TRUE(reduction.samples[0].starts_new_run);
  EXPECT_FALSE(reduction.samples[1].starts_new_run);
  EXPECT_TRUE(reduction.samples[2].starts_new_run);
  EXPECT_FALSE(reduction.samples[3].starts_new_run);
  EXPECT_EQ(reduction.input_runs, 2U);
  EXPECT_EQ(reduction.retained_runs, 2U);
}

TEST(PlotSampleReducer, PreservesIsolatedScreenSpaceSpikeAndTrough) {
  std::vector<QPointF> points;
  points.reserve(1026);
  for (std::size_t index = 0; index < 1026; ++index) {
    points.emplace_back(static_cast<qreal>(index), 0.0);
  }
  points[257].setY(1000.0);
  points[768].setY(-1000.0);

  const PlotSampleReduction reduction = reduce(points, 66, 16);
  const std::vector<std::size_t> retained = indices(reduction);

  EXPECT_TRUE(reduction.reduced);
  EXPECT_LE(reduction.samples.size(), 66U);
  EXPECT_NE(std::find(retained.begin(), retained.end(), 257U), retained.end());
  EXPECT_NE(std::find(retained.begin(), retained.end(), 768U), retained.end());
  EXPECT_EQ(retained.front(), 0U);
  EXPECT_EQ(retained.back(), 1025U);
}

TEST(PlotSampleReducer, PreservesDistinctXAndYExtremaInSourceOrder) {
  const std::vector<QPointF> points{{0.0, 0.0}, {9.0, 1.0}, {-8.0, 2.0}, {3.0, -7.0}, {4.0, 11.0}, {5.0, 5.0}};

  const PlotSampleReduction reduction = reduce(points, 6, 1);

  EXPECT_EQ(indices(reduction), (std::vector<std::size_t>{0, 1, 2, 3, 4, 5}));
}

TEST(PlotSampleReducer, DropsOnlyWholeRunsWhenEndpointsExceedBudget) {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const std::vector<QPointF> points{{0.0, 0.0}, {1.0, 1.0}, {nan, nan}, {2.0, 2.0},
                                    {3.0, 3.0}, {nan, nan}, {4.0, 4.0}, {5.0, 5.0}};

  const PlotSampleReduction reduction = reduce(points, 3, 10);

  EXPECT_EQ(indices(reduction), (std::vector<std::size_t>{0, 1}));
  EXPECT_EQ(reduction.retained_runs, 1U);
  EXPECT_EQ(reduction.dropped_runs, 2U);
  ASSERT_EQ(reduction.samples.size(), 2U);
  EXPECT_TRUE(reduction.samples.front().starts_new_run);
  EXPECT_FALSE(reduction.samples.back().starts_new_run);
}

TEST(PlotSampleReducer, StopsAtFirstRunThatCannotFit) {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const std::vector<QPointF> points{{0.0, 0.0}, {nan, nan}, {1.0, 1.0}, {2.0, 2.0}, {nan, nan}, {3.0, 3.0}};

  const PlotSampleReduction reduction = reduce(points, 2, 10);

  EXPECT_EQ(indices(reduction), (std::vector<std::size_t>{0}));
  EXPECT_EQ(reduction.input_runs, 3U);
  EXPECT_EQ(reduction.retained_runs, 1U);
  EXPECT_EQ(reduction.dropped_runs, 2U);
}

TEST(PlotSampleReducer, ZeroBudgetReportsRunsWithoutEmittingPartialData) {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const std::vector<QPointF> points{{0.0, 0.0}, {1.0, 1.0}, {nan, nan}, {2.0, 2.0}};

  const PlotSampleReduction reduction = reduce(points, 0, 10);

  EXPECT_TRUE(reduction.reduced);
  EXPECT_TRUE(reduction.samples.empty());
  EXPECT_EQ(reduction.input_runs, 2U);
  EXPECT_EQ(reduction.dropped_runs, 2U);
}

TEST(PlotSampleReducer, UsesWideArithmeticForMultiMillionSampleBuckets) {
  constexpr std::size_t kSampleCount = 6'000'003;
  constexpr std::size_t kSpikeIndex = 3'100'001;
  const PlotSampleReduction reduction = reducePlotSamples(kSampleCount, 3'200, 799, [](std::size_t index) {
    return QPointF(static_cast<qreal>(index), index == kSpikeIndex ? 1000.0 : 0.0);
  });
  const std::vector<std::size_t> retained = indices(reduction);

  EXPECT_TRUE(reduction.reduced);
  EXPECT_LE(reduction.samples.size(), 3'200U);
  EXPECT_GT(reduction.samples.size(), 1'000U);
  EXPECT_NE(std::find(retained.begin(), retained.end(), kSpikeIndex), retained.end());
  EXPECT_EQ(retained.front(), 0U);
  EXPECT_EQ(retained.back(), kSampleCount - 1);
}

TEST(PlotSampleReducer, GapHeavyInputKeepsAuxiliaryRunStateBudgetBounded) {
  constexpr std::size_t kSampleCount = 1'000'000;
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const PlotSampleReduction reduction = reducePlotSamples(kSampleCount, 8, 100, [nan](std::size_t index) {
    return index % 2 == 0 ? QPointF(static_cast<qreal>(index), 1.0) : QPointF(nan, nan);
  });

  EXPECT_EQ(reduction.samples.size(), 8U);
  EXPECT_EQ(reduction.input_runs, kSampleCount / 2);
  EXPECT_EQ(reduction.retained_runs, 8U);
  EXPECT_EQ(reduction.dropped_runs, kSampleCount / 2 - 8);
  EXPECT_TRUE(std::all_of(reduction.samples.begin(), reduction.samples.end(), [](const ReducedPlotSample& sample) {
    return sample.starts_new_run;
  }));
}

TEST(PlotSampleReducer, IsDeterministicAndNeverExceedsBudget) {
  std::vector<QPointF> points;
  points.reserve(1000);
  for (std::size_t index = 0; index < 1000; ++index) {
    points.emplace_back(static_cast<qreal>((index * 37) % 101), static_cast<qreal>((index * 53) % 211));
  }

  const PlotSampleReduction first = reduce(points, 127, 50);
  const PlotSampleReduction second = reduce(points, 127, 50);
  const std::vector<std::size_t> first_indices = indices(first);

  EXPECT_EQ(first_indices, indices(second));
  EXPECT_LE(first.samples.size(), 127U);
  EXPECT_TRUE(std::is_sorted(first_indices.begin(), first_indices.end()));
}

TEST(PlotSampleReducer, PathOffsetsFollowOriginalZigzagInsteadOfReducedChords) {
  const std::vector<QPointF> points{{0.0, 0.0},   {1.0, 10.0}, {2.0, -10.0}, {3.0, 10.0},
                                    {4.0, -10.0}, {5.0, 10.0}, {6.0, 0.0}};
  PlotSampleReduction reduction = reduce(points, 4, 1);
  annotateEuclidean(reduction, points);

  std::vector<qreal> expected(points.size(), 0.0);
  for (std::size_t index = 1; index < points.size(); ++index) {
    expected[index] = expected[index - 1] + QLineF(points[index - 1], points[index]).length();
  }
  ASSERT_LT(reduction.samples.size(), points.size());
  for (const ReducedPlotSample& sample : reduction.samples) {
    EXPECT_DOUBLE_EQ(sample.source_path_offset, expected[sample.source_index]);
  }
}

TEST(PlotSampleReducer, CommonSourceAnchorsSurviveBudgetChangesAndAppend) {
  std::vector<QPointF> points;
  for (std::size_t index = 0; index < 65; ++index) {
    points.emplace_back(static_cast<qreal>(index), index % 2 == 0 ? -20.0 : 20.0);
  }
  PlotSampleReduction coarse = reduce(points, 10, 2);
  PlotSampleReduction fine = reduce(points, 18, 4);
  annotateEuclidean(coarse, points);
  annotateEuclidean(fine, points);

  const auto offset_for = [](const PlotSampleReduction& reduction, std::size_t source_index) {
    const auto found = std::find_if(
        reduction.samples.cbegin(), reduction.samples.cend(),
        [source_index](const auto& s) { return s.source_index == source_index; });
    return found == reduction.samples.cend() ? -1.0 : found->source_path_offset;
  };
  for (const ReducedPlotSample& sample : coarse.samples) {
    const qreal fine_offset = offset_for(fine, sample.source_index);
    if (fine_offset >= 0.0) {
      EXPECT_DOUBLE_EQ(sample.source_path_offset, fine_offset);
    }
  }

  points.emplace_back(65.0, 20.0);
  PlotSampleReduction appended = reduce(points, 10, 2);
  annotateEuclidean(appended, points);
  bool compared_nonzero_prefix = false;
  for (const ReducedPlotSample& sample : coarse.samples) {
    if (sample.source_index == 0) {
      continue;
    }
    const qreal appended_offset = offset_for(appended, sample.source_index);
    if (appended_offset >= 0.0) {
      EXPECT_DOUBLE_EQ(appended_offset, sample.source_path_offset);
      compared_nonzero_prefix = true;
    }
  }
  EXPECT_TRUE(compared_nonzero_prefix);
}

TEST(PlotSampleReducer, PathOffsetsResetAtGapsAndSupportStepMetric) {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const std::vector<QPointF> points{{0.0, 0.0}, {3.0, 4.0}, {nan, nan}, {10.0, 10.0}, {13.0, 14.0}};
  PlotSampleReduction reduction = reduce(points, 10, 10);
  annotatePlotSamplePathOffsets(
      reduction, [&points](std::size_t index) { return points.at(index); },
      [](const QPointF& from, const QPointF& to) { return std::abs(to.x() - from.x()) + std::abs(to.y() - from.y()); });

  ASSERT_EQ(reduction.samples.size(), 4U);
  EXPECT_DOUBLE_EQ(reduction.samples[0].source_path_offset, 0.0);
  EXPECT_DOUBLE_EQ(reduction.samples[1].source_path_offset, 7.0);
  EXPECT_DOUBLE_EQ(reduction.samples[2].source_path_offset, 0.0);
  EXPECT_DOUBLE_EQ(reduction.samples[3].source_path_offset, 7.0);
}

}  // namespace
}  // namespace PJ
