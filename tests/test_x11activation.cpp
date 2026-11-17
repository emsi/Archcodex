// SPDX-License-Identifier: GPL-3.0-or-later
#include "notifications.h"
#include <KStartupInfo>
#include <KWindowInfo>
#include <KX11Extras>
#include <QApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QWidget>
#include <QtTest>
#include <cstdio>
#include <memory>
#include <xcb/xcb.h>
#include <xcb/xtest.h>

using namespace Archcodex;

class OtherWindow : public QProcess {
  public:
    ~OtherWindow() override {
        if (state() != NotRunning) {
            terminate();
            if (!waitForFinished(1000)) {
                kill();
                waitForFinished(1000);
            }
        }
    }
    WId open(const QString &name) {
        QStringList command;
        for (const auto &arg :
             QJsonDocument::fromJson(qgetenv("ARCHCODEX_X11_TEST_COMMAND")).array())
            command << arg.toString();
        if (command.isEmpty())
            command << QCoreApplication::applicationFilePath();
        command << "--window" << name;
        start(command.first(), command.mid(1));
        if (!waitForStarted(5000) || !waitForReadyRead(5000))
            return 0;
        return readLine().trimmed().toULongLong();
    }
};

class X11ActivationTests : public QObject {
    Q_OBJECT
    std::unique_ptr<xcb_connection_t, decltype(&xcb_disconnect)> connection{nullptr,
                                                                            xcb_disconnect};
    xcb_connection_t *c = nullptr;
    xcb_window_t root = 0;
    xcb_atom_t atom(const QByteArray &name) {
        auto *reply =
            xcb_intern_atom_reply(c, xcb_intern_atom(c, false, name.size(), name.data()), nullptr);
        const auto result = reply ? reply->atom : XCB_ATOM_NONE;
        free(reply);
        return result;
    }
    uint32_t property(WId window, const QByteArray &name) {
        auto *reply = xcb_get_property_reply(
            c, xcb_get_property(c, false, window, atom(name), XCB_GET_PROPERTY_TYPE_ANY, 0, 1),
            nullptr);
        const auto value = reply && reply->format == 32 && xcb_get_property_value_length(reply) >= 4
                               ? *static_cast<uint32_t *>(xcb_get_property_value(reply))
                               : 0;
        free(reply);
        return value;
    }
    uint32_t userTime(WId window) {
        const auto timeWindow = property(window, "_NET_WM_USER_TIME_WINDOW");
        return property(timeWindow ? timeWindow : window, "_NET_WM_USER_TIME");
    }
    bool focusAndType(WId window) {
        // Test setup represents a user selecting another app. The production
        // presentation path must use ordinary application activation instead.
        KStartupInfoId input;
        input.initId(KStartupInfo::createNewStartupId());
        KX11Extras::forceActiveWindow(window, input.timestamp());
        const auto focused = [&] {
            auto *reply = xcb_get_input_focus_reply(c, xcb_get_input_focus(c), nullptr);
            const bool result = reply && reply->focus == window;
            free(reply);
            return result && KX11Extras::activeWindow() == window;
        };
        if (!QTest::qWaitFor(focused, 5000))
            return false;
        const auto old = userTime(window);
        QTest::qWait(25); // Give subsequent genuine X input a distinct timestamp.
        xcb_test_fake_input(c, XCB_KEY_PRESS, 50, XCB_CURRENT_TIME, root, 0, 0, 0);
        xcb_test_fake_input(c, XCB_KEY_RELEASE, 50, XCB_CURRENT_TIME, root, 0, 0, 0);
        xcb_flush(c);
        return QTest::qWaitFor([&] { return userTime(window) && userTime(window) != old; }, 5000);
    }
    static QString freshToken() {
        return QString::fromUtf8(KStartupInfo::createNewStartupId());
    }

