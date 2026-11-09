// SPDX-License-Identifier: GPL-3.0-or-later
#include "terminal.h"
#include <QAccessible>
#include <QFile>
#include <QLabel>
#include <QPushButton>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QtTest>
using namespace Archcodex;

class TerminalTests : public QObject {
    Q_OBJECT
  private slots:
    void containerHistoryAndChangedEnvironmentCannotLaunch() {
        TerminalArea area;
        BackendProfile p{"container", "Container"};
        p.kind = "mycodex";
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
        QTRY_COMPARE(exits.size(), 2);
        QVERIFY(!failed);
        QVERIFY(stack->currentWidget() != first);
        area.closeCurrent();
        QVERIFY(!qvariant_cast<Session>(states.last()[0]).terminalOpen);
        session.id = "first";
        QVERIFY2(area.open(p, session, &error), qPrintable(error));
        QCOMPARE(stack->currentWidget(), first);
    }
};
QTEST_MAIN(TerminalTests)
#include "test_terminal.moc"
