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
    void compressionAndAggregatesFollowEachView() {
        SessionModel model;
        SessionFilter filter;
        filter.setSourceModel(&model);
        QAbstractItemModelTester modelTester(
            &model, QAbstractItemModelTester::FailureReportingMode::QtTest);
        QAbstractItemModelTester proxyTester(
            &filter, QAbstractItemModelTester::FailureReportingMode::QtTest);
        auto put = [&](const QString &id, const QString &cwd, const QString &state) {
            auto s = Session::fromThread(
                "b", {{"id", id},
                      {"cwd", cwd},
                      {"status", QJsonObject{{"type", state}, {"activeFlags", QJsonArray{}}}}});
            model.upsert(s);
            return s;
        };
        for (const auto &path : {"/home", "/home/emsi", "/home/emsi/Documents", "/var/history"})
            put(path, path, "notLoaded");
        auto waiting = put("waiting", "/home/emsi/Documents/Chats", "idle");
        auto running = put("running", "/home/emsi/Documents/Chats/work/build", "active");
        auto root = [&] { return filter.index(0, 0); };
        auto folder = [&] { return filter.index(0, 0, root()); };
        QCOMPARE(root().data(PathRole).toString(), "/");
        QCOMPARE(root().data(CountRole).toInt(), 6);
        filter.setView(SessionFilter::View::Active);
        QCOMPARE(root().data(PathRole).toString(), waiting.cwd);
        QCOMPARE(root().data(CountRole).toInt(), 2);
        bool foundCompact = false;
        for (int i = 0; i < filter.rowCount(root()); ++i) {
            const auto row = filter.index(i, 0, root());
            if (row.data(KindRole).toInt() == int(NodeKind::Folder)) {
                QCOMPARE(row.data(Qt::DisplayRole).toString(), "work/build");
                foundCompact = true;
            }
        }
        QVERIFY(foundCompact);
        filter.setView(SessionFilter::View::Running);
        QCOMPARE(folder().data(Qt::DisplayRole).toString(), "build");
        QCOMPARE(root().data(PathRole).toString(), "/home/emsi/Documents/Chats/work");
        QCOMPARE(root().data(CountRole).toInt(), 1);
        QCOMPARE(root().data(WaitingCountRole).toInt(), 0);
        QCOMPARE(root().data(AttentionRole), running.attention());
        QCOMPARE(filter.index(0, 0, folder()).data(ThreadRole).toString(), "running");
        filter.setView(SessionFilter::View::Waiting);
        QCOMPARE(folder().data(Qt::DisplayRole).toString(), "Chats");
        QCOMPARE(root().data(PathRole).toString(), "/home/emsi/Documents");
        QCOMPARE(root().data(CountRole).toInt(), 1);
        QCOMPARE(root().data(RunningCountRole).toInt(), 0);
        QCOMPARE(root().data(AttentionRole), waiting.attention());
        const auto leaf = QPersistentModelIndex(filter.index(0, 0, folder()));
        QSignalSpy topology(&model, &SessionModel::topologyChanged);
        for (int i = 0; i < 80; ++i)
            model.upsert(waiting);
        QCOMPARE(topology.size(), 0);
        QVERIFY(leaf.isValid());
        // A pushed status change adds/removes a branch and recomputes the common root.
        running.status = {{"type", "idle"}};
        running.cwd = "/srv/work";
        model.upsert(running);
        QCOMPARE(root().data(PathRole).toString(), "/");
        QCOMPARE(root().data(CountRole).toInt(), 2);
        QSet<QString> labels;
        for (int i = 0; i < filter.rowCount(root()); ++i)
            labels.insert(filter.index(i, 0, root()).data(Qt::DisplayRole).toString());
        QCOMPARE(labels, (QSet<QString>{"home/emsi/Documents/Chats", "srv/work"}));
        running.status = {{"type", "active"}};
        model.upsert(running);
        QCOMPARE(folder().data(Qt::DisplayRole).toString(), "Chats");
        QCOMPARE(root().data(CountRole).toInt(), 1);
        filter.setView(SessionFilter::View::All);
        QCOMPARE(root().data(CountRole).toInt(), 6); // Hidden history was never discarded.
        QCOMPARE(root().data(PathRole).toString(), "/");
    }
    void waitingIconsAreGreenAndHistoryRemainsNeutral() {
        SessionModel model;
        auto s = Session::fromThread(
            "b", {{"id", "t"}, {"cwd", "/work"}, {"status", QJsonObject{{"type", "idle"}}}});
        const auto color = [](const QStandardItem *item) {
            return item->icon().pixmap(16, 16).toImage().pixelColor(8, 8);
        };
        model.upsert(s);
        const auto idle = color(model.item(0)->child(0)->child(0));
        QVERIFY(idle.green() > idle.red() && idle.green() > idle.blue());
        QCOMPARE(color(model.item(0)), idle);
        s.status = {{"type", "active"}, {"activeFlags", QJsonArray{"waitingOnApproval"}}};
        model.upsert(s);
        QCOMPARE(color(model.item(0)), idle);
        s.status = {{"type", "active"}, {"activeFlags", QJsonArray{"waitingOnUserInput"}}};
        model.upsert(s);
        QCOMPARE(color(model.item(0)), idle);
        s.status = {{"type", "active"}};
        model.upsert(s);
        const auto running = color(model.item(0));
        QVERIFY(running.blue() > running.green() && running.blue() > running.red());
        s.status = {{"type", "notLoaded"}};
        model.upsert(s);
        QVERIFY(color(model.item(0)) != idle);
        s.known = false;
        model.upsert(s);
        QCOMPARE(color(model.item(0)).alpha(), 0);
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
    void statusIconsKeepTheirColoursAcrossSelectionAndFocus() {
        SessionModel model;
        auto s = Session::fromThread(
            "b", {{"id", "fork"}, {"cwd", "/work"}, {"status", QJsonObject{{"type", "idle"}}}});
        for (const auto type : {"idle", "active", "notLoaded", "systemError"}) {
            s.status = {{"type", type}};
            model.upsert(s);
            for (const auto *item :
                 {model.item(0), model.item(0)->child(0), model.item(0)->child(0)->child(0)}) {
                const auto icon = item->icon();
                const auto expected = icon.pixmap(16, 16, QIcon::Normal).toImage();
                for (const auto mode : {QIcon::Selected, QIcon::Active, QIcon::Disabled})
                    for (const auto state : {QIcon::Off, QIcon::On})
                        QCOMPARE(icon.pixmap(16, 16, mode, state).toImage(), expected);
            }
        }
    }
    void sameTitleForksRemainIndependentWhenOneClientFails() {
        SessionModel model;
        auto first = Session::fromThread("b", {{"id", "fork-1"},
                                               {"name", "Shared ancestor title"},
                                               {"cwd", "/work"},
                                               {"path", "/saved/first.jsonl"},
                                               {"status", QJsonObject{{"type", "idle"}}}});
        auto second = first;
        second.id = "fork-2";
        model.upsertMany({first, second});
        first.terminalOpen = true;
        first.clientIssue = "Terminal client exited with code 1";
        model.upsert(first);
        for (const auto view : {SessionView::All, SessionView::Active}) {
            model.setView(view);
            QCOMPARE(model.item(0)->data(CountRole).toInt(), 2);
            QSet<QString> ids;
            const auto *folder = model.item(0)->child(0);
            for (int i = 0; i < folder->rowCount(); ++i) {
                const auto *row = folder->child(i);
                QCOMPARE(row->text(), first.title);
                ids.insert(row->data(ThreadRole).toString());
            }
            QCOMPARE(ids, (QSet<QString>{first.id, second.id}));
        }
        model.setView(SessionView::Waiting);
        QCOMPARE(model.item(0)->data(CountRole).toInt(), 1);
        QCOMPARE(model.item(0)->child(0)->child(0)->data(ThreadRole).toString(), second.id);
        first.terminalOpen = false;
        first.clientIssue.clear();
        model.upsert(first);
        QCOMPARE(model.item(0)->data(CountRole).toInt(), 2);
        QCOMPARE(model.item(0)->data(WaitingCountRole).toInt(), 2);
    }
    void containerWorkdirScopeIsPerBackendAndPreservesHiddenHistory() {
        SessionModel model;
        model.backendName("container", "myCodex");
        model.backendName("other", "Remote myCodex");
        model.backendName("host", "Host");
        model.setContainerWorkdir("container", "/workspace/project/");
        model.setContainerWorkdir("other", "/elsewhere");
        const QStringList paths{"/workspace/project",
                                "/workspace/project/sub",
                                "/workspace/project-other",
                                "/workspace/project/../other",
                                "/elsewhere",
                                "relative"};
        for (const auto &backend : {"container", "other", "host"})
            for (qsizetype i = 0; i < paths.size(); ++i)
                model.upsert(
                    Session::fromThread(backend, {{"id", QString::number(i)},
                                                  {"cwd", paths[i]},
                                                  {"path", "/saved/file"},
                                                  {"status", QJsonObject{{"type", "idle"}}}}));
        const auto count = [&](const QString &backend) {
            for (int i = 0; i < model.rowCount(); ++i)
                if (model.item(i)->data(BackendRole) == backend)
                    return model.item(i)->data(CountRole).toInt();
            return -1;
        };
        for (const auto view : {SessionView::All, SessionView::Active, SessionView::Waiting}) {
            model.setView(view);
            QCOMPARE(count("container"), 2);
            QCOMPARE(count("other"), 1);
            QCOMPARE(count("host"), 6);
        }
        model.setMyCodexWorkdirOnly(false);
        QCOMPARE(count("container"), 6);
        QCOMPARE(count("other"), 6);
        model.setMyCodexWorkdirOnly(true);
        QCOMPARE(count("container"), 2);
        model.setContainerWorkdir("container", "/elsewhere");
        QCOMPARE(count("container"), 1);
        model.setContainerWorkdir("container", ""); // Older cached profiles need rediscovery.
        QCOMPARE(count("container"), 0);
        model.setContainerWorkdir("container", "/");
        QCOMPARE(count("container"), 5); // Root covers absolute paths, not unknown/relative paths.
        model.setView(SessionView::All);
        model.resetResults(); // Search clears records but must keep backend scopes.
        model.upsert(
            Session::fromThread("other", {{"id", "search"}, {"cwd", "/workspace/project"}}));
        QCOMPARE(count("other"), 0);
        model.setMyCodexWorkdirOnly(false);
        QCOMPARE(count("other"), 1);
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
