// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "DiagnosticDump.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QEvent>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <Qt>
#include <utility>

#include "pj_base/diagnostic_sink.hpp"
#include "pj_marketplace/qt_diagnostic_bridge.hpp"

namespace PJ {

namespace {

// The bridge emits DiagnosticLevel cast to int; map it back to the stable
// wire strings. Unknown values degrade to "info" rather than failing the run.
[[nodiscard]] QString levelName(int level) {
  switch (static_cast<DiagnosticLevel>(level)) {
    case DiagnosticLevel::kWarning:
      return QStringLiteral("warning");
    case DiagnosticLevel::kError:
      return QStringLiteral("error");
    case DiagnosticLevel::kInfo:
      break;
  }
  return QStringLiteral("info");
}

}  // namespace

DiagnosticDump::DiagnosticDump(QString output_path, QObject* parent)
    : QObject(parent), output_path_(std::move(output_path)) {}

void DiagnosticDump::attachTo(QtDiagnosticBridge* bridge) {
  bridge_ = bridge;
  connect(bridge, &QtDiagnosticBridge::diagnosticReported, this, &DiagnosticDump::record, Qt::UniqueConnection);
}

void DiagnosticDump::record(int level, const QString& source, const QString& id, const QString& message) {
  QJsonObject entry;
  entry.insert(QStringLiteral("level"), levelName(level));
  entry.insert(QStringLiteral("source"), source);
  entry.insert(QStringLiteral("id"), id);
  entry.insert(QStringLiteral("message"), message);
  entry.insert(QStringLiteral("timestamp"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
  records_.append(entry);
}

int DiagnosticDump::size() const {
  return static_cast<int>(records_.size());
}

QString DiagnosticDump::outputPath() const {
  return output_path_;
}

bool DiagnosticDump::write() {
  // The bridge re-emits QUEUED, so a diagnostic reported moments before a
  // quit (the aboutToQuit call context: settle -> QCoreApplication::exit ->
  // write) is still a posted event here. Deliver the bridge's pending
  // metacalls first or the tail of the run — typically the very failure the
  // dump exists to capture — would be lost. Deliberately NARROW: only the
  // bridge's queued re-emits, never the whole posted-event queue — a
  // quit-time write must not run unrelated shutdown-time work.
  if (bridge_ != nullptr) {
    QCoreApplication::sendPostedEvents(bridge_, QEvent::MetaCall);
  }
  QFile file(output_path_);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    return false;
  }
  // Versioned envelope, never a bare array: run-level metadata (app version,
  // exit code, ...) can then be added compatibly without breaking consumers.
  QJsonObject envelope;
  envelope.insert(QStringLiteral("version"), 1);
  envelope.insert(QStringLiteral("records"), records_);
  const QByteArray json = QJsonDocument(envelope).toJson(QJsonDocument::Indented);
  return file.write(json) == json.size();
}

}  // namespace PJ
