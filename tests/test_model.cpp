// SPDX-License-Identifier: GPL-3.0-or-later
#include "sessionmodel.h"
#include <QAbstractItemModelTester>
#include <QJsonArray>
#include <QtTest>
using namespace Archcodex;
class ModelTests : public QObject {
    Q_OBJECT
  private slots:
    void eightySessionsAndActiveFiltering() {
        SessionModel model;
        model.backendName("one", "Host one");
        model.backendName("two", "Host two");
        SessionFilter filter;
        filter.setSourceModel(&model);
        for (int i = 0; i < 80; ++i) {
            Session s = Session::fromThread(
                i < 40 ? "one" : "two", {{"id", QString::number(i)},
                                         {"cwd", "/same/path"},
                                         {"name", QString("Session %1").arg(i)},
                                         {"status", QJsonObject{{"type", i % 2 ? "idle" : "active"},
                                                                {"activeFlags", QJsonArray{}}}}});
            model.upsert(s);
        }
        QCOMPARE(filter.rowCount(), 2);
        QCOMPARE(filter.rowCount(filter.index(0, 0, filter.index(0, 0))), 40);
        filter.setView(SessionFilter::View::Active);
        QCOMPARE(filter.rowCount(), 2);
        QCOMPARE(filter.rowCount(filter.index(0, 0, filter.index(0, 0))), 40);
        filter.setView(SessionFilter::View::Running);
        QCOMPARE(filter.rowCount(filter.index(0, 0, filter.index(0, 0))), 20);
        QCOMPARE(filter.rowCount(filter.index(0, 0, filter.index(1, 0))), 20);
        model.removeBackend("one");
        QCOMPARE(filter.rowCount(), 1);
    }
    void unknownAndWaitingRemainVisible() {
        SessionModel model;
        SessionFilter filter;
        filter.setSourceModel(&model);
        filter.setView(SessionFilter::View::Active);
        auto s = Session::fromThread(
            "b", {{"id", "t"},
                  {"cwd", "/work"},
                  {"status", QJsonObject{{"type", "active"},
                                         {"activeFlags", QJsonArray{"waitingOnApproval"}}}}});
        model.upsert(s);
        QCOMPARE(filter.rowCount(), 1);
        QCOMPARE(s.stateText(), "Waiting for approval");
        s.known = false;
        model.upsert(s);
        QCOMPARE(filter.rowCount(), 1);
        QCOMPARE(model.item(0)->child(0)->child(0)->data(AttentionRole).toInt(), 0);
        s.known = true;
        s.status = {{"type", "idle"}};
        model.upsert(s);
        QCOMPARE(filter.rowCount(), 1);
        filter.setView(SessionFilter::View::Waiting);
        QCOMPARE(filter.rowCount(), 1);
        s.status = {{"type", "notLoaded"}};
        model.upsert(s);
        QCOMPARE(filter.rowCount(), 0);
        filter.setView(SessionFilter::View::Active);
        QCOMPARE(filter.rowCount(), 0);
        filter.setView(SessionFilter::View::All);
        QCOMPARE(filter.rowCount(), 1);
    }
    void predicates_data() {
        QTest::addColumn<QString>("type");
        QTest::addColumn<QStringList>("flags");
        QTest::addColumn<bool>("known");
        QTest::addColumn<bool>("active");
        QTest::addColumn<bool>("running");
        QTest::addColumn<bool>("waiting");
        QTest::newRow("saved") << "notLoaded" << QStringList{} << true << false << false << false;
        QTest::newRow("prompt") << "idle" << QStringList{} << true << true << false << true;
        QTest::newRow("running") << "active" << QStringList{} << true << true << true << false;
        QTest::newRow("approval") << "active" << QStringList{"waitingOnApproval"} << true << true
                                  << false << true;
        QTest::newRow("answer") << "active" << QStringList{"waitingOnUserInput"} << true << true
                                << false << true;
        QTest::newRow("error") << "systemError" << QStringList{} << true << true << false << false;
        QTest::newRow("future-flag")
            << "active" << QStringList{"futureActivity"} << true << true << false << false;
        QTest::newRow("disconnected-running")
            << "active" << QStringList{} << false << true << false << false;
        QTest::newRow("disconnected-prompt")
            << "idle" << QStringList{} << false << true << false << false;
        QTest::newRow("unknown") << "" << QStringList{} << false << false << false << false;
    }
    void predicates() {
        QFETCH(QString, type);
        QFETCH(QStringList, flags);
        QFETCH(bool, known);
        QFETCH(bool, active);
        QFETCH(bool, running);
        QFETCH(bool, waiting);
        auto s = Session::fromThread(
            "b", {{"id", "t"},
                  {"cwd", "/work"},
                  {"status", QJsonObject{{"type", type},
                                         {"activeFlags", QJsonArray::fromStringList(flags)}}}});
        s.known = known;
        SessionModel model;
        SessionFilter filter;
        filter.setSourceModel(&model);
        model.upsert(s);
        for (const auto [view, visible] : {std::pair{SessionFilter::View::All, true},
                                           {SessionFilter::View::Active, active},
                                           {SessionFilter::View::Running, running},
                                           {SessionFilter::View::Waiting, waiting}}) {
            filter.setView(view);
            QCOMPARE(filter.rowCount(), visible ? 1 : 0);
            if (visible)
                QCOMPARE(filter.rowCount(filter.index(0, 0, filter.index(0, 0))), 1);
        }
    }
    void identicalUpdatesDoNotDisturbTheView() {
        SessionModel model;
        const auto s = Session::fromThread(
            "b", {{"id", "t"}, {"cwd", "/work"}, {"status", QJsonObject{{"type", "idle"}}}});
        model.upsert(s);
        QSignalSpy changes(&model, &QAbstractItemModel::dataChanged);
        QSignalSpy layouts(&model, &QAbstractItemModel::layoutChanged);
        for (int i = 0; i < 80; ++i)
            model.upsert(s);
        QCOMPARE(changes.size(), 0);
        QCOMPARE(layouts.size(), 0);
    }
    void separateServerRootsAndCompactFolders() {
        SessionModel model;
        QAbstractItemModelTester tester(&model,
                                        QAbstractItemModelTester::FailureReportingMode::QtTest);
        model.backendName("a", "Main");
        model.backendName("b", "Work");
        model.backendEnvironment("a", "local-host");
        model.backendEnvironment("b", "local-host");
        auto put = [&](const QString &backend, const QString &id, const QString &cwd) {
            model.upsert(Session::fromThread(backend, {{"id", id}, {"cwd", cwd}}));
        };
        put("a", "one", "/home/user/git/team/alpha");
        put("a", "two", "/home/user/git/team/beta");
        put("a", "three", "/home/user/docs/notes");
        put("b", "one", "/home/user/git/team/alpha");
        QCOMPARE(model.rowCount(), 2);
        auto *main = model.item(0);
        QCOMPARE(main->text(), "Main");
        QCOMPARE(main->data(PathRole).toString(), "/home/user");
        QCOMPARE(main->rowCount(), 2);
        auto find = [&](const QString &path) -> QStandardItem * {
            for (int i = 0; i < main->rowCount(); ++i)
                if (main->child(i)->data(PathRole) == path)
                    return main->child(i);
            return nullptr;
        };
        auto *team = find("/home/user/git/team");
        QVERIFY(team);
        QCOMPARE(team->text(), "git/team");
        QCOMPARE(team->rowCount(), 2);
        QVERIFY(find("/home/user/docs/notes"));
        QCOMPARE(find("/home/user/docs/notes")->text(), "docs/notes");
        // A folder with its own session remains an explicit node.
        put("a", "four", "/home/user/git");
        auto *git = find("/home/user/git");
        QVERIFY(git);
        QCOMPARE(git->text(), "git");
        QCOMPARE(team->parent(), git);
        QCOMPARE(team->text(), "team");
        model.remove("a", "four");
        QCOMPARE(team->parent(), main);
        QCOMPARE(team->text(), "git/team");
        // Removing a branch recompresses ancestors without losing remaining sessions.
        model.remove("a", "three");
        QCOMPARE(main->data(PathRole).toString(), "/home/user/git/team");
        QCOMPARE(main->data(CountRole).toInt(), 2);
        QCOMPARE(model.item(1)->data(CountRole).toInt(), 1);
        QCOMPARE(model.item(1)->child(0)->child(0)->data(BackendRole).toString(), "b");
        put("a", "outside", "/home/username/work");
        QCOMPARE(main->data(PathRole).toString(), "/home");
        put("a", "unknown", "");
        QVERIFY(find(""));
        put("a", "root", "/");
        QCOMPARE(main->data(PathRole).toString(), "/");
        model.removeBackend("a");
        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(model.item(0)->text(), "Work");
    }
    void savedVisibilityAndRetainedErrors() {
        SessionModel model;
        SessionFilter filter;
        filter.setSourceModel(&model);
        auto s = Session::fromThread("b", {{"id", "t"},
                                           {"cwd", "/work"},
                                           {"ephemeral", false},
                                           {"path", QJsonValue::Null},
                                           {"status", QJsonObject{{"type", "idle"}}}});
        model.upsert(s);
        QCOMPARE(model.item(0)->data(CountRole).toInt(), 0);
        model.setHideUnsaved(false);
        QCOMPARE(model.item(0)->data(CountRole).toInt(), 1);
        model.setHideUnsaved(true);
        s.historyMode = "paginated";
        model.upsert(s);
        QCOMPARE(model.item(0)->data(CountRole).toInt(), 1);
        s.ephemeral = true;
        model.upsert(s);
        QCOMPARE(model.item(0)->data(CountRole).toInt(), 0);
        s.terminalOpen = true;
        s.clientIssue = "Terminal client exited with code 1";
        model.upsert(s);
        QCOMPARE(model.item(0)->data(CountRole).toInt(), 1);
        filter.setView(SessionFilter::View::Active);
        QCOMPARE(filter.rowCount(), 1);
        filter.setView(SessionFilter::View::Running);
        QCOMPARE(filter.rowCount(), 0);
        filter.setView(SessionFilter::View::Waiting);
        QCOMPARE(filter.rowCount(), 0);
        auto restored = Session::fromThread("b", s.metadata());
        QVERIFY(!restored.terminalOpen);
        QVERIFY(restored.clientIssue.isEmpty());
        s.terminalOpen = false;
        s.clientIssue.clear();
        model.upsert(s);
        QCOMPARE(model.item(0)->data(CountRole).toInt(), 0);
        s.ephemeral = false;
        s.rolloutPath = "/saved/file.jsonl";
        s.historyMode.clear();
        model.upsert(s);
        QCOMPARE(model.item(0)->data(CountRole).toInt(), 1);
        s.unavailable = true;
        model.upsert(s);
        QCOMPARE(model.item(0)->data(CountRole).toInt(), 0);
    }
    void manualNamesRemainAndEmptyRootsAreAllowed() {
        SessionModel model;
        model.backendName("b", "Before");
        model.project({"b", "/work", "My project"});
        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(model.item(0)->rowCount(), 0);
        model.upsert(Session::fromThread("b", {{"id", "t"}, {"cwd", "/work"}}));
        QCOMPARE(model.item(0)->child(0)->text(), "My project");
        model.backendName("b", "After");
        QCOMPARE(model.item(0)->text(), "After");
        model.remove("b", "t");
        QCOMPARE(model.item(0)->rowCount(), 0);
    }
};
QTEST_MAIN(ModelTests)
#include "test_model.moc"
