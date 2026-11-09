// SPDX-License-Identifier: GPL-3.0-or-later
#include "backend.h"
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QtEndian>
#include <algorithm>
#include <limits>

namespace Archcodex {
static QJsonArray sources() {
    return {"cli",
            "vscode",
            "exec",
            "appServer",
            "subAgent",
            "subAgentReview",
            "subAgentCompact",
            "subAgentThreadSpawn",
            "subAgentOther",
            "unknown"};
}
CodexBackend::CodexBackend(BackendProfile p, const QString &path, QObject *parent)
    : Backend(std::move(p), parent), bridgePath(path) {
    reconnectTimer.setSingleShot(true);
    deadlineTimer.setSingleShot(true);
    connect(&reconnectTimer, &QTimer::timeout, this, &CodexBackend::connectServer);
    connect(&deadlineTimer, &QTimer::timeout, this,
            [this] { failed(QStringLiteral("Server request timed out. Reconnecting…")); });
}
CodexBackend::~CodexBackend() {
    wanted = false;
    shutdownProcess();
    // Reap helper processes on destruction; ordinary reconnect cleanup is asynchronous.
    for (auto *p : findChildren<QProcess *>()) {
        if (p->state() != QProcess::NotRunning && !p->waitForFinished(1000)) {
            p->kill();
            p->waitForFinished(1000);
        }
    }
}
void CodexBackend::shutdownProcess() {
    ++generation;
    initialized = false;
    listing = false;
    pending.clear();
    deadlineTimer.stop();
    buffer.clear();
    reads.clear();
    scheduledReads.clear();
    reading = 0;
    if (process) {
        auto *p = process;
        process = nullptr;
        p->disconnect(this);
        if (p->state() == QProcess::NotRunning)
            p->deleteLater();
        else {
            connect(p, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), p,
                    &QObject::deleteLater);
            p->terminate();
            QTimer::singleShot(1000, p, [p] {
                if (p->state() != QProcess::NotRunning)
                    p->kill();
            });
        }
    }
}
void CodexBackend::disconnectServer() {
    wanted = false;
    reconnectTimer.stop();
    shutdownProcess();
    for (auto &s : sessions) {
        s.known = false;
        emit sessionChanged(s);
    }
    emit connectionChanged(QStringLiteral("Disconnected"), false);
}
void CodexBackend::connectServer() {
    wanted = true;
    reconnectTimer.stop();
    shutdownProcess();
    statuses.clear();
    names.clear();
    removed.clear();
    loaded.clear();
    nextCursor.clear();
    for (auto &s : sessions) {
        s.known = false;
        emit sessionChanged(s);
    }
    const auto error = profile.validate();
    if (!error.isEmpty()) {
        wanted = false;
        emit connectionChanged(error, false);
        return;
    }
    emit connectionChanged(QStringLiteral("Connecting…"), false);
    process = new QProcess(this);
    const auto epoch = generation;
    const auto args = bridgeArguments();
    connect(process, &QProcess::readyReadStandardOutput, this, &CodexBackend::receive);
    // Never accumulate or surface unbounded helper stderr (which can contain host details).
    connect(process, &QProcess::readyReadStandardError, this, [this] {
        if (process)
            process->readAllStandardError();
    });
    connect(process, &QProcess::errorOccurred, this, [this, epoch](QProcess::ProcessError error) {
        if (epoch == generation && error == QProcess::FailedToStart)
            failed(QStringLiteral("Cannot start the packaged socket bridge."));
    });
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this, epoch](int, QProcess::ExitStatus) {
                if (epoch == generation)
                    failed(QStringLiteral(
                        "Connection closed. Check the host Codex executable and socket."));
            });
    connect(process, &QProcess::started, this, [this] {
        request("initialize",
                {{"clientInfo", QJsonObject{{"name", "archcodex"},
                                            {"title", "Archcodex"},
                                            {"version", ARCHCODEX_VERSION}}},
                 {"capabilities", QJsonObject{{"experimentalApi", true}}}},
                [this](const auto &, const QString &err) {
                    if (!err.isEmpty()) {
                        wanted = false;
                        failed("Initialization failed: " + err);
                        return;
                    }
                    send({{"method", "initialized"}});
                    initialized = true;
                    retry = 0;
                    emit connectionChanged(QStringLiteral("Connected — loading history"), true);
                    loadedPage();
                    listPage({});
                });
    });
    process->start(bridgePath, args);
}
void CodexBackend::failed(const QString &reason) {
    shutdownProcess();
    for (auto &s : sessions) {
        s.known = false;
        emit sessionChanged(s);
    }
    emit connectionChanged(reason, false);
    if (wanted)
        reconnectTimer.start(std::min(30000, 1000 * (1 << std::min(retry++, 5))));
}
void CodexBackend::send(const QJsonObject &m) {
    if (!process)
        return;
    const auto bytes = QJsonDocument(m).toJson(QJsonDocument::Compact) + '\n';
    if (process->bytesToWrite() + bytes.size() > 16 * 1024 * 1024) {
        failed("Outgoing request buffer exceeded its limit.");
        return;
    }
    process->write(bytes);
}
void CodexBackend::request(const QString &method, const QJsonObject &params, Reply reply) {
    if (!process) {
        reply({}, "Not connected.");
        return;
    }
    const qint64 id = nextId++;
    pending.insert(id, {std::move(reply), QDateTime::currentMSecsSinceEpoch() + 30000});
    send({{"id", id}, {"method", method}, {"params", params}});
    armDeadline();
}
void CodexBackend::armDeadline() {
    if (pending.isEmpty()) {
        deadlineTimer.stop();
        return;
    }
    qint64 soon = std::numeric_limits<qint64>::max();
    for (const auto &p : pending)
        soon = std::min(soon, p.deadline);
    deadlineTimer.start(
        static_cast<int>(std::max<qint64>(1, soon - QDateTime::currentMSecsSinceEpoch())));
}
void CodexBackend::receive() {
    if (!process)
        return;
    buffer += process->readAllStandardOutput();
    while (buffer.size() >= 4) {
        const auto n = qFromBigEndian<quint32>(buffer.constData());
        if (n > 16 * 1024 * 1024) {
            failed("Server message exceeded 16 MiB.");
            return;
        }
        if (buffer.size() < 4 + n)
            return;
        QJsonParseError error;
        const auto doc = QJsonDocument::fromJson(buffer.mid(4, n), &error);
        buffer.remove(0, 4 + n);
        if (error.error != QJsonParseError::NoError || !doc.isObject()) {
            failed("Server sent malformed JSON.");
            return;
        }
        const auto epoch = generation;
        message(doc.object());
        if (epoch != generation)
            return;
    }
}
void CodexBackend::message(const QJsonObject &m) {
    if (m.contains("id") && !m.contains("method")) {
        const auto id = m["id"].toVariant().toLongLong();
        if (!pending.contains(id))
            return;
        auto callback = pending.take(id).reply;
        armDeadline();
        callback(m["result"].toObject(),
                 m.contains("error")
                     ? m["error"].toObject()["message"].toString("Server rejected the request.")
                     : QString());
        return;
    }
    // This connection is an observer: never claim, approve, deny or cancel server requests.
    if (m.contains("id"))
        return;
    const auto method = m["method"].toString();
    const auto p = m["params"].toObject();
    const auto id = p["threadId"].toString();
    if (!id.isEmpty() && !validText(id, 128)) {
        failed("Server sent an invalid thread identifier.");
        return;
    }
    if ((method == "thread/status/changed" || method == "thread/closed") && !id.isEmpty()) {
        statuses[id] =
            method == "thread/closed" ? QJsonObject{{"type", "notLoaded"}} : p["status"].toObject();
        if (sessions.contains(id)) {
            auto &s = sessions[id];
            s.status = statuses[id];
            s.known = true;
            emit sessionChanged(s);
        } else
            readThread(id);
    } else if (method == "thread/started") {
        const auto thread = p["thread"].toObject();
        const auto startedId = thread["id"].toString();
        if (thread["status"].isObject())
            statuses[startedId] = thread["status"].toObject();
        upsert(thread);
    } else if (method == "thread/name/updated" || method == "thread/nameUpdated") {
        names[id] = p["threadName"].toString();
        if (sessions.contains(id)) {
            sessions[id].title = names[id];
            emit sessionChanged(sessions[id]);
        }
    } else if (method == "thread/archived" || method == "thread/deleted") {
        removed.insert(id);
        sessions.remove(id);
        statuses.remove(id);
        names.remove(id);
        emit sessionRemoved(id);
    } else if (method == "thread/unarchived") {
        removed.remove(id);
        readThread(id);
    }
}
void CodexBackend::upsert(QJsonObject t) {
    const auto id = t["id"].toString();
    if (id.isEmpty() || !validText(id, 128) || removed.contains(id))
        return;
    // Events received since this connection started override any concurrent inventory snapshot.
    if (statuses.contains(id))
        t["status"] = statuses[id];
    if (names.contains(id))
        t["name"] = names[id];
    auto s = Session::fromThread(profile.id, t);
    if (!validText(s.cwd))
        return;
    sessions[id] = s;
    emit sessionChanged(s);
}
void CodexBackend::listPage(const QString &cursor) {
    if (listing || !initialized)
        return;
    listing = true;
    QJsonObject p = {{"limit", 200},
                     {"sortKey", "updated_at"},
                     {"sortDirection", "desc"},
                     {"modelProviders", QJsonArray{}},
                     {"sourceKinds", sources()}};
    if (!cursor.isEmpty())
        p["cursor"] = cursor;
    request("thread/list", p, [this, cursor](const auto &r, const QString &error) {
        listing = false;
        if (!error.isEmpty()) {
            emit connectionChanged("History unavailable: " + error, true);
            return;
        }
        for (const auto &t : r["data"].toArray())
            upsert(t.toObject());
        nextCursor = r["nextCursor"].toString();
        if (!nextCursor.isEmpty() && nextCursor == cursor)
            nextCursor.clear();
        emit moreAvailable(!nextCursor.isEmpty());
        emit connectionChanged(
            nextCursor.isEmpty() ? "Connected" : "Connected — more history available", true);
    });
}
void CodexBackend::loadMore() {
    if (!nextCursor.isEmpty())
        listPage(nextCursor);
}
void CodexBackend::loadedPage(const QString &cursor) {
    QJsonObject p = {{"limit", 200}};
    if (!cursor.isEmpty())
        p["cursor"] = cursor;
    request("thread/loaded/list", p, [this, cursor](const auto &r, const QString &error) {
        if (!error.isEmpty()) {
            emit connectionChanged("Connected — loaded-session inventory unavailable: " + error,
                                   true);
            return;
        }
        for (const auto &id : r["data"].toArray()) {
            loaded.insert(id.toString());
            readThread(id.toString());
        }
        const auto next = r["nextCursor"].toString();
        if (!next.isEmpty() && next == cursor) {
            emit connectionChanged(
                "Connected — loaded-session inventory repeated a page; coverage is incomplete",
                true);
            return;
        }
        if (!next.isEmpty())
            loadedPage(next);
        else {
            // Only reconcile after the complete inventory, never after an error
            // or a partial page. Events received during it take precedence.
            for (auto it = statuses.cbegin(); it != statuses.cend(); ++it) {
                if (it.value()["type"] == "notLoaded")
                    loaded.remove(it.key());
                else
                    loaded.insert(it.key());
            }
            for (auto &s : sessions)
                if (!s.known && !loaded.contains(s.id)) {
                    s.status = {{"type", "notLoaded"}};
                    s.known = true;
                    emit sessionChanged(s);
                }
            emit loadedInventory(loaded);
        }
    });
}
void CodexBackend::readThread(const QString &id) {
    if (id.isEmpty() || !validText(id, 128) || scheduledReads.contains(id))
        return;
    if (scheduledReads.size() >= 4096) {
        failed("Too many pending metadata events.");
        return;
    }
    scheduledReads.insert(id);
    reads.enqueue(id);
    pumpReads();
}
void CodexBackend::pumpReads() {
    while (initialized && reading < 4 && !reads.isEmpty()) {
        const auto id = reads.dequeue();
        ++reading;
        request("thread/read", {{"threadId", id}, {"includeTurns", false}},
                [this, id](const auto &r, const QString &error) {
                    --reading;
                    scheduledReads.remove(id);
                    if (error.isEmpty())
                        upsert(r["thread"].toObject());
                    pumpReads();
                });
    }
}
void CodexBackend::search(const QString &query, quint64 token) {
    const auto sid = ++searchGeneration;
    if (query.isEmpty())
        return;
    if (!initialized) {
        emit searchResults(token, {}, "Disconnected — search coverage incomplete.", true);
        return;
    }
    searchPage(query, {}, token, sid, 0);
}
void CodexBackend::searchPage(const QString &query, const QString &cursor, quint64 token,
                              quint64 sid, int pages) {
    QJsonObject p = {{"searchTerm", query},
                     {"limit", 100},
                     {"sortKey", "updated_at"},
                     {"sortDirection", "desc"},
                     {"sourceKinds", sources()}};
    if (!cursor.isEmpty())
        p["cursor"] = cursor;
    request("thread/search", p,
            [this, query, cursor, token, sid, pages](const auto &r, const QString &error) {
                if (sid != searchGeneration)
                    return;
                if (!error.isEmpty()) {
                    emit searchResults(token, {}, "Conversation search unavailable: " + error,
                                       true);
                    return;
                }
                QList<Session> matches;
                for (const auto &v : r["data"].toArray()) {
                    auto t = v.toObject()["thread"].toObject();
                    auto s = Session::fromThread(profile.id, t);
                    if (!s.id.isEmpty() && validText(s.id, 128) && validText(s.cwd))
                        matches.append(s);
                }
                const auto next = r["nextCursor"].toString();
                const bool done = next.isEmpty() || next == cursor || pages >= 99;
                emit searchResults(
                    token, matches,
                    pages >= 99 ? "Search stopped at 10,000 results; narrow the query." : QString(),
                    done);
                if (!done)
                    searchPage(query, next, token, sid, pages + 1);
            });
}
} // namespace Archcodex
