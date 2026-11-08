// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
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
    qint64 recency = 0;
    qint64 opened = 0;
    bool known = false;
    bool isActive() const;
    QString stateText() const;
    int attention() const;
    static Session fromThread(const QString &backend, const QJsonObject &thread);
    QJsonObject metadata() const;
};
QString sessionKey(const QString &backend, const QString &thread);
QString projectKey(const QString &backend, const QString &path);
bool validText(const QString &text, int maxLength = 4096);
QStringList terminalArguments(const BackendProfile &profile, const QString &threadId);
} // namespace Archcodex
