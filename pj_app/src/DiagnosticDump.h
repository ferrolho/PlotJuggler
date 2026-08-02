#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// The --dump-diagnostics observation channel: accumulates EVERY diagnostic
// reported through a QtDiagnosticBridge for the lifetime of the run and
// serializes them to a JSON file on demand (main.cpp calls write() from
// QCoreApplication::aboutToQuit, which fires on every event-loop exit —
// including QCoreApplication::exit(nonzero), so failure paths get the file
// too). Deliberately NOT a view over DiagnosticHistory: that bell-service
// ring caps at 200 records, and a long headless run must not silently
// truncate — this collector subscribes and accumulates unbounded instead.
//
// File shape (STABLE — scripts assert on `id` values, never message text):
//   {"version": 1,
//    "records": [{"level": "info" | "warning" | "error",
//                 "source": "...", "id": "...", "message": "...",
//                 "timestamp": "<ISO 8601 UTC with milliseconds>"}, ...]}
// The top level is a versioned ENVELOPE, never a bare array, so run-level
// metadata (schema/app version, exit code, ...) can be added compatibly
// later; `version` bumps on any incompatible change. The timestamp is
// stamped at (queued) delivery time, the same convention as
// DiagnosticHistory. Because the bridge re-emits QUEUED, attaching any time
// before the event loop first runs also captures diagnostics sunk earlier
// (e.g. during MainWindow construction) — nothing is delivered until the
// loop spins.

#include <QJsonArray>
#include <QObject>
#include <QPointer>
#include <QString>

namespace PJ {

class QtDiagnosticBridge;

class DiagnosticDump : public QObject {
  Q_OBJECT

 public:
  explicit DiagnosticDump(QString output_path, QObject* parent = nullptr);

  // Subscribe to every diagnosticReported of `bridge`. May be called before
  // the event loop runs to capture the whole run (see the header comment).
  // One bridge per collector (main.cpp's shape): attaching the SAME bridge
  // twice is a no-op (unique connection), while attaching a different
  // bridge adds a second subscription but re-targets the quit-time drain to
  // the most recently attached one.
  void attachTo(QtDiagnosticBridge* bridge);

  // Append one record. `level` is a PJ::DiagnosticLevel cast to int (the
  // bridge's signal shape); unknown values serialize as "info".
  void record(int level, const QString& source, const QString& id, const QString& message);

  [[nodiscard]] int size() const;
  [[nodiscard]] QString outputPath() const;

  // Serialize the envelope (records oldest first) to the output path, after
  // draining the attached bridge's still-queued deliveries (a quit-time
  // write must not lose the tail of the run). Returns false when the file
  // cannot be written.
  [[nodiscard]] bool write();

 private:
  QString output_path_;
  QJsonArray records_;
  // The drain target: only THIS receiver's queued re-emits are delivered at
  // write() time — never the whole posted-event queue (a quit-time write
  // must not run unrelated shutdown work).
  QPointer<QtDiagnosticBridge> bridge_;
};

}  // namespace PJ
