// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace Archcodex {
struct BackendProfile {
    QString id;
    QString name;
    QString kind = QStringLiteral("local-unix");
    QString socketPath; // Empty selects the host Codex default, not a guessed sandbox path.
    QString executable = QStringLiteral("codex");
    bool autoConnect = true;
    QString environmentId; // Shared only when discovery establishes a common host namespace.
    bool discovered = false;
    QJsonObject identity; // Non-secret server/discovery metadata; never configuration contents.
    QString sshHost, dockerContext, containerId, containerEpoch, runtimeUser, runtimeHome;
    QString discoveryHost;
    QString relaySocket; // Live host endpoint; never persisted or restored from saved metadata.
    bool isContainer() const {
        return kind == QStringLiteral("mycodex");
    }
    QString validate() const;
    QString endpoint() const {
        return QStringLiteral("unix://") + socketPath;
    }
    QJsonObject json() const;
    static BackendProfile fromJson(const QJsonObject &object);
};
struct Project {
    QString backendId;
    QString path;
    QString name;
};
struct Session {
    QString backendId;
    QString id;
    QString cwd;
    QString title;
    QJsonObject status;
    QString rolloutPath;
    QString historyMode;
    bool ephemeral = false;
    bool persistenceKnown = false;
    bool unavailable = false;
    bool terminalOpen = false;  // UI lifetime; deliberately not persisted.
    bool clientPending = false; // A remembered view has not started its client yet.
    bool contextRequired = false;
    bool executionObserved = false; // Evidence from this backend's loaded inventory/events only.
    QString executionEnvironment;   // Bind cached evidence to the container/mount identity.
    QString clientIssue;
    qint64 recency = 0;
    qint64 opened = 0;
    bool known = false;
    bool isActive() const;
    bool isRunning() const;
    bool isWaiting() const;
    bool isSaved() const;
    bool inWorkdir(const QString &workdir) const;
    QString stateText() const;
    int attention() const;
    static Session fromThread(const QString &backend, const QJsonObject &thread);
    QJsonObject metadata() const;
};
// A live occurrence, separate from the replaceable session inventory. Never persisted.
struct SessionAttention {
    enum class Reason { Waiting, Question };
    Session session;
    Reason reason = Reason::Waiting;
};
QString sessionKey(const QString &backend, const QString &thread);
QString projectKey(const QString &backend, const QString &path);
bool validText(const QString &text, int maxLength = 4096);
QStringList terminalArguments(const BackendProfile &profile, const QString &threadId);
QStringList clientArguments(const BackendProfile &profile, const QString &threadId,
                            const QString &cwd = {}, const QString &localExecutable = {});
QString detectedLocalCodex(const QJsonArray &endpoints);
QString serverLabel(const BackendProfile &profile);
QString serverTreeLabel(const BackendProfile &profile);
QString serverDetails(const BackendProfile &profile);
} // namespace Archcodex

Q_DECLARE_METATYPE(Archcodex::Session)
Q_DECLARE_METATYPE(Archcodex::SessionAttention)
