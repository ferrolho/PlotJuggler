#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// The --exit-after-layout exit-code decision (stage-5 E5): observes ONE
// MainWindow's layoutRestoreSettled boundary and reports exactly one exit
// code through the injected callback —
//   kExitCodeSuccess    (0)  the restore settled with the layout committed;
//   kExitCodeLoadFailed (1)  the restore settled without committing (open/
//                            parse/apply failure, cancel, cancelled batch);
//   kExitCodeTimeout    (2)  no settlement within the deadline.
// The first decision is FINAL: a timer expiring after a settlement (or a
// settlement arriving after the timeout) never re-fires the callback.
// Because layoutRestoreSettled is emitted only after the import batch
// finished and every restore waiter cleared, a caller quitting on the
// success code can never tear down a still-active batch. main.cpp injects
// QCoreApplication::exit as the callback; tests inject a recorder.

#include <QObject>
#include <QTimer>
#include <chrono>
#include <functional>

namespace PJ {

class MainWindow;

class LayoutExitWatch : public QObject {
  Q_OBJECT

 public:
  static constexpr int kExitCodeSuccess = 0;
  static constexpr int kExitCodeLoadFailed = 1;
  static constexpr int kExitCodeTimeout = 2;

  // Arms the deadline immediately; construct BEFORE the layout load is
  // scheduled so no settlement can precede the subscription. `timeout`
  // beyond QTimer's int-millisecond range (~24.8 days) is clamped to that
  // maximum rather than silently wrapping (the CLI additionally rejects such
  // values as a usage error); a non-positive timeout fires the timeout
  // decision on the next event-loop iteration.
  LayoutExitWatch(
      MainWindow& window, std::chrono::milliseconds timeout, std::function<void(int)> report_exit_code,
      QObject* parent = nullptr);

 private:
  void decide(int code);

  std::function<void(int)> report_exit_code_;
  QTimer timeout_timer_;
  bool decided_ = false;
};

}  // namespace PJ
