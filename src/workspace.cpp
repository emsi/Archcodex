// SPDX-License-Identifier: GPL-3.0-or-later
#include "workspace.h"
#include <QCryptographicHash>
#include <QDir>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <stdexcept>

namespace Archcodex {
QString WorkspaceView::key() const {
    return sessionKey(backendId, threadId.isEmpty() ? "new-" + id : threadId);
}
QJsonObject WorkspaceView::json() const {
    return {{"id", id},
            {"backend", backendId},
            {"thread", threadId},
            {"title", title},
            {"cwd", cwd},
            {"sourceIdentity", sourceIdentity},
            {"executionEnvironment", executionEnvironment},
            {"executionObserved", executionObserved},
            {"disposition", disposition == ViewDisposition::Automatic      ? "automatic"
                            : disposition == ViewDisposition::Disconnected ? "disconnected"
                                                                           : "unresolved"},
            {"summary", summary}};
}
WorkspaceView WorkspaceView::fromJson(const QJsonObject &o) {
    WorkspaceView v;
    for (const auto *key : {"id", "backend", "thread", "title", "cwd", "sourceIdentity",
                            "executionEnvironment", "disposition", "summary"})
        if (!o[key].isString())
            throw std::runtime_error("Invalid terminal workspace field.");
    if (!o["executionObserved"].isBool())
        throw std::runtime_error("Invalid terminal execution evidence.");
    v.id = o["id"].toString();
    v.backendId = o["backend"].toString();
    v.threadId = o["thread"].toString();
    v.title = o["title"].toString();
    v.cwd = o["cwd"].toString();
    v.sourceIdentity = o["sourceIdentity"].toString();
    v.executionEnvironment = o["executionEnvironment"].toString();
    v.executionObserved = o["executionObserved"].toBool();
    v.summary = o["summary"].toString();
    const auto disposition = o["disposition"].toString();
    if (disposition == "automatic")
        v.disposition = ViewDisposition::Automatic;
    else if (disposition == "disconnected")
        v.disposition = ViewDisposition::Disconnected;
    else if (disposition == "unresolved")
        v.disposition = ViewDisposition::Unresolved;
    else
        throw std::runtime_error("Unsupported terminal workspace disposition.");
    return v;
}
QString WorkspaceSnapshot::validate() const {
    if (revision < 0 || revision > 9007199254740991LL || views.size() > 512 ||
        recent.size() != views.size())
        return "Invalid workspace size, revision or navigation order.";
    QSet<QString> ids, keys;
    static const QRegularExpression digest("^[a-f0-9]{64}$");
    for (const auto &v : views) {
        if (v.id.isEmpty() || !validText(v.id, 128) || v.backendId.isEmpty() ||
            !validText(v.backendId, 128) || !validText(v.threadId, 128) ||
            v.threadId.startsWith("new-") || !validText(v.title, 512) || !validText(v.cwd) ||
            (!v.cwd.isEmpty() && !QDir::isAbsolutePath(v.cwd)) || !validText(v.summary, 1024) ||
            !validText(v.executionEnvironment, 128) ||
            (!v.sourceIdentity.isEmpty() && !digest.match(v.sourceIdentity).hasMatch()) ||
            (v.threadId.isEmpty() && v.disposition != ViewDisposition::Unresolved) ||
            ids.contains(v.id) || keys.contains(v.key()))
            return "Invalid or duplicate terminal workspace entry.";
        ids.insert(v.id);
        keys.insert(v.key());
    }
    QSet<QString> order;
    for (const auto &id : recent) {
        if (!ids.contains(id) || order.contains(id))
            return "Invalid terminal workspace navigation order.";
        order.insert(id);
    }
    if (!selected.isEmpty() && !ids.contains(selected))
        return "Selected terminal is missing from the workspace.";
    return {};
}
QJsonObject WorkspaceSnapshot::json() const {
    QJsonArray entries;
    for (const auto &v : views)
        entries.append(v.json());
    return {{"version", 1},
            {"revision", revision},
            {"views", entries},
            {"recent", QJsonArray::fromStringList(recent)},
            {"selected", selected}};
}
WorkspaceSnapshot WorkspaceSnapshot::fromJson(const QJsonObject &o) {
    if (o["version"] != 1 || !o["revision"].isDouble() || !o["views"].isArray() ||
        !o["recent"].isArray() || !o["selected"].isString() || o["views"].toArray().size() > 512)
        throw std::runtime_error("Unsupported or malformed terminal workspace record.");
    WorkspaceSnapshot s;
    s.revision = o["revision"].toInteger(-1);
    if (double(s.revision) != o["revision"].toDouble())
        throw std::runtime_error("Invalid workspace revision.");
    for (const auto &entry : o["views"].toArray())
        s.views.append(WorkspaceView::fromJson(entry.toObject()));
    for (const auto &id : o["recent"].toArray()) {
        if (!id.isString())
            throw std::runtime_error("Invalid workspace navigation identity.");
        s.recent.append(id.toString());
    }
    s.selected = o["selected"].toString();
    const auto error = s.validate();
    if (!error.isEmpty())
        throw std::runtime_error(error.toStdString());
    return s;
}
QString workspaceSourceIdentity(const BackendProfile &p) {
    const auto home = p.identity["codexHome"].toString();
    if (home.isEmpty() || !validText(home) || !QDir::isAbsolutePath(home))
        return {};
    QJsonObject identity{{"kind", p.kind},
                         {"socket", p.socketPath},
                         {"codexHome", home},
                         {"sshHost", p.sshHost},
                         {"environment", p.environmentId}};
    if (p.isContainer()) {
        identity["container"] = p.containerId;
        identity["host"] = p.discoveryHost;
        identity["engine"] = p.identity["dockerEngine"];
        identity["dockerContext"] = p.dockerContext;
        identity["runtimeUser"] = p.runtimeUser;
        identity["runtimeHome"] = p.runtimeHome;
    }
    return QString::fromLatin1(
        QCryptographicHash::hash(QJsonDocument(identity).toJson(QJsonDocument::Compact),
                                 QCryptographicHash::Sha256)
            .toHex());
}
} // namespace Archcodex
