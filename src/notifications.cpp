// SPDX-License-Identifier: GPL-3.0-or-later
#include "notifications.h"
#include "notificationlog.h"
#include <KNotification>
#include <KWindowInfo>
#include <KWindowSystem>
#include <KX11Extras>
#include <QDBusArgument>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QFile>
#include <QUuid>
#include <QWidget>
#include <utility>

namespace Archcodex {
static constexpr auto service = "org.freedesktop.portal.Desktop";
static constexpr auto path = "/org/freedesktop/portal/desktop";
static constexpr auto interface = "org.freedesktop.portal.Notification";
Notifications::Notifications(QObject *parent)
    : Notifications(QFile::exists("/.flatpak-info"), QDBusConnection::sessionBus(), parent) {}
Notifications::Notifications(bool usePortal, const QDBusConnection &connection, QObject *parent)
    : QObject(parent), portal(usePortal), bus(connection) {
    if (portal) {
        qDBusRegisterMetaType<QList<QVariantMap>>();
        bus.connect(service, path, interface, "ActionInvoked", this,
                    SLOT(portalAction(QString, QString, QVariantList)));
    }
}
Notifications::~Notifications() {
    clear();
}
void Notifications::setEnabled(bool value) {
    enabled = value;
    if (!enabled)
        clear();
}
bool Notifications::inScope(const Session &session, const BackendProfile &profile) const {
    return !profile.isContainer() || !myCodexWorkdirOnly ||
           session.inWorkdir(profile.identity["containerWorkdir"].toString());
}
void Notifications::setMyCodexWorkdirOnly(bool value) {
    myCodexWorkdirOnly = value;
    const auto ids = order;
    for (const auto &id : ids) {
        const auto &target = targets[id];
        if (!inScope(target.session, target.profile))
            remove(id);
    }
}
void Notifications::withdrawBackend(const QString &backend) {
    const auto ids = order;
    for (const auto &id : ids)
        if (targets[id].profile.id == backend)
            remove(id);
}
void Notifications::withdrawSession(const QString &backend, const QString &thread) {
    const auto ids = order;
    for (const auto &id : ids)
        if (targets[id].profile.id == backend && targets[id].session.id == thread)
            remove(id);
}
void Notifications::setViewedSession(const QString &backend, const QString &thread) {
    viewedSession = backend.isEmpty() || thread.isEmpty() ? QString() : sessionKey(backend, thread);
    if (!viewedSession.isEmpty())
        withdrawSession(backend, thread);
}
void Notifications::clear() {
    const auto ids = order;
    for (const auto &id : ids)
        remove(id);
}
void Notifications::remove(const QString &id) {
    if (!targets.contains(id))
        return;
    const auto target = targets.take(id);
    qCDebug(notificationLog) << "withdraw popup" << id << target.profile.id << target.session.id;
    order.removeAll(id);
    if (portal) {
        auto message =
            QDBusMessage::createMethodCall(service, path, interface, "RemoveNotification");
        message << id;
        bus.asyncCall(message);
    } else if (target.native)
        target.native->close();
}
void Notifications::activate(const QString &id, const QString &token) {
    if (!targets.contains(id))
        return;
    const auto target = targets[id];
    remove(id);
    qCDebug(notificationLog) << "activate popup" << id << target.profile.id << target.session.id;
    emit openRequested(target.profile.id, target.session.id, token);
}
void Notifications::portalAction(const QString &id, const QString &action,
                                 const QVariantList &parameters) {
    if (action != "default" && action != "open")
        return;
    // No action target is sent: platform-data is the first parameter in portal v2.
    // v1 supplies no token. Never derive session identity from action parameters.
    QString token;
    if (!parameters.isEmpty()) {
        QVariant data = parameters.first();
        if (data.metaType() == QMetaType::fromType<QDBusVariant>())
            data = qvariant_cast<QDBusVariant>(data).variant();
        const auto map = data.metaType() == QMetaType::fromType<QDBusArgument>()
                             ? qdbus_cast<QVariantMap>(data)
                             : data.toMap();
        token = map.value("activation-token").toString();
        if (token.size() > 4096 || token.contains(QChar::Null))
            token.clear();
    }
    activate(id, token);
}
void Notifications::show(const SessionAttention &event, const BackendProfile &profile) {
    const auto &session = event.session;
    if (!enabled || session.id.isEmpty() || session.backendId != profile.id ||
        (portal && pendingDeliveries >= 128))
        return;
    if (viewedSession == sessionKey(profile.id, session.id)) {
        qCDebug(notificationLog) << "suppress viewed terminal" << profile.id << session.id;
        return;
    }
    if (!session.isSaved() || !inScope(session, profile)) {
        qCDebug(notificationLog) << "suppress popup" << profile.id << session.id << "saved"
                                 << session.isSaved() << "in scope" << inScope(session, profile);
        withdrawSession(profile.id, session.id);
        return;
    }
    // Update in place: withdrawing/recreating makes bursts flicker and removes
    // actionable notifications from the desktop history before the user can click.
    QString id;
    for (const auto &candidate : std::as_const(order))
        if (targets[candidate].profile.id == profile.id &&
            targets[candidate].session.id == session.id) {
            id = candidate;
            break;
        }
    if (id.isEmpty()) {
        if (order.size() >= 128)
            remove(order.first());
        id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    }
    auto &target = targets[id];
    target.session = session;
    target.profile = profile;
    target.delivery = ++nextDelivery;
    order.removeAll(id);
    order << id;
    qCDebug(notificationLog) << "show/update popup" << id << profile.id << session.id << "container"
                             << profile.containerId << "endpoint" << profile.socketPath;
    const bool question = event.reason == SessionAttention::Reason::Question;
    const QString title = question ? "Codex needs your input" : "Codex is waiting";
    const QString name = session.title.isEmpty() ? "Session " + session.id.left(12) : session.title;
    const QString body =
        name.simplified().left(200) + "\n" + serverLabel(profile).simplified().left(240);
    if (portal) {
        const QList<QVariantMap> buttons{{{"label", "Open session"}, {"action", "open"}}};
        const QVariantMap properties{{"title", title},
                                     {"body", body},
                                     {"icon", "io.github.emsi.Archcodex"},
                                     {"priority", "normal"},
                                     {"default-action", "default"},
                                     {"buttons", QVariant::fromValue(buttons)}};
        auto message = QDBusMessage::createMethodCall(service, path, interface, "AddNotification");
        message << id << properties;
        auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(message, 5000), this);
        ++pendingDeliveries;
        connect(watcher, &QDBusPendingCallWatcher::finished, this,
                [this, watcher, id, delivery = target.delivery] {
                    --pendingDeliveries;
                    const QDBusPendingReply<> reply = *watcher;
                    if (reply.isError()) {
                        qWarning() << "Desktop notification failed:" << reply.error().name();
                        // An older failed send must not erase a newer successful update.
                        if (targets.contains(id) && targets[id].delivery == delivery)
                            remove(id);
                    }
                    watcher->deleteLater();
                });
        return;
    }
    if (target.native) {
        // KNotification setters schedule an update to the existing notification.
        target.native->setTitle(title);
        target.native->setText(body.toHtmlEscaped().replace('\n', "<br/>"));
        return;
    }
    auto *notification = new KNotification(question ? "SessionQuestion" : "SessionWaiting");
    notification->setParent(this);
    notification->setComponentName("archcodex");
    notification->setTitle(title);
    notification->setText(body.toHtmlEscaped().replace('\n', "<br/>"));
    notification->setIconName("io.github.emsi.Archcodex");
    target.native = notification;
    const auto open = [this, id, notification] {
        activate(id, notification->xdgActivationToken());
    };
    connect(notification->addDefaultAction("Open session"), &KNotificationAction::activated, this,
            open);
    connect(notification->addAction("Open session"), &KNotificationAction::activated, this, open);
    connect(notification, &KNotification::closed, this, [this, id] {
        targets.remove(id);
        order.removeAll(id);
    });
    notification->sendEvent();
}
void Notifications::presentWindow(QWidget *window, const QString &token) {
    if (KWindowSystem::isPlatformX11()) {
        const KWindowInfo info(window->winId(), NET::WMDesktop);
        if (info.valid() && !info.onAllDesktops() && info.desktop() > 0 &&
            info.desktop() != KX11Extras::currentDesktop())
            KX11Extras::setCurrentDesktop(info.desktop());
    }
    // showNormal() would discard maximized/full-screen state.
    window->setWindowState(window->windowState() & ~Qt::WindowMinimized);
    window->show();
    if (KWindowSystem::isPlatformX11()) {
        const auto marker = token.lastIndexOf("_TIME");
        const auto timestamp = marker < 0 ? 0 : token.mid(marker + 5).toULong();
        KX11Extras::activateWindow(window->winId(), timestamp);
    } else if (KWindowSystem::isPlatformWayland()) {
        KWindowSystem::setCurrentXdgActivationToken(token);
        KWindowSystem::activateWindow(window->windowHandle());
    } else
        window->activateWindow();
}
} // namespace Archcodex
