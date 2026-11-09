// SPDX-License-Identifier: GPL-3.0-or-later
#include "types.h"
#include <QDir>
#include <QJsonArray>

namespace Archcodex {
bool validText(const QString &text, int maxLength) {
    if (text.size() > maxLength)
        return false;
    for (auto c : text)
        if (c.unicode() < 32 || c.unicode() == 127)
            return false;
    return true;
}
QString BackendProfile::validate() const {
    if (id.isEmpty() || !validText(id, 128))
        return QStringLiteral("Invalid connection ID.");
    if (name.trimmed().isEmpty() || !validText(name, 160))
        return QStringLiteral("Enter a connection name.");
    if (kind != QStringLiteral("local-unix"))
        return QStringLiteral("This version supports local Unix sockets only.");
    if (!validText(environmentId, 128))
        return QStringLiteral("Invalid execution environment ID.");
    if (!validText(socketPath) || (!socketPath.isEmpty() && !QDir::isAbsolutePath(socketPath)))
        return QStringLiteral("Socket path must be absolute, or empty for the host Codex default.");
    if (!validText(executable) ||
        (executable != QStringLiteral("codex") && !QDir::isAbsolutePath(executable)))
        return QStringLiteral("Codex executable must be 'codex' or an absolute host path.");
    return {};
}
QJsonObject BackendProfile::json() const {
    return {{"id", id},
            {"name", name},
            {"kind", kind},
            {"socket", socketPath},
            {"executable", executable},
            {"autoConnect", autoConnect},
            {"environment", environmentId},
            {"discovered", discovered}};
}
BackendProfile BackendProfile::fromJson(const QJsonObject &o) {
    return {o["id"].toString(),
            o["name"].toString(),
            o["kind"].toString("local-unix"),
            o["socket"].toString(),
            o["executable"].toString("codex"),
            o["autoConnect"].toBool(true),
            o["environment"].toString(),
            o["discovered"].toBool()};
}
bool Session::isActive() const {
    // Runtime presence and turn activity are different. Preserve last-known
    // presence while disconnected, but never claim a current running/waiting state.
    const auto type = status["type"].toString();
    return !type.isEmpty() && type != QStringLiteral("notLoaded");
}
bool Session::isRunning() const {
    return known && status["type"] == "active" && status["activeFlags"].toArray().isEmpty();
}
bool Session::isWaiting() const {
    if (!known)
        return false;
    const auto flags = status["activeFlags"].toArray();
    return status["type"] == "idle" ||
           (status["type"] == "active" &&
            (flags.contains("waitingOnApproval") || flags.contains("waitingOnUserInput")));
}
int Session::attention() const {
    if (!known)
        return 0;
    if (isWaiting() && status["type"] == "active")
        return 4;
    if (isRunning())
        return 2;
    return status["type"].toString() == QStringLiteral("systemError") ? 3 : 1;
}
QString Session::stateText() const {
    if (!known)
        return isActive() ? QStringLiteral("Unknown — last known active")
                          : QStringLiteral("Unknown / disconnected");
    const auto type = status["type"].toString();
    if (type == "active") {
        const auto flags = status["activeFlags"].toArray();
        if (flags.contains("waitingOnApproval"))
            return QStringLiteral("Waiting for approval");
        if (flags.contains("waitingOnUserInput"))
            return QStringLiteral("Waiting for input");
        return flags.isEmpty() ? QStringLiteral("Running")
                               : QStringLiteral("Active — other activity");
    }
    if (type == "idle")
        return QStringLiteral("Waiting for input — ready for a prompt");
    if (type == "notLoaded")
        return QStringLiteral("Saved / not loaded");
    if (type == "systemError")
        return QStringLiteral("Server error");
    return QStringLiteral("Unknown state");
}
Session Session::fromThread(const QString &backend, const QJsonObject &t) {
    Session s;
    s.backendId = backend;
    s.id = t["id"].toString();
    s.cwd = t["cwd"].toString();
    s.title = t["name"].toString().simplified().left(512);
    if (s.title.isEmpty())
        s.title = t["preview"].toString().simplified().left(160);
    if (s.title.isEmpty())
        s.title = QStringLiteral("Untitled session");
    s.status = t["status"].toObject();
    s.known = !s.status.isEmpty();
    s.recency = t["recencyAt"].toVariant().toLongLong();
    if (!s.recency)
        s.recency = t["updatedAt"].toVariant().toLongLong();
    return s;
}
QJsonObject Session::metadata() const {
    // Persist only the list metadata, never complete turns or authentication data.
    return {{"id", id}, {"cwd", cwd}, {"name", title}, {"status", status}, {"recencyAt", recency}};
}
QString sessionKey(const QString &backend, const QString &thread) {
    return backend + QChar(0x1f) + thread;
}
QString projectKey(const QString &backend, const QString &path) {
    return backend + QChar(0x1f) + path;
}
QStringList terminalArguments(const BackendProfile &p, const QString &id) {
    // Constant script; every variable is a separate argv entry. Konsole includes argv[0].
    return {QStringLiteral("/bin/sh"),
            QStringLiteral("-c"),
            QStringLiteral("exec \"$@\""),
            QStringLiteral("archcodex"),
            p.executable,
            QStringLiteral("--remote"),
            p.endpoint(),
            QStringLiteral("resume"),
            QStringLiteral("--"),
            id};
}
} // namespace Archcodex
