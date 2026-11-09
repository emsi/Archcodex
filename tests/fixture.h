// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "backend.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPointer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QWebSocket>
#include <QWebSocketServer>
using namespace Archcodex;
class Fixture : public QObject {
  public:
    QTemporaryDir directory;
    QLocalServer local;
    QWebSocketServer server{"fixture", QWebSocketServer::NonSecureMode};
    QPointer<QWebSocket> peer;
    QStringList methods;
    QStringList loaded;
    bool delaySnapshot = false;
    bool failLoadedInventory = false;
    QJsonValue snapshotId = QJsonValue::Undefined;
    Fixture() {
        if (!server.listen(QHostAddress::LocalHost, 0) || !local.listen(path()))
            qFatal("Fixture listen failed");
        connect(&local, &QLocalServer::newConnection, this, [this] {
            auto *u = local.nextPendingConnection();
            auto *tcp = new QTcpSocket(u);
            auto forward = [u, tcp] {
                if (tcp->state() == QAbstractSocket::ConnectedState)
                    tcp->write(u->readAll());
            };
            connect(u, &QLocalSocket::readyRead, this, forward);
            connect(tcp, &QTcpSocket::connected, this, forward);
            connect(tcp, &QTcpSocket::readyRead, u, [u, tcp] { u->write(tcp->readAll()); });
            connect(u, &QLocalSocket::disconnected, u, &QObject::deleteLater);
            connect(tcp, &QTcpSocket::disconnected, u, &QLocalSocket::disconnectFromServer);
            tcp->connectToHost(QHostAddress::LocalHost, server.serverPort());
        });
        connect(&server, &QWebSocketServer::newConnection, this, [this] {
            peer = server.nextPendingConnection();
            peer->setParent(this);
            connect(peer, &QWebSocket::textMessageReceived, this, [this](const QString &text) {
                const auto m = QJsonDocument::fromJson(text.toUtf8()).object();
                const auto method = m["method"].toString();
                methods << method;
                if (method == "initialize")
                    reply(m["id"], {{"userAgent", "fixture"}});
                else if (method == "thread/list") {
                    if (delaySnapshot)
                        snapshotId = m["id"];
                    else
                        reply(m["id"],
                              {{"data", QJsonArray{thread()}}, {"nextCursor", QJsonValue::Null}});
                } else if (method == "thread/loaded/list") {
                    if (failLoadedInventory)
                        peer->sendTextMessage(QString::fromUtf8(
                            QJsonDocument(
                                QJsonObject{
                                    {"id", m["id"]},
                                    {"error", QJsonObject{{"code", -32601},
                                                          {"message", "Inventory unavailable"}}}})
                                .toJson()));
                    else
                        reply(m["id"], {{"data", QJsonArray::fromStringList(loaded)}});
                } else if (method == "thread/read")
                    reply(m["id"],
                          {{"thread", thread(m["params"].toObject()["threadId"].toString())}});
                else if (method == "thread/search") {
                    const bool second = m["params"].toObject().contains("cursor");
                    reply(m["id"],
                          {{"data",
                            QJsonArray{QJsonObject{{"thread", thread(second ? "second" : "first")},
                                                   {"snippet", "needle"}}}},
                           {"nextCursor",
                            second ? QJsonValue(QJsonValue::Null) : QJsonValue("page2")}});
                }
            });
        });
    }
    QString path() const {
        return directory.path() + "/server.sock";
    }
    QJsonObject thread(const QString &id = "thread-1") const {
        return {{"id", id},
                {"name", "Example"},
                {"cwd", "/work/project"},
                {"updatedAt", 123},
                {"status", QJsonObject{{"type", "idle"}}}};
    }
    void reply(QJsonValue id, const QJsonObject &result) {
        peer->sendTextMessage(
            QString::fromUtf8(QJsonDocument(QJsonObject{{"id", id}, {"result", result}})
                                  .toJson(QJsonDocument::Indented)));
    }
    void notify(const QString &method, const QJsonObject &params) {
        peer->sendTextMessage(
            QString::fromUtf8(QJsonDocument(QJsonObject{{"method", method}, {"params", params}})
                                  .toJson(QJsonDocument::Compact)));
    }
    BackendProfile profile() const {
        return {"backend-1", "Local", "local-unix", path(), "codex", false};
    }
};
