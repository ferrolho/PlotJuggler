#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Test support shared by the network-facing pj_runtime test binaries
// (update_checker_test, telemetry_ping_test): a signal-spy event-loop pump and
// a loopback HTTP/1.1 server with a configurable canned response. The server
// records the full request (headers + body, reassembled across TCP chunks via
// Content-Length) before replying, so tests can assert what was sent.

#include <QByteArray>
#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrl>

namespace PJ::test {

// Spins the event loop until `spy` receives a signal or the timeout expires.
inline bool waitForSignal(QSignalSpy& spy, int timeout_ms = 5000) {
  QDeadlineTimer deadline(timeout_ms);
  while (spy.isEmpty() && !deadline.hasExpired()) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
  }
  return !spy.isEmpty();
}

class LocalHttpServer {
 public:
  LocalHttpServer() {
    server_.listen(QHostAddress::LocalHost, 0);
    QObject::connect(&server_, &QTcpServer::newConnection, [this]() {
      QTcpSocket* socket = server_.nextPendingConnection();
      socket->setParent(&server_);
      QObject::connect(socket, &QTcpSocket::readyRead, [this, socket]() {
        request_ += socket->readAll();
        if (!requestComplete()) {
          return;  // body still arriving in a later chunk
        }
        socket->write(response_);
        socket->flush();
        socket->disconnectFromHost();
      });
    });
  }

  QUrl url() const {
    return QUrl(QStringLiteral("http://127.0.0.1:%1/").arg(server_.serverPort()));
  }

  void setResponse(const QByteArray& status_line, const QByteArray& body) {
    response_ = "HTTP/1.1 " + status_line +
                "\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(body.size()) +
                "\r\nConnection: close\r\n\r\n" + body;
  }

  // The request body (bytes after the blank line); empty until complete.
  QByteArray requestBody() const {
    const qsizetype header_end = request_.indexOf("\r\n\r\n");
    return header_end < 0 ? QByteArray() : request_.mid(header_end + 4);
  }

  QByteArray requestHeaders() const {
    const qsizetype header_end = request_.indexOf("\r\n\r\n");
    return header_end < 0 ? request_ : request_.left(header_end);
  }

 private:
  bool requestComplete() const {
    const qsizetype header_end = request_.indexOf("\r\n\r\n");
    if (header_end < 0) {
      return false;
    }
    const QByteArray headers = request_.left(header_end).toLower();
    const qsizetype cl_pos = headers.indexOf("content-length:");
    const int content_length =
        cl_pos < 0 ? 0 : headers.mid(cl_pos + 15, headers.indexOf("\r\n", cl_pos) - cl_pos - 15).trimmed().toInt();
    return request_.size() - (header_end + 4) >= content_length;
  }

  QTcpServer server_;
  QByteArray request_;
  QByteArray response_;
};

}  // namespace PJ::test
