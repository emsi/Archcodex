// SPDX-License-Identifier: GPL-3.0-or-later
#include "types.h"
#include "launch.h"
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>

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
    if (kind != "local-unix" && !isContainer())
        return QStringLiteral("Unsupported connection type.");
    if (isContainer()) {
        static const QRegularExpression cid("^[a-f0-9]{64}$");
        if (!cid.match(containerId).hasMatch() || containerEpoch.isEmpty() ||
            !validText(containerEpoch, 80) || (!sshHost.isEmpty() && !validSshHost(sshHost)) ||
            dockerContext.isEmpty() || dockerContext.startsWith('-') ||
            !validText(dockerContext, 256) || runtimeUser.isEmpty() ||
            runtimeUser.startsWith('-') || !validText(runtimeUser, 128) ||
            !runtimeHome.startsWith('/') || !validText(runtimeHome) ||
            !validText(discoveryHost, 128) || discoveryHost.isEmpty())
            return QStringLiteral("Invalid container identity or execution context.");
        if (socketPath.isEmpty())
            return QStringLiteral("Select an explicitly discovered container server.");
    }
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
            {"discovered", discovered},
            {"identity", identity},
            {"sshHost", sshHost},
            {"dockerContext", dockerContext},
            {"containerId", containerId},
            {"containerEpoch", containerEpoch},
            {"runtimeUser", runtimeUser},
            {"runtimeHome", runtimeHome},
            {"discoveryHost", discoveryHost}};
}
BackendProfile BackendProfile::fromJson(const QJsonObject &o) {
    return {o["id"].toString(),
            o["name"].toString(),
            o["kind"].toString("local-unix"),
            o["socket"].toString(),
            o["executable"].toString("codex"),
            o["autoConnect"].toBool(true),
            o["environment"].toString(),
            o["discovered"].toBool(),
            o["identity"].toObject(),
            o["sshHost"].toString(),
            o["dockerContext"].toString(),
            o["containerId"].toString(),
            o["containerEpoch"].toString(),
            o["runtimeUser"].toString(),
            o["runtimeHome"].toString(),
            o["discoveryHost"].toString(),
            {}};
}
bool Session::isSaved() const {
    // Paginated history lives in the server's store and need not have a rollout file.
    return !ephemeral && !unavailable && (!rolloutPath.isEmpty() || historyMode == "paginated");
}
bool Session::isActive() const {
    // Runtime presence and turn activity are different. Preserve last-known
    // presence while disconnected, but never claim a current running/waiting state.
    const auto type = status["type"].toString();
    return terminalOpen || (!unavailable && !type.isEmpty() && type != QStringLiteral("notLoaded"));
}
bool Session::isRunning() const {
    return clientIssue.isEmpty() && !unavailable && known && status["type"] == "active" &&
           status["activeFlags"].toArray().isEmpty();
}
bool Session::isWaiting() const {
    if (!known || unavailable || !clientIssue.isEmpty())
        return false;
    const auto flags = status["activeFlags"].toArray();
    return status["type"] == "idle" ||
           (status["type"] == "active" &&
            (flags.contains("waitingOnApproval") || flags.contains("waitingOnUserInput")));
}
int Session::attention() const {
    if (!clientIssue.isEmpty())
        return 3;
    if (!known)
        return 0;
    if (isWaiting())
        return 4;
    if (isRunning())
        return 2;
    return status["type"].toString() == QStringLiteral("systemError") ? 3 : 1;
}
QString Session::stateText() const {
    if (!clientIssue.isEmpty())
        return clientIssue;
    if (unavailable)
        return QStringLiteral("Saved session unavailable on this server");
    if (contextRequired && !executionObserved)
        return QStringLiteral("Shared history — execution context not established here");
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
    s.rolloutPath = t["path"].toString();
    s.historyMode = t["historyMode"].toString();
    s.ephemeral = t["ephemeral"].toBool();
    s.persistenceKnown = t.contains("path") || s.ephemeral || t.contains("historyMode");
    s.unavailable = t["unavailable"].toBool();
    s.contextRequired = t["archcodexContextRequired"].toBool();
    s.executionObserved = t["archcodexExecutionObserved"].toBool();
    s.executionEnvironment = t["archcodexExecutionEnvironment"].toString();
    s.known = !s.status.isEmpty();
    s.recency = t["recencyAt"].toVariant().toLongLong();
    if (!s.recency)
        s.recency = t["updatedAt"].toVariant().toLongLong();
    return s;
}
QJsonObject Session::metadata() const {
    // Persist only the list metadata, never complete turns or authentication data.
    auto data = QJsonObject{{"id", id},
                            {"cwd", cwd},
                            {"name", title},
                            {"status", status},
                            {"recencyAt", recency},
                            {"unavailable", unavailable},
                            {"archcodexContextRequired", contextRequired},
                            {"archcodexExecutionObserved", executionObserved},
                            {"archcodexExecutionEnvironment", executionEnvironment}};
    if (persistenceKnown) {
        data["path"] =
            rolloutPath.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(rolloutPath);
        data["ephemeral"] = ephemeral;
        data["historyMode"] = historyMode;
    }
    return data;
}
QString sessionKey(const QString &backend, const QString &thread) {
    return backend + QChar(0x1f) + thread;
}
QString projectKey(const QString &backend, const QString &path) {
    return backend + QChar(0x1f) + path;
}
QStringList terminalArguments(const BackendProfile &p, const QString &id) {
    return clientArguments(p, id);
}
QString detectedLocalCodex(const QJsonArray &endpoints) {
    QStringList defaults, others;
    for (const auto &value : endpoints) {
        const auto endpoint = value.toObject();
        const auto executable = endpoint["executable"].toString();
        if (!endpoint["hostEnvironment"].toBool() || !validText(executable) ||
            !QDir::isAbsolutePath(executable))
            continue;
        (endpoint["default"].toBool() ? defaults : others).append(executable);
    }
    // Prefer the default local server. Stable order avoids selection changes
    // caused solely by /proc iteration order when several local versions run.
    defaults.sort();
    others.sort();
    return defaults.isEmpty() ? others.value(0) : defaults.first();
}
QStringList clientArguments(const BackendProfile &p, const QString &id, const QString &cwd,
                            const QString &localExecutable) {
    if (p.isContainer() && p.relaySocket.isEmpty())
        return {}; // Never fall back to a default or a stale container command.
    if (p.isContainer()) {
        const QJsonObject request{{"socket", p.relaySocket},
                                  {"thread", id},
                                  {"cwd", cwd},
                                  {"executable", localExecutable}};
        return QStringList{"/bin/sh", "-c", "exec \"$@\"", "archcodex"} +
               transportCommand(
                   "client", QString::fromLatin1(
                                 QJsonDocument(request).toJson(QJsonDocument::Compact).toBase64()));
    }
    // Constant script; every variable is a separate argv entry. Konsole includes argv[0].
    auto args = QStringList{QStringLiteral("/bin/sh"),
                            QStringLiteral("-c"),
                            QStringLiteral("exec \"$@\""),
                            QStringLiteral("archcodex"),
                            p.executable,
                            QStringLiteral("--remote"),
                            p.endpoint()};
    if (id.isEmpty())
        args << "--cd" << cwd;
    else
        args << "resume" << "--" << id;
    return args;
}
QString serverLabel(const BackendProfile &p) {
    if (!p.discovered)
        return p.name;
    if (p.isContainer())
        return ("myCodex · " + p.identity["containerName"].toString() + " · " +
                (p.sshHost.isEmpty() ? "This computer" : p.sshHost) + " · " +
                p.identity["stateMode"].toString("Unknown storage"))
            .left(150);
    const auto home = p.identity["codexHome"].toString();
    const auto ownerHome = p.identity["ownerHome"].toString();
    QString label;
    if (p.identity["default"].toBool())
        label = "Main";
    else if (!home.isEmpty() && home != ownerHome + "/.codex")
        label = home.startsWith(ownerHome + "/") && !ownerHome.isEmpty()
                    ? "~" + home.mid(ownerHome.size())
                    : home;
    else {
        const auto cwd = p.identity["cwd"].toString();
        label = "Codex";
        if (!cwd.isEmpty() && cwd != ownerHome && cwd != "/")
            label += " · " + QFileInfo(cwd).fileName();
    }
    const auto user = p.identity["user"].toString(), host = p.identity["host"].toString();
    if (!user.isEmpty() || !host.isEmpty())
        label += " · " + user + (host.isEmpty() ? QString() : "@" + host);
    return label.left(150);
}
QString serverDetails(const BackendProfile &p) {
    QStringList lines{p.name, "Socket: " + p.endpoint(), "Executable: " + p.executable};
    if (p.isContainer())
        lines << "Docker context: " + p.dockerContext << "Container: " + p.containerId
              << "Container WORKDIR: " +
                     p.identity["containerWorkdir"].toString("Unknown until rediscovery")
              << "Runtime user: " + p.runtimeUser << "Container started: " + p.containerEpoch;
    for (const auto &field :
         {"codexHome", "cwd", "user", "host", "pid", "userAgent", "stateMode", "stateVolume"}) {
        const auto value = p.identity[field].toVariant().toString();
        if (!value.isEmpty())
            lines << QString::fromLatin1(field) + ": " + value;
    }
    return lines.join('\n');
}
QString serverTreeLabel(const BackendProfile &p) {
    if (!p.discovered)
        return p.name;
    if (p.isContainer())
        return p.identity["containerName"].toString("Container").left(150);
    auto local = p;
    local.identity.remove("user");
    local.identity.remove("host");
    return serverLabel(local);
}
} // namespace Archcodex
