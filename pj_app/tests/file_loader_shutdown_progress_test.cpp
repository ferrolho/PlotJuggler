// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Regression coverage for FileLoader's queued single-load progress callback.
// The crashing scenario runs in a subprocess so the parent reports one normal
// GTest failure instead of losing the rest of the FileLoader test executable.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QThread>
#include <QtGlobal>
#include <memory>

#include "FileLoader.h"
#include "pj_runtime/AppSession.h"
#include "pj_runtime/ExtensionCatalogService.h"

using namespace Qt::StringLiterals;

#ifndef PJ_PROGRESS_SHUTDOWN_SOURCE_PLUGIN_PATH
#error "PJ_PROGRESS_SHUTDOWN_SOURCE_PLUGIN_PATH must be defined"
#endif

namespace {

constexpr auto kChildEnv = "PJ_RUN_PROGRESS_SHUTDOWN_CHILD";
constexpr auto kProbeEnv = "PJ_PROGRESS_SHUTDOWN_PROBE_FILE";
constexpr auto kChildFilter = "FileLoaderShutdownProgressChild.DispatchesQueuedProgressAfterJoin";

QByteArray readProbe(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    return {};
  }
  return file.readAll();
}

bool waitForProbeMarker(const QString& path, const QByteArray& marker) {
  QElapsedTimer timer;
  timer.start();
  while (timer.elapsed() < 5000) {
    if (readProbe(path).contains(marker)) {
      return true;
    }
    // Deliberately do not process Qt events here: the progress metacall must
    // remain queued until joinForShutdown has joined and reset ctx_.
    QThread::msleep(1);
  }
  return false;
}

}  // namespace

TEST(FileLoaderShutdownProgressChild, DispatchesQueuedProgressAfterJoin) {
  if (qgetenv(kChildEnv) != QByteArrayLiteral("1")) {
    GTEST_SKIP() << "subprocess-only crash scenario";
  }

  const QString probe_path = QString::fromUtf8(qgetenv(kProbeEnv));
  ASSERT_FALSE(probe_path.isEmpty());
  QFile::remove(probe_path);

  QTemporaryDir extensions_dir;
  QTemporaryDir data_dir;
  ASSERT_TRUE(extensions_dir.isValid());
  ASSERT_TRUE(data_dir.isValid());

  const QString plugin_src = QString::fromUtf8(PJ_PROGRESS_SHUTDOWN_SOURCE_PLUGIN_PATH);
  const QString plugin_dst = extensions_dir.filePath(QFileInfo(plugin_src).fileName());
  ASSERT_TRUE(QFile::copy(plugin_src, plugin_dst)) << "could not stage " << plugin_src.toStdString();

  auto app_session = std::make_unique<PJ::AppSession>(extensions_dir.path());
  ASSERT_FALSE(app_session->extensionCatalog().findSourcesForExtension(u".progressshutdown"_s).empty());
  PJ::FileLoader loader(app_session->sessionManager(), app_session->extensionCatalog(), app_session->catalogModel());

  const QString input_path = data_dir.filePath(u"queued.progressshutdown"_s);
  QFile input(input_path);
  ASSERT_TRUE(input.open(QIODevice::WriteOnly));
  input.close();

  PJ::LoadHints hints;
  hints.expected_plugin_id = u"Progress Shutdown Source"_s;
  hints.preset_config_json = u"{}"_s;
  hints.dialog_policy = PJ::DialogPolicy::kPreferPreset;
  ASSERT_TRUE(loader.loadFile(input_path, nullptr, hints));

  ASSERT_TRUE(waitForProbeMarker(probe_path, QByteArrayLiteral("progress_callback_returned")))
      << "plugin never returned from progressUpdate; no queued callback was proven";

  // The plugin is still running. joinForShutdown requests stop, waits for it,
  // destroys the abandoned load's ctx_, and leaves the queued progress metacall
  // untouched in this GUI thread's event queue.
  loader.joinForShutdown();
  ASSERT_TRUE(readProbe(probe_path).contains(QByteArrayLiteral("stop_observed")))
      << "join did not exercise the running-worker shutdown path";
  ASSERT_FALSE(loader.isBusy());

  // Deliver the straggler tick. Without the generation/ctx_ guard in the
  // queued lambda this would dereference the context joinForShutdown reset —
  // the child process crashing here is exactly what the parent test asserts
  // does not happen.
  QCoreApplication::sendPostedEvents(&loader, QEvent::MetaCall);
}

TEST(FileLoaderShutdownProgressRegression, QueuedProgressAfterJoinMustNotAccessDestroyedContext) {
  if (qgetenv(kChildEnv) == QByteArrayLiteral("1")) {
    GTEST_SKIP() << "parent assertion is not run recursively";
  }

  QTemporaryDir probe_dir;
  ASSERT_TRUE(probe_dir.isValid());
  const QString probe_path = probe_dir.filePath(u"progress_shutdown_probe.log"_s);

  QProcess child;
  child.setProcessChannelMode(QProcess::MergedChannels);
  QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
  env.insert(QString::fromLatin1(kChildEnv), u"1"_s);
  env.insert(QString::fromLatin1(kProbeEnv), probe_path);
  child.setProcessEnvironment(env);
  child.setProgram(QCoreApplication::applicationFilePath());
  child.setArguments(
      {QStringLiteral("--gtest_filter=%1").arg(QString::fromLatin1(kChildFilter)), QStringLiteral("--gtest_color=no")});
  child.start();
  ASSERT_TRUE(child.waitForStarted(5000)) << child.errorString().toStdString();

  if (!child.waitForFinished(15000)) {
    child.kill();
    child.waitForFinished(5000);
    FAIL() << "shutdown/progress child timed out\n" << child.readAll().constData();
  }

  const QByteArray output = child.readAll();
  const QByteArray probe = readProbe(probe_path);
  EXPECT_EQ(child.exitStatus(), QProcess::NormalExit) << "queued progress callback crashed after joinForShutdown\n"
                                                      << "probe:\n"
                                                      << probe.constData() << "child output:\n"
                                                      << output.constData();
  EXPECT_EQ(child.exitCode(), 0) << "shutdown/progress child did not complete cleanly\n"
                                 << "probe:\n"
                                 << probe.constData() << "child output:\n"
                                 << output.constData();
}
