// SPDX-License-Identifier: GPL-3.0-or-later
#include "discovery.h"
#include "fixture.h"
#include "window.h"
#include <QComboBox>
#include <QLineEdit>
#include <QTreeView>
#include <QtTest>

class WindowTests : public QObject {
    Q_OBJECT
  private slots:
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
        QTRY_COMPARE_WITH_TIMEOUT(tree->model()->rowCount(), 1, 5000);
        auto project = [&] { return tree->model()->index(0, 0); };
        QTRY_COMPARE_WITH_TIMEOUT(tree->model()->rowCount(project()), 3, 5000);
        QVERIFY(first.methods.contains("thread/loaded/list"));
        QVERIFY(first.methods.contains("thread/read"));
        QVERIFY(!first.methods.contains("thread/start"));
        QVERIFY(!first.methods.contains("thread/resume"));
        // The same discovery result does not reconnect or duplicate sessions.
        const auto requests = first.methods.size();
        discovery->snapshot(endpoints, {});
        QTest::qWait(100);
        QCOMPARE(first.methods.size(), requests);
        QCOMPARE(tree->model()->rowCount(project()), 3);
        // A selected project searches both local servers sharing its environment.
        tree->setCurrentIndex(project());
        window.findChild<QComboBox *>()->setCurrentIndex(1);
        auto *query = window.findChild<QLineEdit *>();
        query->setText("needle");
        QTest::keyClick(query, Qt::Key_Return);
        QTRY_COMPARE(first.methods.count("thread/search"), 2);
        QTRY_COMPARE(second.methods.count("thread/search"), 2);
        QTRY_COMPARE(tree->model()->rowCount(project()), 4);
        window.close();
    }
};
QTEST_MAIN(WindowTests)
#include "test_window.moc"
