// SPDX-License-Identifier: GPL-3.0-or-later
#include "terminal.h"
#include <QAccessible>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLabel>
#include <QPushButton>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QtTest>
using namespace Archcodex;

class TerminalTests : public QObject {
    Q_OBJECT
  private slots:
    void containerTerminalUsesSelectedLocalExecutableAndExplicitRelay() {
        QTemporaryDir dir;
        QFile script(dir.path() + "/local codex ; literal");
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write(
            "#!/usr/bin/python3\nimport json,sys\n"
            "if '--version' in sys.argv: print('codex-cli 0.166.0')\n"
            "else:\n with open(__file__+'.args','w') as out: json.dump(sys.argv[1:],out)\n");
        script.close();
        QVERIFY(script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        TerminalArea area;
        area.setLocalCodexExecutable(script.fileName());
        area.show();
        BackendProfile p{"container", "Selected remote container", "mycodex",
                         "/remote/app-server.sock"};
        p.executable = "/remote-only/codex";
        p.relaySocket = "/local/explicit-relay.sock";
        p.sshHost = "must-not-be-contacted";
        p.containerId = QString(64, 'a');
        p.containerEpoch = "epoch";
        p.environmentId = "execution-environment";
        p.dockerContext = "default";
        p.runtimeUser = "alice";
        p.runtimeHome = "/home/alice";
        p.discoveryHost = "remote-host";
        auto s = Session::fromThread(p.id, {{"id", "thread-1"}, {"cwd", "/remote-only/workdir"}});
        s.executionObserved = true;
        s.executionEnvironment = p.environmentId;
        QSignalSpy exits(&area, &TerminalArea::clientExited);
        QString error;
        QVERIFY2(area.open(p, s, &error), qPrintable(error));
        QTRY_COMPARE(exits.size(), 1);
        QCOMPARE(exits.first()[1].toInt(), 0);
        QFile args(script.fileName() + ".args");
        QVERIFY(args.open(QIODevice::ReadOnly));
        QCOMPARE(QJsonDocument::fromJson(args.readAll()).array(),
                 QJsonArray({"--remote", "unix:///local/explicit-relay.sock", "resume", "--",
                             "thread-1"}));
        // The test executable was invoked only as a version probe and explicit
        // remote client; neither SSH nor an app-server launch is in this path.
        area.closeCurrent();
    }
    void controlTabCyclesFrozenRecentOrderAndNeverReachesTheTerminal() {
        QTemporaryDir dir;
        QFile script(dir.path() + "/client");
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write("#!/bin/sh\nfor id; do :; done\nprintf 'READY\\n'\nIFS= read -r "
                     "answer\nprintf '%s' \"$answer\" > '" +
                     dir.path().toUtf8() + "/'\"$id\"\n");
        script.close();
        QVERIFY(script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        TerminalArea area;
        area.resize(800, 500);
        area.show();
        area.activateWindow();
        QVERIFY(QTest::qWaitForWindowExposed(&area));
        QSignalSpy exits(&area, &TerminalArea::clientExited);
        QSignalSpy titles(&area, &TerminalArea::currentChanged);
        BackendProfile p{
            "b", "Full server · host · storage", "local-unix", "/unused", script.fileName(), false};
        QString error;
        for (const auto &id : {"a", "b", "c"}) {
            auto s = Session::fromThread("b", {{"id", id}, {"name", id}, {"cwd", dir.path()}});
            QVERIFY2(area.open(p, s, &error), qPrintable(error));
        }
        QVERIFY(titles.last()[0].toString().contains(p.name));
        QTRY_VERIFY(QApplication::focusWidget() && area.isAncestorOf(QApplication::focusWidget()));
        const auto tab = [&](bool backwards = false) {
            const auto modifiers =
                Qt::ControlModifier | (backwards ? Qt::ShiftModifier : Qt::NoModifier);
            QKeyEvent shortcut(QEvent::ShortcutOverride, Qt::Key_Tab, modifiers);
            auto *input = QApplication::focusWidget();
            QVERIFY(input && area.isAncestorOf(input));
            QApplication::sendEvent(input, &shortcut);
            QVERIFY(shortcut.isAccepted());
            QKeyEvent press(QEvent::KeyPress, backwards ? Qt::Key_Backtab : Qt::Key_Tab, modifiers);
            QApplication::sendEvent(input, &press);
        };
        const auto release = [&] {
            QKeyEvent event(QEvent::KeyRelease, Qt::Key_Control, Qt::NoModifier);
            QApplication::sendEvent(QApplication::focusWidget(), &event);
        };
        QCOMPARE(area.currentKey(), sessionKey("b", "c"));
        tab();
        QCOMPARE(area.currentKey(), sessionKey("b", "b"));
        tab();
        QCOMPARE(area.currentKey(), sessionKey("b", "a"));
        tab(true);
        QCOMPARE(area.currentKey(), sessionKey("b", "b"));
        release();
        tab();
        QCOMPARE(area.currentKey(), sessionKey("b", "c"));
        release();
        tab(true);
        QCOMPARE(area.currentKey(), sessionKey("b", "a"));
        tab(true);
        QCOMPARE(area.currentKey(), sessionKey("b", "b"));
        release();
        QTRY_VERIFY(QApplication::focusWidget() && area.isAncestorOf(QApplication::focusWidget()));
        auto *input = QApplication::focusWidget();
        auto *accessible = QAccessible::queryAccessibleInterface(input);
        QVERIFY(accessible && accessible->textInterface());
        auto *text = accessible->textInterface();
        QTRY_VERIFY(text->text(0, text->characterCount()).contains("READY"));
        QTest::keyClicks(input, "ordinary input");
        QTest::keyClick(input, Qt::Key_Return);
        QTRY_COMPARE(exits.size(), 1);
        QFile received(dir.path() + "/b");
        QVERIFY(received.open(QIODevice::ReadOnly));
        QCOMPARE(received.readAll(), QByteArray("ordinary input"));
        // Retained exit output participates, while closed views do not.
        area.switchRecent();
        QCOMPARE(area.currentKey(), sessionKey("b", "c"));
        area.switchRecent();
        QCOMPARE(area.currentKey(), sessionKey("b", "b"));
        area.closeCurrent();
        area.switchRecent();
        QCOMPARE(area.currentKey(), sessionKey("b", "c"));
        area.closeSession(sessionKey("b", "a")); // Closing a different row preserves the view.
        QCOMPARE(area.currentKey(), sessionKey("b", "c"));
        QVERIFY(!area.focusSession(sessionKey("b", "a")));
        area.closeBackend("b");
        area.switchRecent();
        QVERIFY(area.currentKey().isEmpty());
    }
    void containerHistoryAndChangedEnvironmentCannotLaunch() {
        TerminalArea area;
        BackendProfile p{"container", "Container"};
        p.kind = "mycodex";
        p.relaySocket = "/unused/fixture-relay";
        p.environmentId = "current-environment";
        Session s;
        s.backendId = p.id;
        s.id = "shared-thread";
        s.contextRequired = true;
        QString error;
        QVERIFY(!area.open(p, s, &error));
        QVERIFY(!error.isEmpty());
        s.executionObserved = true;
        s.executionEnvironment = "old-environment";
        QVERIFY(!area.open(p, s, &error));
        p.relaySocket.clear();
        s.executionEnvironment = p.environmentId;
        QVERIFY(!area.open(p, s, &error));
        QVERIFY(error.contains("no live desktop transport"));
    }
    void failedClientDoesNotSwitchBack_data() {
        QTest::addColumn<QByteArray>("command");
        QTest::addColumn<int>("exitCode");
        QTest::addColumn<bool>("crashed");
        QTest::newRow("error") << QByteArray("exit 7") << 7 << false;
        QTest::newRow("clean-exit") << QByteArray("exit 0") << 0 << false;
        QTest::newRow("signal") << QByteArray("kill -TERM $$") << 15 << true;
        QTest::newRow("missing-program")
            << QByteArray("exec /nonexistent/archcodex-test") << 127 << false;
    }
    void failedClientDoesNotSwitchBack() {
        QFETCH(QByteArray, command);
        QFETCH(int, exitCode);
        QFETCH(bool, crashed);
        QTest::failOnWarning(QRegularExpression(".*(Unexpected nullptr|No event config).*"));
        QTemporaryDir dir;
        QFile script(dir.path() + "/client");
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write("#!/bin/sh\nprintf 'CLIENT OUTPUT\\n'\ncase \"$*\" in *failure*) " + command +
                     ";; esac\nread answer\n");
        script.close();
        QVERIFY(script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        TerminalArea area;
        QSignalSpy exits(&area, &TerminalArea::clientExited);
        QSignalSpy states(&area, &TerminalArea::clientStateChanged);
        QSignalSpy restarts(&area, &TerminalArea::restartRequested);
        area.resize(800, 500);
        area.show();
        BackendProfile p{"b", "Local", "local-unix", "/unused", script.fileName(), false};
        auto session = Session::fromThread("b", {{"id", "first"}, {"cwd", dir.path()}});
        QString error;
        QVERIFY2(area.open(p, session, &error), qPrintable(error));
        auto *stack = area.findChild<QStackedWidget *>();
        auto *first = stack->currentWidget();
        session.id = "failure";
        QVERIFY2(area.open(p, session, &error), qPrintable(error));
        QPointer<QWidget> failed = stack->currentWidget();
        QVERIFY(first != failed);
        QTRY_COMPARE(exits.size(), 1);
        QCOMPARE(exits[0][1].toInt(), exitCode);
        QCOMPARE(exits[0][2].toBool(), crashed);
        QVERIFY(failed);
        auto client = qvariant_cast<Session>(states.last()[0]);
        QVERIFY(client.terminalOpen && client.isActive());
        QVERIFY(!client.clientIssue.isEmpty());
        QVERIFY(!client.isRunning() && !client.isWaiting());
        QVERIFY(area.focusSession(sessionKey("b", "first")));
        session.id = "first";
        QVERIFY(!area.restart(p, session, &error)); // Never replace a running client.
        session.id = "failure";
        QVERIFY(area.focusSession(sessionKey("b", "failure")));
        QCOMPARE(stack->currentWidget(), failed.data());
        bool retainedOutput = false;
        for (auto *widget : failed->findChildren<QWidget *>()) {
            auto *accessible = QAccessible::queryAccessibleInterface(widget);
            if (accessible && accessible->textInterface()) {
                auto *text = accessible->textInterface();
                retainedOutput |= text->text(0, text->characterCount()).contains("CLIENT OUTPUT");
            }
        }
        QVERIFY(retainedOutput);
        auto *retry = area.findChild<QPushButton *>("reconnectTerminal");
        QVERIFY(retry && retry->isVisible());
        QTest::mouseClick(retry, Qt::LeftButton);
        QCOMPARE(restarts.size(), 1);
        QCOMPARE(qvariant_cast<Session>(restarts[0][0]).id, session.id);
        QCOMPARE(stack->currentWidget(), failed.data()); // Keep output until validation succeeds.
        auto changed = p;
        changed.kind = "mycodex";
        changed.environmentId = "different-environment";
        QVERIFY(!area.restart(changed, session, &error));
        QCOMPARE(stack->currentWidget(), failed.data());
        // The controller supplies a refreshed profile, not the old command.
        QFile refreshed(dir.path() + "/refreshed-client");
        QVERIFY(refreshed.open(QIODevice::WriteOnly));
        refreshed.write("#!/bin/sh\nprintf 'FRESH PROFILE\\n'\nexit 9\n");
        refreshed.close();
        QVERIFY(refreshed.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        p.executable = refreshed.fileName();
        QVERIFY2(area.restart(p, session, &error), qPrintable(error));
        QTRY_COMPARE(exits.size(), 2);
        QCOMPARE(exits[1][1].toInt(), 9);
        QVERIFY(!failed);
        QVERIFY(stack->currentWidget() != first);
        area.closeCurrent();
        QVERIFY(!qvariant_cast<Session>(states.last()[0]).terminalOpen);
        QVERIFY(!area.restart(p, session, &error));
        session.id = "first";
        QVERIFY2(area.open(p, session, &error), qPrintable(error));
        QCOMPARE(stack->currentWidget(), first);
    }
    void restartingNewSessionRepeatsTheLauncherWithoutResumingItsSyntheticId() {
        QTemporaryDir dir;
        QFile script(dir.path() + "/client");
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write(
            "#!/bin/sh\nfor arg; do\ncase \"$arg\" in resume|new-*) exit 8;; esac\ndone\nexit 0\n");
        script.close();
        QVERIFY(script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        TerminalArea area;
        QSignalSpy exits(&area, &TerminalArea::clientExited);
        QSignalSpy states(&area, &TerminalArea::clientStateChanged);
        BackendProfile p{"b", "Local", "local-unix", "/unused", script.fileName(), false};
        QString error;
        QVERIFY2(area.newSession(p, dir.path(), &error), qPrintable(error));
        QTRY_COMPARE(exits.size(), 1);
        QCOMPARE(exits[0][1].toInt(), 0);
        const auto client = qvariant_cast<Session>(states.last()[0]);
        QVERIFY(client.id.startsWith("new-"));
        QVERIFY2(area.restart(p, client, &error), qPrintable(error));
        QTRY_COMPARE(exits.size(), 2);
        QCOMPARE(exits[1][1].toInt(), 0);
        QCOMPARE(exits[0][0], exits[1][0]);
        area.closeCurrent();
    }
};
QTEST_MAIN(TerminalTests)
#include "test_terminal.moc"
