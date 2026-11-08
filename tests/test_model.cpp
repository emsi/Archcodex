// SPDX-License-Identifier: GPL-3.0-or-later
#include "sessionmodel.h"
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
        QCOMPARE(filter.rowCount(filter.index(0, 0)), 40);
        filter.setActiveOnly(true);
        QCOMPARE(filter.rowCount(), 2);
        QCOMPARE(filter.rowCount(filter.index(0, 0)), 20);
        QCOMPARE(filter.rowCount(filter.index(1, 0)), 20);
        model.removeBackend("one");
        QCOMPARE(filter.rowCount(), 1);
    }
    void unknownAndWaitingRemainVisible() {
        SessionModel model;
        SessionFilter filter;
        filter.setSourceModel(&model);
        filter.setActiveOnly(true);
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
        QCOMPARE(model.item(0)->child(0)->data(AttentionRole).toInt(), 0);
        s.known = true;
        s.status = {{"type", "idle"}};
        model.upsert(s);
        QCOMPARE(filter.rowCount(), 0);
        filter.setActiveOnly(false);
        QCOMPARE(filter.rowCount(), 1);
    }
    void manualNamesSurviveSessionUpdatesAndBackendRename() {
        SessionModel model;
        model.backendName("b", "Before");
        model.project({"b", "/work", "My project"});
        QCOMPARE(model.rowCount(), 0); // A saved label does not create an empty project.
        model.upsert(
            Session::fromThread("b", {{"id", "t"}, {"cwd", "/work"}, {"name", "Session"}}));
        QVERIFY(model.item(0)->text().startsWith("My project"));
        model.backendName("b", "After");
        QVERIFY(model.item(0)->text().endsWith("After"));
    }
    void sharedHostProjectsKeepTheirBackendRouting() {
        SessionModel model;
        model.backendEnvironment("one", "local-host");
        model.backendEnvironment("two", "local-host");
        auto first = Session::fromThread("one", {{"id", "same"}, {"cwd", "/work"}});
        auto second = Session::fromThread("two", {{"id", "same"}, {"cwd", "/work"}});
        model.upsert(first);
        model.upsert(second);
        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(model.item(0)->rowCount(), 2);
        QCOMPARE(model.item(0)->data(BackendsRole).toStringList().size(), 2);
        QVERIFY(model.item(0)->data(BackendRole).toString().isEmpty());
        QCOMPARE(model.item(0)->child(0)->data(BackendRole).toString(), "one");
        model.removeBackend("one");
        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(model.item(0)->child(0)->data(BackendRole).toString(), "two");
        model.remove("two", "same");
        QCOMPARE(model.rowCount(), 0);
    }
    void discoveredEnvironmentRegroupsCachedSessions() {
        SessionModel model;
        auto s = Session::fromThread("one", {{"id", "t"}, {"cwd", "/work"}});
        model.upsert(s);
        model.backendEnvironment("one", "local-host");
        model.upsert(s);
        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(model.item(0)->data(EnvironmentRole).toString(), "local-host");
        s.cwd = "/new-worktree";
        model.upsert(s);
        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(model.item(0)->data(PathRole).toString(), "/new-worktree");
    }
};
QTEST_MAIN(ModelTests)
#include "test_model.moc"
