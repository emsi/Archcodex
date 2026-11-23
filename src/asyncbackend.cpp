// SPDX-License-Identifier: GPL-3.0-or-later
#include "asyncbackend.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QPointer>
#include <algorithm>
#include <cmath>

namespace Archcodex {
// One GUI scheduler for all backends: at most one page per backend, one slice
// per turn. Round-robin delivery prevents a fast source from monopolizing input.
class SearchDelivery : public QObject {
    QQueue<QPointer<AsyncBackend>> ready;
    bool scheduled = false;
    QElapsedTimer previousTurn;
    qsizetype previousCount = 0;
    double millisecondsPerItem = 0.125, lastGuiCost = 1.0;
    void schedule() {
        if (scheduled || ready.isEmpty())
            return;
        scheduled = true;
        // Leave room for input, PTY I/O and painting. Deferred layout is a
        // per-turn cost: use it to pace turns, not to shrink every batch towards
        // one row (which would repeat that layout thousands of extra times).
        const int delay = int(std::clamp(std::ceil(lastGuiCost), 1.0, 8.0));
        QTimer::singleShot(delay, this, [this, delay] {
            if (previousCount && previousTurn.isValid()) {
                lastGuiCost = std::max(0.0, previousTurn.nsecsElapsed() / 1e6 - delay);
            }
            const auto limit = std::clamp(qsizetype(4.0 / std::max(0.01, millisecondsPerItem)),
                                          qsizetype(1), qsizetype(32));
            previousTurn.start();
            previousCount = 0;
            if (!ready.isEmpty())
                if (auto backend = ready.dequeue())
                    previousCount = backend->deliverSearch(limit);
            if (previousCount)
                millisecondsPerItem = 0.5 * millisecondsPerItem +
                                      0.5 * (previousTurn.nsecsElapsed() / 1e6) / previousCount;
            scheduled = false;
            if (ready.isEmpty())
                previousTurn.invalidate(); // Server wait time is not GUI work.
            schedule();
        });
    }

  public:
    using QObject::QObject;
    static SearchDelivery *instance() {
        static QPointer<SearchDelivery> shared;
        if (!shared)
            shared = new SearchDelivery(QCoreApplication::instance());
        return shared;
    }
    void enqueue(AsyncBackend *backend) {
        if (!ready.contains(backend))
            ready.enqueue(backend);
        schedule();
    }
    void cancel(AsyncBackend *backend) {
        ready.removeAll(backend);
    }
};
class BackendPump : public QObject {
    Q_OBJECT
  public:
    LocalBackend *core;
    QHash<QString, Session> pending;
    QHash<QString, QPair<SessionAttention, quint64>> attention;
    std::shared_ptr<std::atomic<quint64>> epoch;
    quint64 connectionEpoch = 0; // Worker-owned identity of the actual transport.
    bool inFlight = false, scheduled = false;
    BackendPump(const BackendProfile &p, const QString &bridge, AsyncBackend *target,
                std::shared_ptr<std::atomic<quint64>> generation, QObject *parent)
        : QObject(parent), core(new LocalBackend(p, bridge, this)), epoch(std::move(generation)) {
        core->setSearchPaced(true);
        connect(core, &Backend::attentionRequested, this, [this](const SessionAttention &event) {
            // Coalesce by thread, like metadata: retain its latest unread
            // occurrence without accumulating one queued signal per event.
            attention[event.session.id] = {event, connectionEpoch};
            schedule();
        });
        connect(core, &Backend::connectionChanged, this, [this](const ConnectionState &state) {
            const bool ready = state.ready();
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
            attention.remove(id);
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
        connect(core, &Backend::searchResults, target,
                [target](quint64 revision, const QList<Session> &items, const QString &error,
                         bool complete) {
                    if (revision != target->searchRevision)
                        return;
                    // Pagination waits for the last slice's acknowledgement, so
                    // another page of this generation cannot overwrite this one.
                    Q_ASSERT(!target->searchPage);
                    target->searchPage = AsyncBackend::SearchPage{items, error, 0, complete};
                    target->searchDelivery->enqueue(target);
                });
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
                const auto event = attention.begin().value();
                attention.erase(attention.begin());
                if (event.second == eventEpoch)
                    events << event.first;
            }
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
    searchDelivery = SearchDelivery::instance();
    qRegisterMetaType<ConnectionState>();
    qRegisterMetaType<Session>();
    qRegisterMetaType<QList<Session>>();
    qRegisterMetaType<SessionAttention>();
    qRegisterMetaType<QList<SessionAttention>>();
    connect(this, &Backend::connectionChanged, this, [this](const ConnectionState &state) {
        const bool ready = state.ready();
        connected = ready;
        if (!ready)
            cancelSearchDelivery();
    });
    state->pump = new BackendPump(profile, bridge, this, attentionEpoch, nullptr);
    state->pump->moveToThread(worker->context->thread());
}
AsyncBackend::~AsyncBackend() {
    cancelSearchDelivery();
    worker->post([s = state] {
        delete s->pump;
        s->pump = nullptr;
    });
}
void AsyncBackend::connectServer() {
    cancelSearchDelivery();
    ++*attentionEpoch;
    connected = false;
    worker->post([s = state] { s->pump->core->connectServer(); });
}
void AsyncBackend::disconnectServer() {
    cancelSearchDelivery();
    ++*attentionEpoch;
    connected = false;
    worker->post([s = state] { s->pump->core->disconnectServer(); });
}
void AsyncBackend::loadMore() {
    worker->post([s = state] { s->pump->core->loadMore(); });
}
void AsyncBackend::search(const QString &q, quint64 token) {
    cancelSearchDelivery();
    searchToken = token;
    worker->post([s = state, q, revision = searchRevision] { s->pump->core->search(q, revision); });
}
void AsyncBackend::cancelSearchDelivery() {
    ++searchRevision;
    searchPage.reset();
    searchDelivery->cancel(this);
}
qsizetype AsyncBackend::deliverSearch(qsizetype limit) {
    if (!searchPage)
        return 0;
    const auto revision = searchRevision;
    const auto items = searchPage->items.mid(searchPage->offset, limit);
    searchPage->offset += items.size();
    const bool last = searchPage->offset == searchPage->items.size();
    const auto error = last ? searchPage->error : QString();
    const bool complete = last && searchPage->complete;
    if (last)
        searchPage.reset();
    QPointer<AsyncBackend> guard(this);
    emit searchResults(searchToken, items, error, complete);
    // Consumers may replace/cancel the query or remove this backend synchronously.
    if (!guard || revision != searchRevision)
        return items.size();
    if (last)
        worker->post([s = state, revision] { s->pump->core->advanceSearch(revision); });
    else
        searchDelivery->enqueue(this);
    return items.size();
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
