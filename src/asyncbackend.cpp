// SPDX-License-Identifier: GPL-3.0-or-later
#include "asyncbackend.h"

namespace Archcodex {
class BackendPump : public QObject {
    Q_OBJECT
  public:
    LocalBackend *core;
    QHash<QString, Session> pending;
    bool inFlight = false, scheduled = false;
    BackendPump(const BackendProfile &p, const QString &bridge, AsyncBackend *target,
                QObject *parent)
        : QObject(parent), core(new LocalBackend(p, bridge, this)) {
        connect(core, &Backend::sessionChanged, this, [this](const Session &s) {
            pending[s.id] = s;
            if (pending.size() > 16384) {
                pending.clear();
                QTimer::singleShot(0, core, &Backend::disconnectServer);
                return;
            }
            schedule();
        });
        connect(core, &Backend::sessionRemoved, this, [this](const QString &id) {
            pending.remove(id);
            emit removed(id);
        });
        connect(this, &BackendPump::removed, target, &Backend::sessionRemoved);
        connect(
            core, &Backend::sessionVerified, this,
            [this](const QString &id, const QString &error, quint64 token, const Session &session) {
                // The selected record must precede its preflight result, even during a bulk
                // import.
                if (pending.contains(id))
                    emit selected(pending.take(id));
                emit verified(id, error, token, session);
            });
        connect(this, &BackendPump::selected, target, &Backend::sessionChanged);
        connect(this, &BackendPump::verified, target, &Backend::sessionVerified);
        connect(core, &Backend::sessionForked, this,
                [this](const QString &id, const Session &session, const QString &error) {
                    // Deliver metadata before the result, even during a history import.
                    if (pending.contains(session.id))
                        emit selected(pending.take(session.id));
                    emit forked(id, session, error);
                });
        connect(this, &BackendPump::forked, target, &Backend::sessionForked);
        connect(this, &BackendPump::batch, target, [target](const QList<Session> &items) {
            emit target->sessionsChanged(items);
            emit target->batchAccepted();
        });
        connect(target, &AsyncBackend::batchAccepted, this, [this] {
            inFlight = false;
            schedule();
        });
        connect(core, &Backend::connectionChanged, target, &Backend::connectionChanged);
        connect(core, &Backend::loadedInventory, target, &Backend::loadedInventory);
        connect(core, &Backend::loadedInventoryFailed, target, &Backend::loadedInventoryFailed);
        connect(core, &Backend::identityReceived, target, &Backend::identityReceived);
        connect(core, &Backend::moreAvailable, target, &Backend::moreAvailable);
        connect(core, &Backend::searchResults, target, &Backend::searchResults);
    }
    void schedule() {
        if (inFlight || scheduled || pending.isEmpty())
            return;
        scheduled = true;
        QTimer::singleShot(0, this, [this] {
            scheduled = false;
            if (inFlight || pending.isEmpty())
                return;
            QList<Session> items;
            while (!pending.isEmpty() && items.size() < 64) {
                auto it = pending.begin();
                items << it.value();
                pending.erase(it);
            }
            inFlight = true;
            emit batch(items);
        });
    }
  signals:
    void batch(const QList<Archcodex::Session> &items);
    void selected(const Archcodex::Session &session);
    void removed(const QString &id);
    void verified(const QString &id, const QString &error, quint64 token,
                  const Archcodex::Session &session);
    void forked(const QString &id, const Archcodex::Session &session, const QString &error);
};
AsyncBackend::AsyncBackend(BackendProfile p, const QString &bridge, QObject *parent)
    : Backend(std::move(p), parent), state(std::make_shared<State>()),
      worker(Worker::forHost("backend:" + profile.sshHost)) {
    qRegisterMetaType<Session>();
    qRegisterMetaType<QList<Session>>();
    connect(this, &Backend::connectionChanged, this,
            [this](const QString &, bool ready) { connected = ready; });
    state->pump = new BackendPump(profile, bridge, this, nullptr);
    state->pump->moveToThread(worker->context->thread());
}
AsyncBackend::~AsyncBackend() {
    worker->post([s = state] {
        delete s->pump;
        s->pump = nullptr;
    });
}
void AsyncBackend::connectServer() {
    worker->post([s = state] { s->pump->core->connectServer(); });
}
void AsyncBackend::disconnectServer() {
    connected = false;
    worker->post([s = state] { s->pump->core->disconnectServer(); });
}
void AsyncBackend::loadMore() {
    worker->post([s = state] { s->pump->core->loadMore(); });
}
void AsyncBackend::search(const QString &q, quint64 token) {
    worker->post([s = state, q, token] { s->pump->core->search(q, token); });
}
void AsyncBackend::verifySession(const QString &id, quint64 token) {
    worker->post([s = state, id, token] { s->pump->core->verifySession(id, token); });
}
void AsyncBackend::forkSession(const QString &id, const QString &priorExecutionEnvironment) {
    worker->post([s = state, id, priorExecutionEnvironment] {
        s->pump->core->forkSession(id, priorExecutionEnvironment);
    });
}
} // namespace Archcodex
#include "asyncbackend.moc"
