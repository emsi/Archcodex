// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "types.h"
#include <QList>

namespace Archcodex {
enum class ViewDisposition { Automatic, Disconnected, Unresolved };
// Workspace intent is independent of the session cache and transport lifetime.
// The thread is the launch target, not a claim about later in-TUI navigation.
struct WorkspaceView {
    QString id, backendId, threadId, title, cwd;
    QString sourceIdentity, executionEnvironment;
    bool executionObserved = false;
    ViewDisposition disposition = ViewDisposition::Automatic;
    QString summary;
    QString key() const;
    QJsonObject json() const;
    static WorkspaceView fromJson(const QJsonObject &object);
};
struct WorkspaceSnapshot {
    qint64 revision = 0;
    QList<WorkspaceView> views;
    QStringList recent;
    QString selected;
    QString validate() const;
    QJsonObject json() const;
    static WorkspaceSnapshot fromJson(const QJsonObject &object);
};
// Logical execution identity, excluding display names and ephemeral relay paths.
// An initialized server must supply CODEX_HOME before automatic restoration.
QString workspaceSourceIdentity(const BackendProfile &profile);
} // namespace Archcodex
Q_DECLARE_METATYPE(Archcodex::WorkspaceSnapshot)
