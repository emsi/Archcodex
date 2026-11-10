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
    QHash<QString, QString> readErrors;
    QJsonObject threadFields;
    QJsonObject forkParams;
    QString forkError;
    QList<QJsonObject> starts, renames;
    QHash<QString, QJsonObject> createdThreads;
    QJsonObject startFields;
    QString startError, nameError;
    bool delayStart = false, delayName = false;
    QStringList readIds;
    bool delaySnapshot = false;
    bool delayRead = false;
    bool delaySearch = false;
    QList<QJsonValue> searchIds;
    QJsonValue readId = QJsonValue::Undefined;
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
                    reply(m["id"],
                          {{"userAgent", "fixture"}, {"codexHome", "/home/test/.codex-work"}});
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
                } else if (method == "thread/read") {
                    const auto id = m["params"].toObject()["threadId"].toString();
                    readIds << id;
                    if (delayRead)
                        readId = m["id"];
                    else if (readErrors.contains(id))
                        peer->sendTextMessage(QString::fromUtf8(
                            QJsonDocument(
                                QJsonObject{{"id", m["id"]},
                                            {"error", QJsonObject{{"code", -32600},
                                                                  {"message", readErrors[id]}}}})
                                .toJson()));
                    else
                        reply(m["id"], {{"thread", thread(id)}});
                } else if (method == "thread/start") {
                    starts << m;
                    if (delayStart)
                        return;
                    if (!startError.isEmpty()) {
                        reject(m["id"], startError);
                        return;
                    }
                    const auto id = "created-" + QString::number(starts.size());
                    auto t = thread(id);
                    t["cwd"] = m["params"].toObject()["cwd"];
                    t["name"] = QJsonValue::Null;
                    for (auto it = startFields.begin(); it != startFields.end(); ++it)
                        t[it.key()] = it.value();
                    createdThreads[id] = t;
                    reply(m["id"], {{"thread", t}});
                    notify("thread/started", {{"thread", t}});
                } else if (method == "thread/name/set") {
                    renames << m;
                    if (delayName)
                        return;
                    if (!nameError.isEmpty()) {
                        reject(m["id"], nameError);
                        return;
                    }
                    const auto params = m["params"].toObject();
                    const auto id = params["threadId"].toString();
                    createdThreads[id]["name"] = params["name"];
                    reply(m["id"], {});
                    notify("thread/name/updated",
                           {{"threadId", id}, {"threadName", params["name"]}});
                } else if (method == "thread/fork") {
                    forkParams = m["params"].toObject();
                    if (forkError.isEmpty())
                        reply(m["id"], {{"thread", thread("fork-1")}});
                    else
                        peer->sendTextMessage(QString::fromUtf8(
                            QJsonDocument(
                                QJsonObject{{"id", m["id"]},
                                            {"error", QJsonObject{{"message", forkError}}}})
                                .toJson()));
                } else if (method == "thread/search") {
                    searchIds << m["id"];
                    if (delaySearch)
                        return;
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
        QJsonObject result{{"id", id},
                           {"path", "/saved/" + id + ".jsonl"},
                           {"ephemeral", false},
                           {"name", "Example"},
                           {"cwd", "/work/project"},
                           {"updatedAt", 123},
                           {"status", QJsonObject{{"type", "idle"}}}};
        for (auto it = threadFields.begin(); it != threadFields.end(); ++it)
            result[it.key()] = it.value();
        if (createdThreads.contains(id))
            return createdThreads.value(id);
        return result;
    }
    void reject(QJsonValue id, const QString &error) {
        peer->sendTextMessage(QString::fromUtf8(
            QJsonDocument(QJsonObject{{"id", id}, {"error", QJsonObject{{"message", error}}}})
                .toJson()));
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
