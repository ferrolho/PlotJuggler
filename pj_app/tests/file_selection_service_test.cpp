// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include "FileSelectionService.h"

namespace PJ {
namespace {

TEST(FileSelectionServiceTest, BrowserUsesAggregateFilterOnly) {
  const QString desktop =
      QStringLiteral("All supported files (*.csv *.mcap);;CSV (*.csv);;MCAP (*.mcap);;All files (*)");
  EXPECT_EQ(FileSelectionService::browserContentFilter(desktop), QStringLiteral("All supported files (*.csv *.mcap)"));
}

TEST(FileSelectionServiceTest, BrowserPreservesSingleOrEmptyFilter) {
  EXPECT_EQ(
      FileSelectionService::browserContentFilter(QStringLiteral("All files (*)")), QStringLiteral("All files (*)"));
  EXPECT_TRUE(FileSelectionService::browserContentFilter(QString{}).isEmpty());
}

}  // namespace
}  // namespace PJ
