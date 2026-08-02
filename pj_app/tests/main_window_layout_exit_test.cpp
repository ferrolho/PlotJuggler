// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// GUI suite for LayoutExitWatch, the --exit-after-layout exit-code decision
// (stage-5 E5): exactly one decision per watch, mapped from the settlement
// boundary — load failure -> kExitCodeLoadFailed, committed restore ->
// kExitCodeSuccess, no settlement within the deadline -> kExitCodeTimeout
// (with the decision latched: a timer that fires after a settlement, or vice
// versa, must not re-decide). The timeout scenario leaves its import batch
// PARKED so suite teardown proves the close-time hard shutdown stays clean.
//
// One MainWindow per binary; scenarios share it in declaration order (the
// parked-batch timeout scenario is deliberately LAST).

#include <gtest/gtest.h>

#include <QApplication>
#include <QDeadlineTimer>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <chrono>
#include <memory>
#include <vector>

#include "LayoutExitWatch.h"
#include "dataset_test_helpers.h"
#include "pj_runtime/CatalogModel.h"
#include "support/layout_import_gui_support.h"
#include "support/loader_test_support.h"

namespace {

using PJ::MainWindowLayoutImportTestPeer;
using pj_app_test::pumpUntil;
namespace fake = pj_layout_import_gui;
using Peer = MainWindowLayoutImportTestPeer;

// Spin the loop for a fixed wall-clock window (to prove something does NOT
// happen — a suppressed late timer/settlement).
void pumpFor(int ms) {
  const QDeadlineTimer deadline(ms);
  while (!deadline.hasExpired()) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
  }
}

class MainWindowLayoutExitTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    extensions_dir_ = std::make_unique<QTemporaryDir>();
    project_dir_ = std::make_unique<QTemporaryDir>();
    ASSERT_TRUE(extensions_dir_->isValid());
    ASSERT_TRUE(project_dir_->isValid());
    window_ = std::make_unique<PJ::MainWindow>(extensions_dir_->path());
    ASSERT_TRUE(appSession().extensionCatalog().pluginCatalog().registerStaticToolbox(&fake::kFakeVtable));
  }

  static void TearDownTestSuite() {
    // The timeout scenario left a parked import batch behind on purpose: this
    // reset IS the teardown under test — MainWindow's dtor hard-shuts the
    // still-running batch (children cancelled + joined) without a crash.
    window_.reset();
    project_dir_.reset();
    extensions_dir_.reset();
  }

  [[nodiscard]] static PJ::MainWindow& window() {
    return *window_;
  }
  [[nodiscard]] static PJ::AppSession& appSession() {
    return Peer::session(*window_);
  }

  inline static std::unique_ptr<QTemporaryDir> extensions_dir_;
  inline static std::unique_ptr<QTemporaryDir> project_dir_;
  inline static std::unique_ptr<PJ::MainWindow> window_;
};

// (3): a failed load decides kExitCodeLoadFailed exactly once — and the armed
// timeout, expiring afterwards, must NOT re-decide.
TEST_F(MainWindowLayoutExitTest, LoadFailureDecidesLoadFailedOnceAndSuppressesTheLateTimeout) {
  std::vector<int> codes;
  PJ::LayoutExitWatch watch(window(), std::chrono::milliseconds(300), [&codes](int code) { codes.push_back(code); });
  ASSERT_TRUE(codes.empty()) << "no decision before the load";

  Peer::loadLayout(window(), project_dir_->filePath(QStringLiteral("missing.pj4.xml")), /*interactive=*/false);

  ASSERT_EQ(codes.size(), 1u) << "the failure settlement must decide synchronously";
  EXPECT_EQ(codes.front(), PJ::LayoutExitWatch::kExitCodeLoadFailed);

  pumpFor(400);  // past the 300 ms deadline: the stopped timer must stay quiet
  EXPECT_EQ(codes.size(), 1u) << "a decision is final — the late timeout must not re-fire";
}

// A committed restore decides kExitCodeSuccess.
TEST_F(MainWindowLayoutExitTest, CommittedRestoreDecidesSuccess) {
  const PJ::DatasetId dataset = pj_test::createDataset(appSession(), "plain.mcap", /*own_time_domain=*/true);
  ASSERT_NE(dataset, 0u);
  ASSERT_NE(pj_test::addScalarTopic(appSession(), dataset, "/plain", 1'000, 2'000), 0u);
  appSession().catalogModel().rebuildFromDatastore();
  const QString layout_path = project_dir_->filePath(QStringLiteral("plain.pj4.xml"));
  // The plain sync leg is enough here (the settle suite covers the batch
  // leg); only the settlement -> exit-code mapping is under test.
  ASSERT_TRUE(fake::writeLayoutFile(layout_path, fake::buildGenericCurveLayoutDoc(QStringLiteral("/plain"))));

  std::vector<int> codes;
  PJ::LayoutExitWatch watch(window(), std::chrono::milliseconds(5000), [&codes](int code) { codes.push_back(code); });

  Peer::loadLayout(window(), layout_path, /*interactive=*/false);

  ASSERT_TRUE(pumpUntil([&codes]() { return !codes.empty(); }));
  ASSERT_EQ(codes.size(), 1u);
  EXPECT_EQ(codes.front(), PJ::LayoutExitWatch::kExitCodeSuccess);
}

// (4): a never-settling restore (parked import job) decides kExitCodeTimeout
// after the configured deadline; the batch is still active at that decision
// and is left for the suite teardown to hard-cancel cleanly.
TEST_F(MainWindowLayoutExitTest, NeverSettlingRestoreDecidesTimeoutWhileTheBatchIsStillActive) {
  const QString source_path = project_dir_->filePath(QStringLiteral("data/parked.mcap"));
  const QString layout_path = project_dir_->filePath(QStringLiteral("parked.pj4.xml"));
  ASSERT_TRUE(
      fake::writeImportLayout(
          layout_path, QStringLiteral("/parked"), source_path,
          fake::makeDescriptor(QStringLiteral("trusted"), source_path, QStringLiteral("block"))));

  std::vector<int> codes;
  PJ::LayoutExitWatch watch(window(), std::chrono::milliseconds(1000), [&codes](int code) { codes.push_back(code); });

  Peer::loadLayout(window(), layout_path, /*interactive=*/false);
  ASSERT_TRUE(Peer::progressiveInFlight(window()));
  ASSERT_TRUE(Peer::batchActive(window()));
  EXPECT_TRUE(codes.empty()) << "no decision while the restore is still draining";

  ASSERT_TRUE(pumpUntil([&codes]() { return !codes.empty(); }));
  ASSERT_EQ(codes.size(), 1u);
  EXPECT_EQ(codes.front(), PJ::LayoutExitWatch::kExitCodeTimeout);
  EXPECT_TRUE(Peer::batchActive(window())) << "the batch never settled — timeout fired around it";
  EXPECT_TRUE(Peer::progressiveInFlight(window()));
  // Deliberately NOT released: TearDownTestSuite's window reset must cancel
  // the parked batch via the destructor's hard shutdown without crashing.
}

}  // namespace

PJ_MAIN_WINDOW_TEST_MAIN
