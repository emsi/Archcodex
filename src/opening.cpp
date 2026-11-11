// SPDX-License-Identifier: GPL-3.0-or-later
#include "opening.h"
#include "notificationlog.h"
#include <QTimer>
#include <utility>

namespace Archcodex {
void OpeningCoordinator::setSource(const BackendProfile &profile, Backend *backend) {
    removeSource(profile.id);
    Source source;
    source.profile = profile;
    source.backend = backend;
    source.generation = ++nextGeneration;
    source.ready = backend->ready();
    source.identified = source.ready && !workspaceSourceIdentity(profile).isEmpty();
    sources[profile.id] = source;
    const auto id = profile.id;
    connect(backend, &Backend::identityReceived, this, [this, id](const QJsonObject &identity) {
        auto &s = sources[id];
        s.identified =
            identity["codexHome"].isString() && !identity["codexHome"].toString().isEmpty();
        s.profile.identity["codexHome"] = identity["codexHome"];
        schedule();
    });
    connect(backend, &Backend::connectionChanged, this, [this, id](const QString &, bool ready) {
        if (!ready)
            unavailable(id);
        else {
            sources[id].ready = true;
            schedule();
        }
    });
    connect(backend, &Backend::sessionVerified, this,
            [this, id](const QString &thread, const QString &error, quint64 token,
                       const Session &s) { verified(id, thread, error, token, s); });
    connect(backend, &Backend::loadedInventory, this, [this, id](const QSet<QString> &loaded) {
        auto &source = sources[id];
        source.loadedKnown = true;
        const auto ids = order;
        for (const auto &viewId : ids) {
            auto it = attempts.find(viewId);
            if (it == attempts.end() || it->view.backendId != id || it->phase != Phase::Evidence)
                continue;
            if (loaded.contains(it->view.threadId))
                it->phase = Phase::Waiting;
            else
                fail(viewId, "Shared history does not establish execution in this container. "
                             "Open the conversation in its original environment, then retry.");
        }
        schedule();
    });
    connect(backend, &Backend::loadedInventoryFailed, this, [this, id] {
        sources[id].inventoryFailed = true;
        const auto ids = order;
        for (const auto &view : ids)
            if (attempts.contains(view) && attempts[view].view.backendId == id &&
                attempts[view].phase == Phase::Evidence)
                fail(view, "Container execution inventory is unavailable. Retry its connection in "
                           "Connections.");
    });
    connect(backend, &Backend::sessionChanged, this, [this, id](const Session &session) {
        if (!session.executionObserved)
            return;
        for (auto &a : attempts)
            if (a.view.backendId == id && a.view.threadId == session.id &&
                a.phase == Phase::Evidence)
                a.phase = Phase::Waiting;
        schedule();
    });
    connect(backend, &QObject::destroyed, this, [this, id] { unavailable(id); });
    schedule();
}
void OpeningCoordinator::updateProfile(const BackendProfile &profile) {
    if (!sources.contains(profile.id))
        return;
    auto &source = sources[profile.id];
    if (workspaceSourceIdentity(source.profile) != workspaceSourceIdentity(profile)) {
        // Keep transport state, but recheck all pending targets against the new identity.
        source.generation = ++nextGeneration;
        for (auto &a : attempts)
            if (a.view.backendId == profile.id) {
                a.phase = Phase::Waiting;
                a.token = 0;
            }
    }
    source.profile = profile;
    schedule();
}
void OpeningCoordinator::unavailable(const QString &id) {
    auto it = sources.find(id);
    if (it == sources.end())
        return;
    it->ready = it->identified = it->loadedKnown = false;
    it->generation = ++nextGeneration;
    it->inventoryFailed = false;
    inFlight.removeIf([&](const auto &entry) { return entry.value().source == id; });
    const auto ids = order;
    for (const auto &view : ids) {
        auto a = attempts.find(view);
        if (a != attempts.end() && a->view.backendId == id) {
            a->phase = Phase::Waiting;
            a->token = 0;
            emit stateChanged(
                view, "Waiting for connection. Connect or retry this source in Connections.",
                false);
        }
    }
    schedule();
}
void OpeningCoordinator::removeSource(const QString &id) {
    if (!sources.contains(id))
        return;
    if (sources[id].backend)
        sources[id].backend->disconnect(this);
    unavailable(id);
    sources.remove(id);
}
void OpeningCoordinator::request(const WorkspaceView &view, bool automatic) {
    if (stopped || view.id.isEmpty())
        return;
    if (attempts.contains(view.id)) {
        if (!automatic) {
            attempts[view.id].automatic = false;
            order.removeAll(view.id);
            order.prepend(view.id);
        }
        schedule();
        return;
    }
    if (attempts.size() >= 512) {
        emit stateChanged(view.id,
                          "Too many pending terminal opens. Close a pending view and retry.", true);
        return;
    }
    Attempt attempt;
    attempt.view = view;
    attempt.automatic = automatic;
    attempts[view.id] = attempt;
    if (automatic)
        order.append(view.id);
    else
        order.prepend(view.id);
    emit stateChanged(
        view.id, "Waiting for connection. Connect or retry this source in Connections.", false);
    emit progressChanged();
    schedule();
}
void OpeningCoordinator::cancel(const QString &id) {
    if (attempts.remove(id)) {
        order.removeAll(id);
        emit progressChanged();
        schedule();
    }
}
void OpeningCoordinator::stop() {
    stopped = true;
    attempts.clear();
    order.clear();
    inFlight.clear();
}
void OpeningCoordinator::fail(const QString &id, const QString &text) {
    cancel(id);
    emit stateChanged(id, text, true);
}
void OpeningCoordinator::schedule() {
    if (scheduled || stopped)
        return;
    scheduled = true;
    QTimer::singleShot(0, this, [this] {
        scheduled = false;
        if (!stopped)
            pump();
    });
}
void OpeningCoordinator::pump() {
    // Incremental widget creation: at most one launch per event-loop iteration.
    const auto starting = order;
    for (const auto &id : starting) {
        auto &a = attempts[id];
        const auto source = sources.value(a.view.backendId);
        if (a.phase == Phase::Starting && source.ready && a.generation == source.generation) {
            const auto session = a.verified;
            cancel(id);
            emit launchRequested(id, source.profile, session);
            schedule();
            return;
        }
    }
    int total = 0, remote = 0;
    QHash<QString, int> hosts;
    for (const auto &flight : std::as_const(inFlight)) {
        ++total;
        ++hosts[flight.host];
        remote += !flight.host.isEmpty();
    }
    // Rotate dispatched entries behind the rest, retaining explicit-open priority.
    const auto candidates = order;
    for (const auto &id : candidates) {
        if (total >= 4)
            break;
        auto it = attempts.find(id);
        if (it == attempts.end() || it->phase != Phase::Waiting)
            continue;
        const auto source = sources.value(it->view.backendId);
        if (!source.ready || !source.backend)
            continue;
        const auto host = source.profile.sshHost;
        if (hosts[host] >= 2 || (!host.isEmpty() && remote >= 3))
            continue; // Keep capacity for local work even while discovery is pending.
        const auto identity = workspaceSourceIdentity(source.profile);
        if (!it->view.sourceIdentity.isEmpty() && identity != it->view.sourceIdentity) {
            fail(id, "The server or execution environment changed. Close this view and open "
                     "the conversation from its freshly discovered source.");
            continue;
        }
        if (it->automatic && (!source.identified || it->view.sourceIdentity.isEmpty())) {
            fail(id,
                 "The saved source identity cannot be verified. Reconnect this view explicitly.");
            continue;
        }
        if (it->view.threadId.isEmpty()) {
            fail(id, "This new-session terminal has no confirmed conversation ID. Open its saved "
                     "conversation from Projects; this launcher will not be replayed.");
            continue;
        }
        it->phase = Phase::Verifying;
        it->generation = source.generation;
        it->token = ++nextToken;
        const auto token = it->token;
        const auto thread = it->view.threadId;
        ++total;
        ++hosts[host];
        remote += !host.isEmpty();
        order.removeAll(id);
        order.append(id);
        emit stateChanged(id, "Checking the saved conversation…", false);
        // A slot can be closed synchronously from stateChanged by a UI/test.
        if (attempts.contains(id) && attempts[id].token == token && source.backend) {
            inFlight[token] = {source.profile.id, host};
            source.backend->verifySession(thread, token);
        }
    }
}
void OpeningCoordinator::verified(const QString &backend, const QString &thread,
                                  const QString &error, quint64 token, const Session &session) {
    inFlight.remove(token);
    schedule();
    auto found = attempts.end();
    for (auto it = attempts.begin(); it != attempts.end(); ++it)
        if (token && it->token == token && it->phase == Phase::Verifying &&
            it->view.backendId == backend && it->view.threadId == thread) {
            found = it;
            break;
        }
    if (found == attempts.end())
        return;
    const auto source = sources.value(backend);
    if (!source.ready || found->generation != source.generation)
        return;
    const auto id = found->view.id;
    if (!error.isEmpty()) {
        fail(id, "Could not reopen this conversation: " + error.simplified().left(700));
        return;
    }
    qCDebug(notificationLog) << "verified open" << backend << thread << "saved"
                             << session.isSaved();
    if (session.id != thread || session.backendId != backend || !session.isSaved()) {
        fail(id, "This conversation has no available saved history. It was not replaced with a new "
                 "session.");
        return;
    }
    auto verified = session;
    if (source.profile.isContainer()) {
        const auto environment = source.profile.environmentId;
        verified.executionObserved =
            (session.executionObserved && session.executionEnvironment == environment) ||
            (found->view.executionObserved && found->view.executionEnvironment == environment);
        verified.executionEnvironment = environment;
        if (!verified.executionObserved) {
            if (source.loadedKnown || source.inventoryFailed)
                fail(id, "Shared history does not establish execution in this container. "
                         "Open it in its original environment, then retry.");
            else {
                found->phase = Phase::Evidence;
                found->token = 0;
                emit stateChanged(id, "Waiting for this container's execution inventory…", false);
                schedule();
            }
            return;
        }
    }
    found->verified = verified;
    found->phase = Phase::Starting;
    emit stateChanged(id, "Starting terminal client…", false);
    schedule();
}
} // namespace Archcodex
