// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_runtime/HttpGet.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <utility>

namespace PJ {

namespace {

// Shared tail of every one-shot request: deliver the finished reply to the
// callback on `context`'s thread, then release it.
QNetworkReply* attachFinished(QNetworkReply* reply, QObject* context, std::function<void(QNetworkReply&)> on_finished) {
  QObject::connect(reply, &QNetworkReply::finished, context, [reply, callback = std::move(on_finished)]() {
    callback(*reply);
    reply->deleteLater();
  });
  return reply;
}

}  // namespace

QNetworkReply* httpGetWithTimeout(
    QNetworkAccessManager& network, QNetworkRequest request, std::chrono::milliseconds timeout, QObject* context,
    std::function<void(QNetworkReply&)> on_finished) {
  request.setTransferTimeout(static_cast<int>(timeout.count()));
  return attachFinished(network.get(request), context, std::move(on_finished));
}

QNetworkReply* httpPostWithTimeout(
    QNetworkAccessManager& network, QNetworkRequest request, const QByteArray& body, std::chrono::milliseconds timeout,
    QObject* context, std::function<void(QNetworkReply&)> on_finished) {
  request.setTransferTimeout(static_cast<int>(timeout.count()));
  return attachFinished(network.post(request, body), context, std::move(on_finished));
}

}  // namespace PJ