  private slots:
    void initTestCase() {
        if (qEnvironmentVariable("ARCHCODEX_TEST_REAL_KWIN") != "1" ||
            QGuiApplication::platformName() != "xcb")
            QSKIP("Run only through tests/run-x11-activation.py on its private display/bus");
        connection.reset(xcb_connect(nullptr, nullptr));
        c = connection.get();
        QVERIFY(!xcb_connection_has_error(c));
        root = xcb_setup_roots_iterator(xcb_get_setup(c)).data->root;
        QTRY_VERIFY(property(root, "_NET_SUPPORTING_WM_CHECK"));
        QCOMPARE(KX11Extras::numberOfDesktops(), 2);
    }
    void explicitClick_data() {
        QTest::addColumn<bool>("otherDesktop");
        QTest::addColumn<QString>("tokenKind");
        QTest::addColumn<int>("state");
        QTest::addColumn<bool>("covered");
        QTest::addColumn<bool>("keepAbove");
        QTest::newRow("covered-here-tokenless")
            << false << QStringLiteral("empty") << 0 << true << false;
        QTest::newRow("covered-other-desktop-tokenless")
            << true << QStringLiteral("empty") << 0 << true << false;
        QTest::newRow("covered-here-token")
            << false << QStringLiteral("fresh") << 0 << true << false;
        QTest::newRow("covered-other-desktop-token")
            << true << QStringLiteral("fresh") << 0 << true << false;
        QTest::newRow("malformed-token") << true << QStringLiteral("invalid") << 0 << true << false;
        QTest::newRow("already-frontmost")
            << true << QStringLiteral("empty") << 0 << false << false;
        QTest::newRow("minimized")
            << true << QStringLiteral("empty") << int(Qt::WindowMinimized) << true << false;
        QTest::newRow("maximized")
            << true << QStringLiteral("empty") << int(Qt::WindowMaximized) << true << false;
        QTest::newRow("fullscreen")
            << true << QStringLiteral("empty") << int(Qt::WindowFullScreen) << true << false;
        QTest::newRow("minimized-maximized")
            << true << QStringLiteral("empty") << int(Qt::WindowMinimized | Qt::WindowMaximized)
            << true << false;
        QTest::newRow("keep-above-respected")
            << false << QStringLiteral("empty") << 0 << true << true;
        QTest::newRow("stale-supplied-time-respected")
            << false << QStringLiteral("stale") << 0 << true << false;
    }
    void explicitClick() {
        QFETCH(bool, otherDesktop);
        QFETCH(QString, tokenKind);
        QFETCH(int, state);
        QFETCH(bool, covered);
        QFETCH(bool, keepAbove);
        QWidget window;
        window.resize(640, 480);
        window.show();
        const auto target = window.winId();
        QTRY_VERIFY(KX11Extras::hasWId(target));
        KX11Extras::setCurrentDesktop(2);
        KX11Extras::setOnDesktop(target, 2);
        QTRY_COMPARE(KX11Extras::currentDesktop(), 2);
        QVERIFY(focusAndType(target));
        const auto oldTime = userTime(target);
        window.setWindowState(Qt::WindowStates(state));
        OtherWindow blocker, away;
        const auto cover = blocker.open("ArchcodexActivationCover");
        const auto elsewhere = away.open("ArchcodexActivationElsewhere");
        QVERIFY(cover);
        QVERIFY(elsewhere);
        // Wait for the WM to manage each window before changing its desktop;
        // otherwise its initial placement can race with the test's request.
        QTRY_VERIFY(KX11Extras::hasWId(cover));
        QTRY_VERIFY(KX11Extras::hasWId(elsewhere));
        KX11Extras::setOnDesktop(cover, 2);
        KX11Extras::setOnDesktop(elsewhere, 1);
        QTRY_COMPARE(KWindowInfo(elsewhere, NET::WMDesktop).desktop(), 1);
        if (keepAbove)
            KX11Extras::setState(cover, NET::KeepAbove);
        if (covered) {
            QVERIFY(focusAndType(cover));
            QVERIFY(userTime(cover) != oldTime);
        } else
            QVERIFY(focusAndType(target));
        if (otherDesktop) {
            KX11Extras::setCurrentDesktop(1);
            QTRY_COMPARE(KX11Extras::currentDesktop(), 1);
            QVERIFY(focusAndType(elsewhere));
        }
        // Establish that this setup really contains a background application;
        // an accepting fake WM or windows in a single app would miss the bug.
        const auto before = otherDesktop ? elsewhere : cover;
        QCOMPARE(KX11Extras::activeWindow(), before);
        if (covered)
            QCOMPARE(userTime(target), oldTime);
        const auto token = tokenKind == "fresh"     ? freshToken()
                           : tokenKind == "stale"   ? QString("click_TIME%1").arg(oldTime)
                           : tokenKind == "invalid" ? QString("click_TIMEnot-a-time")
                                                    : QString();
        Notifications::presentWindow(&window, token);
        QTRY_COMPARE(KX11Extras::currentDesktop(), 2);
        if (tokenKind == "stale" ||
            qEnvironmentVariableIntValue("ARCHCODEX_TEST_FOCUS_POLICY") == 4) {
            QTest::qWait(300);
            QVERIFY(KX11Extras::activeWindow() != target);
        } else {
            QTRY_COMPARE(KX11Extras::activeWindow(), target);
            QTRY_VERIFY(KX11Extras::stackingOrder().indexOf(target) >= 0);
            const auto stack = KX11Extras::stackingOrder();
            QVERIFY(stack.contains(cover));
            QVERIFY(keepAbove ? stack.indexOf(target) < stack.indexOf(cover)
                              : stack.indexOf(target) > stack.indexOf(cover));
        }
        QCOMPARE(KWindowInfo(target, NET::WMDesktop).desktop(), 2);
        QVERIFY(!window.isMinimized());
        QCOMPARE(window.windowState() & (Qt::WindowMaximized | Qt::WindowFullScreen),
                 Qt::WindowStates(state) & (Qt::WindowMaximized | Qt::WindowFullScreen));
    }
};

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    if (app.arguments().value(1) == "--window") {
        app.setApplicationName(app.arguments().value(2));
        QWidget window;
        window.setWindowTitle(app.applicationName());
        window.resize(640, 480);
        window.show();
        std::printf("%llu\n", static_cast<unsigned long long>(window.winId()));
        std::fflush(stdout);
        return app.exec();
    }
    app.setApplicationName("ArchcodexActivationTarget");
    X11ActivationTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "test_x11activation.moc"
