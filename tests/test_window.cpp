// SPDX-License-Identifier: GPL-3.0-or-later
#include "discovery.h"
#include "fixture.h"
#include "sessionmodel.h"
#include "terminal.h"
#include "window.h"
#include <QAction>
#include <QComboBox>
#include <QHelpEvent>
#include <QLineEdit>
#include <QMessageBox>
#include <QSettings>
#include <QTabBar>
#include <QToolTip>
#include <QTreeView>
#include <QtTest>

class WindowTests : public QObject {
    Q_OBJECT
  private slots:
    void viewsFollowEventsAndPreserveNavigation() {
        Fixture fixture;
        QTemporaryDir data;
        Window window(data.path(), false);
        window.show();
        window.findChild<HostDiscovery *>()->snapshot(
            {QJsonObject{{"path", fixture.path()}, {"hostEnvironment", true}}}, {});
        auto *tree = window.findChild<QTreeView *>();
        auto *tabs = window.findChild<QTabBar *>();
        QCOMPARE(tabs->count(), 4);
        QTRY_COMPARE(tree->model()->rowCount(), 1);
        auto server = [&] { return tree->model()->index(0, 0); };
        auto project = [&] { return tree->model()->index(0, 0, server()); };
        QTRY_VERIFY(project().isValid());
        QTRY_COMPARE(tree->model()->index(0, 0, project()).data(ThreadRole).toString(), "thread-1");
        tree->expand(project());
        tree->setCurrentIndex(tree->model()->index(0, 0, project()));
        tabs->setCurrentIndex(1);
        QCOMPARE(tree->model()->rowCount(), 1);
        QVERIFY(tree->isExpanded(project()));
        QCOMPARE(tree->currentIndex().data(ThreadRole).toString(), "thread-1");
        tabs->setCurrentIndex(2);
        QCOMPARE(tree->model()->rowCount(), 0);
        tabs->setCurrentIndex(3);
        QCOMPARE(tree->model()->rowCount(), 1);
        fixture.notify(
            "thread/status/changed",
            {{"threadId", "thread-1"},
             {"status", QJsonObject{{"type", "active"}, {"activeFlags", QJsonArray{}}}}});
        QTRY_COMPARE(tree->model()->rowCount(), 0);
        tabs->setCurrentIndex(2);
        QCOMPARE(tree->model()->rowCount(), 1);
        // Tooltips keep a stable snapshot while events arrive under the pointer.
        const auto pos = tree->visualRect(project()).center();
        QHelpEvent help(QEvent::ToolTip, pos, tree->viewport()->mapToGlobal(pos));
        QApplication::sendEvent(tree->viewport(), &help);
        QVERIFY(QToolTip::isVisible());
        const auto tip = QToolTip::text();
        fixture.notify("thread/status/changed",
                       {{"threadId", "thread-1"}, {"status", QJsonObject{{"type", "idle"}}}});
        tabs->setCurrentIndex(1);
        QTRY_VERIFY(tree->model()->index(0, 0, project()).data(WaitingRole).toBool());
        QApplication::sendEvent(tree->viewport(), &help);
        QCOMPARE(QToolTip::text(), tip);
        QToolTip::hideText();
        fixture.notify("thread/closed", {{"threadId", "thread-1"}});
        QTRY_COMPARE(tree->model()->rowCount(), 0);
        tabs->setCurrentIndex(0);
        QCOMPARE(tree->model()->rowCount(), 1);
        window.close();
    }
    void hideUnsavedSettingAndClientError() {
        Fixture fixture;
        fixture.threadFields = {{"path", QJsonValue::Null}, {"ephemeral", true}};
        QTemporaryDir data;
        {
            Window window(data.path(), false);
            window.show();
            window.findChild<HostDiscovery *>()->snapshot({QJsonObject{{"path", fixture.path()}}},
                                                          {});
            auto *tree = window.findChild<QTreeView *>();
            auto *tabs = window.findChild<QTabBar *>();
            auto *setting = window.findChild<QAction *>("hideUnsavedSessions");
            QVERIFY(setting && setting->isChecked());
            QTRY_VERIFY(fixture.methods.contains("thread/read"));
            QTRY_COMPARE(tree->model()->rowCount(), 1);
            QCOMPARE(tree->model()->index(0, 0).data(CountRole).toInt(), 0);
            setting->setChecked(false);
            QTRY_COMPARE(tree->model()->index(0, 0).data(CountRole).toInt(), 1);
            auto session = Session::fromThread(
                tree->model()->index(0, 0).data(BackendRole).toString(), fixture.thread());
            session.terminalOpen = true;
            session.clientIssue = "Terminal client exited with code 1";
            window.findChild<TerminalArea *>()->clientStateChanged(session);
            setting->setChecked(true);
            tabs->setCurrentIndex(1);
            QCOMPARE(tree->model()->rowCount(), 1);
            tabs->setCurrentIndex(2);
            QCOMPARE(tree->model()->rowCount(), 0);
            tabs->setCurrentIndex(3);
            QCOMPARE(tree->model()->rowCount(), 0);
            session.terminalOpen = false;
            session.clientIssue.clear();
            window.findChild<TerminalArea *>()->clientStateChanged(session);
            tabs->setCurrentIndex(1);
            QCOMPARE(tree->model()->rowCount(), 0);
            setting->setChecked(false);
            window.close();
        }
        Window reopened(data.path(), false);
        QVERIFY(!reopened.findChild<QAction *>("hideUnsavedSessions")->isChecked());
        reopened.close();
    }
    void activationChecksOnlyTheSelectedServer() {
        Fixture first, second;
        QTemporaryDir data;
        Window window(data.path(), false);
        window.show();
        window.findChild<HostDiscovery *>()->snapshot(
            {QJsonObject{{"path", first.path()}}, QJsonObject{{"path", second.path()}}}, {});
        auto *tree = window.findChild<QTreeView *>();
        QTRY_COMPARE(first.readIds.size(), 1);
        QTRY_COMPARE(second.readIds.size(), 1);
        QTRY_COMPARE(tree->model()->rowCount(), 2);
        Store store(data.path() + "/metadata.sqlite");
        QString firstId;
        for (const auto &p : store.backends())
            if (p.socketPath == first.path())
                firstId = p.id;
        QVERIFY(!firstId.isEmpty());
        QModelIndex server;
        for (int i = 0; i < tree->model()->rowCount(); ++i)
            if (tree->model()->index(i, 0).data(BackendRole) == firstId)
                server = tree->model()->index(i, 0);
        QVERIFY(server.isValid());
        auto session = tree->model()->index(0, 0, tree->model()->index(0, 0, server));
        QCOMPARE(session.data(ThreadRole).toString(), "thread-1");
        // A rejected preflight must not launch a terminal or fall back to the other server.
        first.readErrors["thread-1"] = "thread not loaded: thread-1";
        QSignalSpy terminalStates(window.findChild<TerminalArea *>(),
                                  &TerminalArea::clientStateChanged);
        bool sawError = false;
        for (auto *backend : window.findChildren<Backend *>())
            connect(backend, &Backend::sessionChanged, &window, [&](const Session &s) {
                if (!s.unavailable)
                    return;
                QTimer::singleShot(0, &window, [&] {
                    for (auto *box : window.findChildren<QMessageBox *>()) {
                        sawError = box->text().contains("thread not loaded: thread-1");
                        box->accept();
                    }
                });
            });
        tree->activated(session);
        QTRY_VERIFY(sawError);
        QCOMPARE(first.readIds.size(), 2);
        QCOMPARE(second.readIds.size(), 1);
        QCOMPARE(terminalStates.size(), 0);
        window.close();
    }
    void discoveryOpensExistingSessionsWithoutProjectSetup() {
        Fixture first, second;
        first.loaded = {"older-loaded-session"};
        QTemporaryDir data;
        Window window(data.path(), false);
        window.show();
        auto *discovery = window.findChild<HostDiscovery *>();
        auto *tree = window.findChild<QTreeView *>();
        QVERIFY(discovery && tree);
        const auto endpoint = [](const Fixture &fixture) {
            return QJsonObject{{"path", fixture.path()},
                               {"aliases", QJsonArray{}},
                               {"executable", "codex"},
                               {"hostEnvironment", true}};
        };
        const QJsonArray endpoints{endpoint(first), endpoint(second)};
        discovery->snapshot(endpoints, {});
        QTRY_COMPARE_WITH_TIMEOUT(tree->model()->rowCount(), 2, 5000);
        auto project = [&] { return tree->model()->index(0, 0); };
        QTRY_COMPARE_WITH_TIMEOUT(tree->model()->index(0, 0).data(CountRole).toInt() +
                                      tree->model()->index(1, 0).data(CountRole).toInt(),
                                  3, 5000);
        QVERIFY(first.methods.contains("thread/loaded/list"));
        QVERIFY(first.methods.contains("thread/read"));
        QVERIFY(!first.methods.contains("thread/start"));
        QVERIFY(!first.methods.contains("thread/resume"));
        // The same discovery result does not reconnect or duplicate sessions.
        const auto requests = first.methods.size();
        discovery->snapshot(endpoints, {});
        QTest::qWait(100);
        QCOMPARE(first.methods.size(), requests);
        QCOMPARE(tree->model()->rowCount(), 2);
        // A selected server searches only that server, even on a shared host.
        tree->setCurrentIndex(project());
        window.findChild<QComboBox *>()->setCurrentIndex(1);
        auto *query = window.findChild<QLineEdit *>();
        query->setText("needle");
        QTest::keyClick(query, Qt::Key_Return);
        QTRY_COMPARE(first.methods.count("thread/search") + second.methods.count("thread/search"),
                     2);
        QVERIFY(first.methods.count("thread/search") == 0 ||
                second.methods.count("thread/search") == 0);
        QTRY_COMPARE(tree->model()->rowCount(), 1);
        QTRY_COMPARE(tree->model()->index(0, 0).data(CountRole).toInt(), 2);
        window.close();
    }
};
QTEST_MAIN(WindowTests)
#include "test_window.moc"
