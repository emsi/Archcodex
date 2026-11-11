// SPDX-License-Identifier: GPL-3.0-or-later
#include "notifications.h"
#include <KWindowInfo>
#include <KX11Extras>
#include <QDBusArgument>
#include <QDBusContext>
#include <QDBusMetaType>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDir>
#include <QFile>
#include <QSocketNotifier>
#include <QTemporaryDir>
#include <QWidget>
#include <QtTest>
#include <memory>
#include <xcb/xcb.h>
using namespace Archcodex;

class Portal : public QObject, protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.portal.Notification")
  public:
    QList<QPair<QString, QVariantMap>> added;
    QStringList removed;
    bool delayNext = false;
    QList<QDBusMessage> delayed;
  public slots:
    void AddNotification(const QString &id, const QVariantMap &properties) {
        added.append({id, properties});
        if (delayNext) {
            delayNext = false;
            setDelayedReply(true);
            delayed << message();
        }
    }
    void RemoveNotification(const QString &id) {
        removed << id;
    }
  signals:
    void ActionInvoked(const QString &id, const QString &action, const QVariantList &parameters);
};
class Desktop : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.Notifications")
  public:
    uint lastId = 0;
    QString body;
    QStringList actions;
    QList<uint> replacements;
  public slots:
    QStringList GetCapabilities() {
        return {"actions", "body", "body-markup"};
    }
    QString GetServerInformation(QString &vendor, QString &version, QString &specification) {
        vendor = "Archcodex tests";
        version = "1";
        specification = "1.2";
        return "Fixture";
    }
    uint Notify(const QString &, uint replaces, const QString &, const QString &,
                const QString &text, const QStringList &buttons, const QVariantMap &, int) {
        body = text;
        actions = buttons;
        replacements << replaces;
        return replaces ? replaces : ++lastId;
    }
    void CloseNotification(uint) {}
  signals:
    void ActionInvoked(uint id, const QString &action);
    void ActivationToken(uint id, const QString &token);
};
class NotificationsTests : public QObject {
    Q_OBJECT
    Portal portal;
    Desktop desktop;
    QDBusConnection bus = QDBusConnection::sessionBus();
    QDBusConnection serviceBus =
        QDBusConnection::connectToBus(QDBusConnection::SessionBus, "notification-fixture");
    SessionAttention event() {
        SessionAttention result;
        result.session.backendId = "backend";
        result.session.id = "session";
        result.session.title = "Named session <test>";
        result.session.cwd = "/work/project";
        result.session.rolloutPath = "/saved/session.jsonl";
        result.session.persistenceKnown = true;
        result.reason = SessionAttention::Reason::Question;
        return result;
    }
    BackendProfile profile() {
        BackendProfile p;
        p.id = "backend";
        p.name = "Remote container";
        return p;
    }
  private slots:
    void initTestCase() {
        QVERIFY(bus.isConnected());
        QVERIFY(serviceBus.isConnected());
        QVERIFY(serviceBus.registerService("org.freedesktop.portal.Desktop"));
        QVERIFY(serviceBus.registerObject("/org/freedesktop/portal/desktop", &portal,
                                          QDBusConnection::ExportAllSlots |
                                              QDBusConnection::ExportAllSignals));
        QVERIFY(serviceBus.registerService("org.freedesktop.Notifications"));
        QVERIFY(serviceBus.registerObject("/org/freedesktop/Notifications", &desktop,
                                          QDBusConnection::ExportAllSlots |
                                              QDBusConnection::ExportAllSignals));
    }
    void init() {
        // Drain withdrawals from the preceding test across the separate bus
        // connections before resetting counters for this test.
        QDBusPendingCallWatcher barrier(bus.asyncCall(QDBusMessage::createMethodCall(
            "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
            "org.freedesktop.DBus.Peer", "Ping")));
        QTRY_VERIFY(barrier.isFinished());
        QVERIFY(!QDBusPendingReply<>(barrier).isError());
        portal.added.clear();
        portal.removed.clear();
    }
    void portalActionsPreserveIdentityAndActivationTokens() {
        Notifications notifications(true, bus);
        QSignalSpy open(&notifications, &Notifications::openRequested);
        notifications.show(event(), profile());
        QTRY_COMPARE(portal.added.size(), 1);
        const auto [id, properties] = portal.added.first();
        QCOMPARE(properties["title"].toString(), "Codex needs your input");
        QVERIFY(properties["body"].toString().contains("Named session <test>"));
        const auto buttons = qdbus_cast<QList<QVariantMap>>(properties["buttons"]);
        QCOMPARE(buttons.size(), 1);
        QCOMPARE(buttons.first()["action"].toString(), "open");
        QCOMPARE(properties["default-action"].toString(), "default");
        QCOMPARE(open.size(), 0); // Delivery cannot activate a window.
        emit portal.ActionInvoked("unknown", "open", {});
        emit portal.ActionInvoked(id, "unrecognized", {});
        emit portal.ActionInvoked(id, "open",
                                  {QVariantMap{{"activation-token", "desktop_TIME1234"}}});
        QTRY_COMPARE(open.size(), 1);
        QCOMPARE(open.first()[0].toString(), "backend");
        QCOMPARE(open.first()[1].toString(), "session");
        QCOMPARE(open.first()[2].toString(), "desktop_TIME1234");
        emit portal.ActionInvoked(id, "default", {});
        QTest::qWait(20);
        QCOMPARE(open.size(), 1);
        QTRY_VERIFY(portal.removed.contains(id));
    }
    void updatesReuseIdentityAndDisableAndShutdownWithdraw() {
        QString second;
        {
            Notifications notifications(true, bus);
            QSignalSpy open(&notifications, &Notifications::openRequested);
            notifications.show(event(), profile());
            QTRY_COMPARE(portal.added.size(), 1);
            const auto first = portal.added.first().first;
            notifications.show(event(), profile());
            QTRY_COMPARE(portal.added.size(), 2);
            second = portal.added.last().first;
            QCOMPARE(second, first);
            QVERIFY(portal.removed.isEmpty());
            notifications.setEnabled(false);
            notifications.show(event(), profile());
            QTRY_VERIFY(portal.removed.contains(second));
            QTest::qWait(20);
            QCOMPARE(open.size(), 0);
            QCOMPARE(portal.added.size(), 2);
            notifications.setEnabled(true);
            notifications.show(event(), profile());
            QTRY_COMPARE(portal.added.size(), 3);
            second = portal.added.last().first;
        }
        QTRY_VERIFY(portal.removed.contains(second));
    }
    void containerScopeAndActionsStayBoundToTheirSource() {
        Notifications notifications(true, bus);
        QSignalSpy open(&notifications, &Notifications::openRequested);
        auto a = profile(), b = profile();
        a.id = "container-a";
        b.id = "container-b";
        a.kind = b.kind = "mycodex";
        a.identity["containerWorkdir"] = "/work/a";
        b.identity["containerWorkdir"] = "/work/b";
        auto attention = event();
        attention.session.id = "shared-history-id";
        attention.session.cwd = "/work/a/sub";
        attention.session.backendId = a.id;
        notifications.show(attention, a);
        QTRY_COMPARE(portal.added.size(), 1);
        const auto aPopup = portal.added.last().first;
        attention.session.backendId = b.id;
        notifications.show(attention, b); // Same shared record, wrong container WORKDIR.
        attention.session.cwd = "/work/b-other";
        notifications.show(attention, b); // Path-component boundary, not a string prefix.
        attention.session.cwd.clear();
        notifications.show(attention, b);
        QTest::qWait(20);
        QCOMPARE(portal.added.size(), 1);
        attention.session.id = "own-b";
        attention.session.cwd = "/work/b";
        notifications.show(attention, b);
        QTRY_COMPARE(portal.added.size(), 2);
        const auto bPopup = portal.added.last().first;
        notifications.setMyCodexWorkdirOnly(false);
        attention.session.id = "shared-history-id";
        attention.session.cwd = "/work/a/sub";
        notifications.show(attention, b);
        QTRY_COMPARE(portal.added.size(), 3);
        const auto foreignPopup = portal.added.last().first;
        QVERIFY(foreignPopup != aPopup);
        notifications.setMyCodexWorkdirOnly(true);
        QTRY_VERIFY(portal.removed.contains(foreignPopup));
        emit portal.ActionInvoked(foreignPopup, "open", {});
        emit portal.ActionInvoked(aPopup, "open", {});
        emit portal.ActionInvoked(bPopup, "open", {});
        QTRY_COMPARE(open.size(), 2);
        QCOMPARE(open[0][0].toString(), a.id);
        QCOMPARE(open[0][1].toString(), "shared-history-id");
        QCOMPARE(open[1][0].toString(), b.id);
        QCOMPARE(open[1][1].toString(), "own-b");
    }
    void unsavedTargetsAndWithdrawnBackendsCannotActivate() {
        Notifications notifications(true, bus);
        QSignalSpy open(&notifications, &Notifications::openRequested);
        auto attention = event();
        attention.session.ephemeral = true;
        notifications.show(attention, profile());
        attention.session.ephemeral = false;
        attention.session.rolloutPath.clear();
        notifications.show(attention, profile());
        QTest::qWait(20);
        QVERIFY(portal.added.isEmpty());
        notifications.show(event(), profile());
        QTRY_COMPARE(portal.added.size(), 1);
        const auto id = portal.added.last().first;
        notifications.withdrawBackend(profile().id);
        QTRY_VERIFY(portal.removed.contains(id));
        emit portal.ActionInvoked(id, "open", {});
        QTest::qWait(20);
        QVERIFY(open.isEmpty());
    }
    void viewedTerminalIsQuietButOtherSourcesAndBackgroundStillNotify() {
        Notifications notifications(true, bus);
        notifications.show(event(), profile());
        QTRY_COMPARE(portal.added.size(), 1);
        const auto old = portal.added.last().first;
        notifications.setViewedSession("backend", "session");
        QTRY_VERIFY(portal.removed.contains(old));
        notifications.show(event(), profile());
        auto background = event();
        background.session.id = "another-open-terminal";
        notifications.show(background, profile());
        auto other = profile();
        other.id = "other-backend";
        background.session.id = "session";
        background.session.backendId = other.id;
        notifications.show(background, other);
        QTRY_COMPARE(portal.added.size(), 3);
        notifications.setViewedSession({}, {}); // Window is hidden, minimized or inactive.
        notifications.show(event(), profile());
        QTRY_COMPARE(portal.added.size(), 4);
    }
    void olderFailedSendCannotEraseUpdatedPopup() {
        Notifications notifications(true, bus);
        QSignalSpy open(&notifications, &Notifications::openRequested);
        portal.delayNext = true;
        notifications.show(event(), profile());
        QTRY_COMPARE(portal.delayed.size(), 1);
        notifications.show(event(), profile());
        QTRY_COMPARE(portal.added.size(), 2);
        QCOMPARE(portal.added[0].first, portal.added[1].first);
        QTRY_COMPARE(notifications.findChildren<QDBusPendingCallWatcher *>().size(), 1);
        QTest::ignoreMessage(QtWarningMsg,
                             "Desktop notification failed: \"org.freedesktop.DBus.Error.Failed\"");
        QVERIFY(serviceBus.send(portal.delayed.takeFirst().createErrorReply(
            "org.freedesktop.DBus.Error.Failed", "Delayed first send failed")));
        QTRY_VERIFY(notifications.findChildren<QDBusPendingCallWatcher *>().isEmpty());
        QVERIFY(portal.removed.isEmpty());
        emit portal.ActionInvoked(portal.added.last().first, "open", {});
        QTRY_COMPARE(open.size(), 1);
    }
    void nativeUpdatesReplaceExistingDesktopNotification() {
        Notifications notifications(false, bus);
        QSignalSpy open(&notifications, &Notifications::openRequested);
        const auto before = desktop.replacements.size();
        notifications.show(event(), profile());
        QTRY_COMPARE(desktop.replacements.size(), before + 1);
        const auto id = desktop.lastId;
        auto attention = event();
        attention.session.title = "Updated title";
        notifications.show(attention, profile());
        QTRY_COMPARE(desktop.replacements.size(), before + 2);
        QCOMPARE(desktop.replacements.last(), id);
        QCOMPARE(desktop.body.count("Updated title"), 1);
        emit desktop.ActionInvoked(id, "default");
        QTRY_COMPARE(open.size(), 1);
    }
    void olderPortalDefaultActionWorksWithoutToken() {
        Notifications notifications(true, bus);
        QSignalSpy open(&notifications, &Notifications::openRequested);
        notifications.show(event(), profile());
        QTRY_COMPARE(portal.added.size(), 1);
        emit portal.ActionInvoked(portal.added.first().first, "default", {});
        QTRY_COMPARE(open.size(), 1);
        QVERIFY(open.first()[2].toString().isEmpty());
    }
    void nativeNotificationEscapesMarkupAndRoutesAction() {
        Notifications notifications(false, bus);
        QSignalSpy open(&notifications, &Notifications::openRequested);
        const auto before = desktop.lastId;
        notifications.show(event(), profile());
        QTRY_VERIFY(desktop.lastId > before);
        QVERIFY(desktop.body.contains("&lt;test&gt;"));
        QVERIFY(!desktop.body.contains("<test>"));
        QVERIFY(desktop.actions.contains("default"));
        QVERIFY(desktop.actions.contains("Open session"));
        emit desktop.ActivationToken(desktop.lastId, "click-token");
        emit desktop.ActionInvoked(desktop.lastId, "default");
        QTRY_COMPARE(open.size(), 1);
        QCOMPARE(open.first()[0].toString(), "backend");
        QCOMPARE(open.first()[1].toString(), "session");
        QCOMPARE(open.first()[2].toString(), "click-token");
    }
    void outstandingPortalNotificationsAreBounded() {
        Notifications notifications(true, bus);
        auto notification = event();
        for (int i = 0; i < 160; ++i) {
            notification.session.id = QString::number(i);
            notifications.show(notification, profile());
            QTRY_COMPARE(portal.added.size(), i + 1);
            QTRY_VERIFY(notifications.findChildren<QDBusPendingCallWatcher *>().isEmpty());
        }
        QTRY_COMPARE(portal.removed.size(), 32);
        notifications.setEnabled(false);
        QTRY_COMPARE(portal.removed.size(), 160);
    }
    void portalDeliveryBurstIsBoundedAndRecovers() {
        Notifications notifications(true, bus);
        auto notification = event();
        // Pending replies cannot grow without bound when delivery outpaces D-Bus.
        for (int i = 0; i < 1000; ++i) {
            notification.session.id = QString::number(i);
            notifications.show(notification, profile());
        }
        QTRY_COMPARE(portal.added.size(), 128);
        QTRY_VERIFY(notifications.findChildren<QDBusPendingCallWatcher *>().isEmpty());
        notification.session.id = "after-burst";
        notifications.show(notification, profile());
        QTRY_COMPARE(portal.added.size(), 129);
    }
    void restoreKeepsMaximizedAndFullscreen_data() {
        QTest::addColumn<int>("state");
        QTest::newRow("maximized") << int(Qt::WindowMaximized);
        QTest::newRow("fullscreen") << int(Qt::WindowFullScreen);
    }
    void restoreKeepsMaximizedAndFullscreen() {
        QFETCH(int, state);
        QWidget window;
        window.setWindowState(Qt::WindowStates(state) | Qt::WindowMinimized);
        Notifications::presentWindow(&window, {});
        QVERIFY(!window.isMinimized());
        QVERIFY(window.windowState().testFlag(Qt::WindowState(state)));
    }
    void x11ActivationRequestsExistingDesktopWithoutMovingWindow() {
        // This fixture owns root-window properties: opt in only on a private Xvfb.
        if (qEnvironmentVariable("ARCHCODEX_TEST_X11_WM") != "1" ||
            QGuiApplication::platformName() != "xcb")
            QSKIP("Run on a private Xvfb with ARCHCODEX_TEST_X11_WM=1");
        std::unique_ptr<xcb_connection_t, decltype(&xcb_disconnect)> connection(
            xcb_connect(nullptr, nullptr), xcb_disconnect);
        auto *c = connection.get();
        QVERIFY(!xcb_connection_has_error(c));
        const auto root = xcb_setup_roots_iterator(xcb_get_setup(c)).data->root;
        const auto atom = [c](const QByteArray &name) {
            const auto cookie = xcb_intern_atom(c, false, name.size(), name.constData());
            auto *reply = xcb_intern_atom_reply(c, cookie, nullptr);
            const auto value = reply ? reply->atom : XCB_ATOM_NONE;
            free(reply);
            return value;
        };
        const auto current = atom("_NET_CURRENT_DESKTOP"), active = atom("_NET_ACTIVE_WINDOW"),
                   desktop = atom("_NET_WM_DESKTOP");
        const auto property = [c](xcb_window_t window, xcb_atom_t key, uint32_t value,
                                  xcb_atom_t type = XCB_ATOM_CARDINAL) {
            xcb_change_property(c, XCB_PROP_MODE_REPLACE, window, key, type, 32, 1, &value);
            xcb_flush(c);
        };
        property(root, current, 0);
        property(root, atom("_NET_NUMBER_OF_DESKTOPS"), 2);
        const uint32_t mask = XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY | XCB_EVENT_MASK_PROPERTY_CHANGE;
        xcb_change_window_attributes(c, root, XCB_CW_EVENT_MASK, &mask);
        xcb_flush(c);
        bool moved = false, activatedOnTargetDesktop = false;
        uint32_t timestamp = 0, selected = 0;
        QSocketNotifier ready(xcb_get_file_descriptor(c), QSocketNotifier::Read);
        const auto receive = [&] {
            while (auto *event = xcb_poll_for_event(c)) {
                if ((event->response_type & 0x7f) == XCB_CLIENT_MESSAGE) {
                    const auto *message = reinterpret_cast<xcb_client_message_event_t *>(event);
                    if (message->type == current) {
                        selected = message->data.data32[0];
                        property(root, current, selected);
                    } else if (message->type == active) {
                        timestamp = message->data.data32[1];
                        activatedOnTargetDesktop = selected == 1;
                        property(root, active, message->window, XCB_ATOM_WINDOW);
                    } else if (message->type == desktop)
                        moved = true;
                }
                free(event);
            }
        };
        connect(&ready, &QSocketNotifier::activated, this, receive);
        QWidget window;
        window.show();
        property(window.winId(), desktop, 1); // X11 desktop 1 is the user's desktop 2.
        const auto state = atom("WM_STATE");
        const uint32_t normal[] = {1, XCB_WINDOW_NONE};
        xcb_change_property(c, XCB_PROP_MODE_REPLACE, window.winId(), state, state, 32, 2, normal);
        xcb_flush(c);
        QTRY_COMPARE(KX11Extras::currentDesktop(), 1);
        QVERIFY(KWindowInfo(window.winId(), NET::WMDesktop).valid());
        QCOMPARE(KWindowInfo(window.winId(), NET::WMDesktop).desktop(), 2);
        Notifications::presentWindow(&window, "notification_TIME1234");
        QTRY_VERIFY((receive(), activatedOnTargetDesktop));
        QCOMPARE(timestamp, 1234u);
        QCOMPARE(KX11Extras::currentDesktop(), 2);
        QCOMPARE(KWindowInfo(window.winId(), NET::WMDesktop).desktop(), 2);
        QVERIFY(!moved);
    }
};
QTEST_MAIN(NotificationsTests)
#include "test_notifications.moc"
