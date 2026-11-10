// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "backend.h"
#include "workspace.h"
#include <QPointer>

namespace Archcodex {
// One event-driven preflight path for explicit opens, retries and restoration.
// Owns no transports or widgets. Idle/offline entries consume no request slots.
class OpeningCoordinator : public QObject {
    Q_OBJECT
  public:
    using QObject::QObject;
    void setSource(const BackendProfile &profile, Backend *backend);
    void updateProfile(const BackendProfile &profile);
    void removeSource(const QString &id);
    void request(const WorkspaceView &view, bool automatic);
    void cancel(const QString &viewId);
    void stop();
    int pendingCount() const {
        return attempts.size();
    }
  signals:
    void stateChanged(const QString &viewId, const QString &text, bool needsAttention);
    void launchRequested(const QString &viewId, const Archcodex::BackendProfile &profile,
                         const Archcodex::Session &session);
    void progressChanged();

  private:
    enum class Phase { Waiting, Verifying, Evidence, Starting };
    struct Source {
        BackendProfile profile;
        QPointer<Backend> backend;
        quint64 generation = 0;
        bool ready = false, identified = false, loadedKnown = false, inventoryFailed = false;
    };
    struct Attempt {
        WorkspaceView view;
        bool automatic = true;
        Phase phase = Phase::Waiting;
        quint64 token = 0, generation = 0;
        Session verified;
    };
    QHash<QString, Source> sources;
    QHash<QString, Attempt> attempts;
    struct InFlight {
        QString source, host;
    };
    QHash<quint64, InFlight> inFlight;
    QStringList order;
    quint64 nextToken = 0, nextGeneration = 0;
    bool scheduled = false, stopped = false;
    void schedule();
    void pump();
    void unavailable(const QString &source);
    void fail(const QString &viewId, const QString &message);
    void verified(const QString &backend, const QString &thread, const QString &error,
                  quint64 token, const Session &session);
};
} // namespace Archcodex
