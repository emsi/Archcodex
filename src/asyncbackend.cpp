// SPDX-License-Identifier: GPL-3.0-or-later
#include "asyncbackend.h"

namespace Archcodex {
class BackendPump : public QObject {
    Q_OBJECT
  public:
    LocalBackend *core;
    QHash<QString, Session> pending;
    QQueue<QPair<SessionAttention, quint64>> attention;
    std::shared_ptr<std::atomic<quint64>> epoch;
    quint64 connectionEpoch = 0; // Worker-owned identity of the actual transport.
    bool inFlight = false, scheduled = false;
    bool attentionOverflow = false;
    BackendPump(const BackendProfile &p, const QString &bridge, AsyncBackend *target,
                std::shared_ptr<std::atomic<quint64>> generation, QObject *parent)
        : QObject(parent), core(new LocalBackend(p, bridge, this)), epoch(std::move(generation)) {
        connect(core, &Backend::attentionRequested, this, [this](const SessionAttention &event) {
            // A stalled GUI cannot create an unbounded Qt queued-signal backlog.
            if (attention.size() == 256) {
                attention.dequeue();
                if (!attentionOverflow)
                    qWarning("Session notification backlog exceeded 256; oldest event discarded.");
                attentionOverflow = true;
            }
            attention.enqueue({event, connectionEpoch});
            schedule();
        });
        connect(core, &Backend::connectionChanged, this, [this](const QString &, bool ready) {
            if (!ready) {
                connectionEpoch = ++*epoch;
                attention.clear();
            }
        });
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
        connect(core, &Backend::sessionStarted, this,
                [this](quint64 token, const Session &session, const QString &error) {
                    if (pending.contains(session.id))
                        emit selected(pending.take(session.id));
                    emit started(token, session, error);
                });
        connect(this, &BackendPump::started, target, &Backend::sessionStarted);
        connect(this, &BackendPump::batch, target,
                [target, generation = epoch](const QList<Session> &items,
                                             const QList<SessionAttention> &events,
                                             quint64 eventEpoch) {
                    emit target->sessionsChanged(items);
                    for (const auto &event : events)
                        if (eventEpoch == generation->load() && target->ready())
                            emit target->attentionRequested(event);
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
        if (inFlight || scheduled || (pending.isEmpty() && attention.isEmpty()))
            return;
        scheduled = true;
        QTimer::singleShot(0, this, [this] {
            scheduled = false;
            if (inFlight || (pending.isEmpty() && attention.isEmpty()))
                return;
            QList<Session> items;
            while (!pending.isEmpty() && items.size() < 64) {
                auto it = pending.begin();
                items << it.value();
                pending.erase(it);
            }
            inFlight = true;
            const auto eventEpoch = connectionEpoch;
            QList<SessionAttention> events;
            while (!attention.isEmpty() && events.size() < 32) {
                const auto event = attention.dequeue();
                if (event.second == eventEpoch)
                    events << event.first;
            }
            if (attention.isEmpty())
                attentionOverflow = false;
            emit batch(items, events, eventEpoch);
        });
    }
  signals:
    void batch(const QList<Archcodex::Session> &items,
               const QList<Archcodex::SessionAttention> &events, quint64 epoch);
    void selected(const Archcodex::Session &session);
    void removed(const QString &id);
    void verified(const QString &id, const QString &error, quint64 token,
                  const Archcodex::Session &session);
    void forked(const QString &id, const Archcodex::Session &session, const QString &error);
    void started(quint64 token, const Archcodex::Session &session, const QString &error);
};
AsyncBackend::AsyncBackend(BackendProfile p, const QString &bridge, QObject *parent)
    : Backend(std::move(p), parent), state(std::make_shared<State>()),
      worker(Worker::forHost("backend:" + profile.sshHost)),
      attentionEpoch(std::make_shared<std::atomic<quint64>>(0)) {
    qRegisterMetaType<Session>();
    qRegisterMetaType<QList<Session>>();
    qRegisterMetaType<SessionAttention>();
    qRegisterMetaType<QList<SessionAttention>>();
    connect(this, &Backend::connectionChanged, this,
            [this](const QString &, bool ready) { connected = ready; });
    state->pump = new BackendPump(profile, bridge, this, attentionEpoch, nullptr);
    state->pump->moveToThread(worker->context->thread());
}
AsyncBackend::~AsyncBackend() {
    worker->post([s = state] {
        delete s->pump;
        s->pump = nullptr;
    });
}
void AsyncBackend::connectServer() {
    ++*attentionEpoch;
    connected = false;
    worker->post([s = state] { s->pump->core->connectServer(); });
}
void AsyncBackend::disconnectServer() {
    ++*attentionEpoch;
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
void AsyncBackend::startSession(const QString &cwd, quint64 token) {
    worker->post([s = state, cwd, token] { s->pump->core->startSession(cwd, token); });
}
} // namespace Archcodex
#include "asyncbackend.moc"
