// SPDX-License-Identifier: GPL-3.0-or-later
#include "asyncstore.h"
#include "opening.h"
#include "store.h"
#include "worker.h"
#include <QJsonDocument>
#include <QSignalSpy>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>
using namespace Archcodex;
namespace {
BackendProfile profile(const QString &id = "local", const QString &host = {}) {
    BackendProfile p{id, id};
    p.socketPath = "/run/user/1000/codex.sock";
    p.sshHost = host;
    p.environmentId = "environment";
    p.identity = {{"codexHome", "/home/test/.codex"}};
    return p;
}
WorkspaceView view(const BackendProfile &p, const QString &id = "thread") {
    WorkspaceView v;
    v.id = "view-" + p.id + "-" + id;
    v.backendId = p.id;
    v.threadId = id;
    v.title = "Saved conversation";
    v.cwd = "/work/project";
    v.sourceIdentity = workspaceSourceIdentity(p);
    return v;
}
Session session(const BackendProfile &p, const QString &id = "thread", bool saved = true) {
    return Session::fromThread(p.id,
                               {{"id", id},
                                {"cwd", "/work/project"},
                                {"name", "Renamed conversation"},
                                {"path", saved ? QJsonValue("/saved/" + id) : QJsonValue::Null}});
}
WorkspaceSnapshot snapshot(const BackendProfile &p = profile()) {
    auto v = view(p);
    return {1, {v}, {v.id}, v.id};
}
class FakeBackend : public Backend {
  public:
    using Backend::Backend;
    bool connected = false;
    struct Read {
        QString id;
        quint64 token;
    };
    QList<Read> reads;
    int count = 0;
    void connectServer() override {
        connected = true;
        emit identityReceived(profile.identity);
        emit connectionChanged("Ready", true);
    }
    void disconnectServer() override {
        connected = false;
        emit connectionChanged("Disconnected", false);
    }
    bool ready() const override {
        return connected;
    }
    void loadMore() override {
        QFAIL("Restoration must not page through history");
    }
    void search(const QString &, quint64) override {
        QFAIL("Restoration must not search history");
    }
    void forkSession(const QString &, const QString &) override {
        QFAIL("Restoration must not fork");
    }
    void verifySession(const QString &id, quint64 token) override {
        reads.append({id, token});
        ++count;
    }
    void finish(const QString &error = {}, bool saved = true, bool observed = false) {
        const auto read = reads.takeFirst();
        auto s = session(profile, read.id, saved);
        s.executionObserved = observed;
        s.executionEnvironment = profile.environmentId;
        emit sessionVerified(read.id, error, read.token, s);
    }
};
void sql(const QString &path, const QString &statement) {
    const auto name = QUuid::createUuid().toString();
    {
        auto db = QSqlDatabase::addDatabase("QSQLITE", name);
        db.setDatabaseName(path);
        QVERIFY(db.open());
        QSqlQuery q(db);
        QVERIFY2(q.exec(statement), qPrintable(statement));
    }
    QSqlDatabase::removeDatabase(name);
}
} // namespace
class WorkspaceTests : public QObject {
    Q_OBJECT
  private slots:
    void snapshotValidationAndIdentity() {
        auto s = snapshot();
        QVERIFY(s.validate().isEmpty());
        QCOMPARE(WorkspaceSnapshot::fromJson(s.json()).json(), s.json());
        auto json = s.json();
        json["version"] = 2;
        QVERIFY_EXCEPTION_THROWN(WorkspaceSnapshot::fromJson(json), std::runtime_error);
        json = s.json();
        json["revision"] = 1.5;
        QVERIFY_EXCEPTION_THROWN(WorkspaceSnapshot::fromJson(json), std::runtime_error);
        s.recent << s.recent.first();
        QVERIFY(!s.validate().isEmpty());
        s = snapshot();
        s.views << s.views.first();
        s.recent << s.recent.first();
        QVERIFY(!s.validate().isEmpty());
        s = snapshot();
        s.views[0].cwd = "relative";
        QVERIFY(!s.validate().isEmpty());
        auto p = profile();
        const auto identity = workspaceSourceIdentity(p);
        p.name = "New label";
        p.relaySocket = "/transient/new";
        p.containerEpoch = "new-epoch";
        QCOMPARE(workspaceSourceIdentity(p), identity);
        p.identity["codexHome"] = "/different/home";
        QVERIFY(workspaceSourceIdentity(p) != identity);
        p = profile();
        p.kind = "mycodex";
        p.containerId = "a";
        const auto container = workspaceSourceIdentity(p);
        p.containerId = "b";
        QVERIFY(workspaceSourceIdentity(p) != container);
    }
    void migrationAtomicSnapshotAndCacheIndependence() {
        QTemporaryDir d;
        const auto path = d.filePath("metadata.sqlite");
        sql(path, "PRAGMA user_version=1");
        auto s = snapshot();
        {
            Store db(path);
            db.save(profile());
            db.save(session(profile()));
            db.save(s);
            db.removeSession("local", "thread");
            QCOMPARE(db.workspace().json(), s.json());
            db.removeBackend("local");
            QCOMPARE(db.workspace().json(), s.json());
            auto stale = s;
            stale.selected.clear();
            db.save(stale);
            QCOMPARE(db.workspace().json(), s.json());
            s.revision = 2;
            s.views.clear();
            s.recent.clear();
            s.selected.clear();
            db.save(s);
        }
        Store reopened(path);
        QCOMPARE(reopened.workspace().json(), s.json());
    }
    void unreadableSnapshotIsNeverOverwritten_data() {
        QTest::addColumn<QString>("data");
        QTest::newRow("corrupt") << QString("not-json");
        auto future = snapshot().json();
        future["version"] = 900;
        QTest::newRow("future") << QString::fromUtf8(
            QJsonDocument(future).toJson(QJsonDocument::Compact));
        auto wrongRevision = snapshot().json();
        wrongRevision["revision"] = 9;
        QTest::newRow("mismatched-revision")
            << QString::fromUtf8(QJsonDocument(wrongRevision).toJson(QJsonDocument::Compact));
    }
    void unreadableSnapshotIsNeverOverwritten() {
        QFETCH(QString, data);
        QTemporaryDir d;
        const auto path = d.filePath("metadata.sqlite");
        {
            Store db(path);
            db.save(snapshot());
        }
        sql(path, "UPDATE workspace SET data='" + data.replace("'", "''") + "'");
        {
            Store db(path);
            QVERIFY_EXCEPTION_THROWN(db.workspace(), std::runtime_error);
            auto empty = WorkspaceSnapshot{};
            empty.revision = 20;
            QVERIFY_EXCEPTION_THROWN(db.save(empty), std::runtime_error);
            db.save(profile()); // Independent cache remains usable.
            QCOMPARE(db.backends().size(), 1);
        }
        {
            AsyncStore db(path);
            QSignalSpy loaded(&db, &AsyncStore::loaded);
            QTRY_COMPARE(loaded.size(), 1);
            QVERIFY(!qvariant_cast<StoreSnapshot>(loaded.first()[0]).workspaceError.isEmpty());
        }
        QVERIFY(Worker::finishPending());
        Store reopened(path);
        QVERIFY_EXCEPTION_THROWN(reopened.workspace(), std::runtime_error);
    }
    void coalescedWriterFlushesLastIntent() {
        QTemporaryDir d;
        const auto path = d.filePath("metadata.sqlite");
        auto s = snapshot();
        {
            AsyncStore db(path);
            QSignalSpy loaded(&db, &AsyncStore::loaded), failed(&db, &AsyncStore::failed);
            QTRY_COMPARE(loaded.size(), 1);
            for (int i = 1; i <= 200; ++i) {
                s.revision = i;
                s.views[0].title = QString::number(i);
                db.save(s);
            }
            QCOMPARE(failed.size(), 0);
        }
        QVERIFY(Worker::finishPending());
        Store db(path);
        QCOMPARE(db.workspace().json(), s.json());
    }
    void offlineAndDuplicateRequests() {
        auto p = profile();
        FakeBackend b(p);
        OpeningCoordinator c;
        c.setSource(p, &b);
        QSignalSpy launched(&c, &OpeningCoordinator::launchRequested);
        const auto v = view(p);
        c.request(v, true);
        c.request(v, true);
        c.request(v, false);
        QTest::qWait(20);
        QCOMPARE(b.count, 0);
        QCOMPARE(c.pendingCount(), 1);
        b.connectServer();
        QTRY_COMPARE(b.reads.size(), 1);
        QCOMPARE(b.reads.first().id, v.threadId);
        b.finish();
        QTRY_COMPARE(launched.size(), 1);
        QCOMPARE(qvariant_cast<Session>(launched.first()[2]).title, "Renamed conversation");
        QCOMPARE(c.pendingCount(), 0);
        QTest::qWait(20);
        QCOMPARE(b.count, 1);
    }
    void cancellationAndReconnectIgnoreLateResults() {
        auto p = profile();
        FakeBackend b(p);
        OpeningCoordinator c;
        c.setSource(p, &b);
        b.connectServer();
        QSignalSpy launched(&c, &OpeningCoordinator::launchRequested);
        auto v = view(p);
        c.request(v, true);
        QTRY_COMPARE(b.reads.size(), 1);
        c.cancel(v.id);
        c.request(v, false);
        QTRY_COMPARE(b.reads.size(), 2);
        b.finish();
        QTest::qWait(20);
        QCOMPARE(launched.size(), 0);
        b.disconnectServer();
        b.connectServer();
        QTRY_COMPARE(b.reads.size(), 2);
        b.finish();
        QTest::qWait(20);
        QCOMPARE(launched.size(), 0);
        b.finish();
        QTRY_COMPARE(launched.size(), 1);
        FakeBackend replacement(p);
        c.request(v, true);
        QTRY_COMPARE(b.reads.size(), 1);
        c.setSource(p, &replacement);
        replacement.connectServer();
        QTRY_COMPARE(replacement.reads.size(), 1);
        b.finish();
        QTest::qWait(20);
        QCOMPARE(launched.size(), 1);
        replacement.finish();
        QTRY_COMPARE(launched.size(), 2);
    }
    void eightyViewsBoundedAndLocalNotStarved() {
        FakeBackend a(profile("a", "ssh-a")), b(profile("b", "ssh-b")), local(profile());
        OpeningCoordinator c;
        for (auto *backend : {&a, &b, &local}) {
            c.setSource(backend->profile, backend);
            backend->connectServer();
        }
        QSignalSpy launched(&c, &OpeningCoordinator::launchRequested);
        for (int i = 0; i < 80; ++i) {
            const auto &p = i < 40 ? a.profile : i < 60 ? b.profile : local.profile;
            c.request(view(p, QString::number(i)), true);
        }
        QTRY_COMPARE(a.reads.size() + b.reads.size() + local.reads.size(), 4);
        QVERIFY(!local.reads.isEmpty());
        QCOMPARE(a.reads.size(), 2);
        while (launched.size() < 80) {
            QVERIFY(a.reads.size() <= 2 && b.reads.size() <= 2 && local.reads.size() <= 2);
            QVERIFY(a.reads.size() + b.reads.size() + local.reads.size() <= 4);
            for (auto *backend : {&a, &b, &local})
                if (!backend->reads.isEmpty())
                    backend->finish();
            QTest::qWait(1);
        }
        QCOMPARE(a.count + b.count + local.count, 80);
        QCOMPARE(c.pendingCount(), 0);
    }
    void cancelledRequestsStillConsumeSlots() {
        FakeBackend b(profile());
        OpeningCoordinator c;
        c.setSource(b.profile, &b);
        b.connectServer();
        for (int i = 0; i < 10; ++i) {
            auto v = view(b.profile, QString::number(i));
            c.request(v, false);
            QTest::qWait(1);
            c.cancel(v.id);
        }
        QCOMPARE(b.reads.size(), 2);
        c.request(view(b.profile, "last"), false);
        QTest::qWait(10);
        QCOMPARE(b.count, 2);
        b.finish();
        QTRY_COMPARE(b.count, 3);
    }
    void failureRequiresExplicitRetry() {
        FakeBackend b(profile());
        OpeningCoordinator c;
        c.setSource(b.profile, &b);
        b.connectServer();
        QSignalSpy state(&c, &OpeningCoordinator::stateChanged),
            launched(&c, &OpeningCoordinator::launchRequested);
        auto v = view(b.profile);
        c.request(v, true);
        QTRY_COMPARE(b.reads.size(), 1);
        b.finish("not found");
        QCOMPARE(c.pendingCount(), 0);
        QVERIFY(state.last()[2].toBool());
        b.disconnectServer();
        b.connectServer();
        QTest::qWait(10);
        QCOMPARE(b.count, 1);
        c.request(v, false);
        QTRY_COMPARE(b.reads.size(), 1);
        b.finish();
        QTRY_COMPARE(launched.size(), 1);
        c.request(v, true);
        QTRY_COMPARE(b.reads.size(), 1);
        b.finish({}, false);
        QTest::qWait(10);
        QCOMPARE(launched.size(), 1);
        QCOMPARE(c.pendingCount(), 0);
        v.threadId.clear();
        v.disposition = ViewDisposition::Unresolved;
        c.request(v, false);
        QTRY_COMPARE(c.pendingCount(), 0);
        QCOMPARE(b.count, 3);
    }
    void identityAndContainerEvidence() {
        auto p = profile();
        p.kind = "mycodex";
        p.containerId = "container-a";
        FakeBackend b(p);
        OpeningCoordinator c;
        c.setSource(p, &b);
        b.connectServer();
        QSignalSpy launched(&c, &OpeningCoordinator::launchRequested);
        auto v = view(p);
        v.executionObserved = true;
        v.executionEnvironment = p.environmentId;
        c.request(v, true);
        QTRY_COMPARE(b.reads.size(), 1);
        b.finish();
        QTRY_COMPARE(launched.size(), 1); // Saved same-context evidence.
        v.executionObserved = false;
        c.request(v, true);
        QTRY_COMPARE(b.reads.size(), 1);
        b.finish();
        QTest::qWait(10);
        QCOMPARE(launched.size(), 1);
        QCOMPARE(c.pendingCount(), 1); // Inventory in progress, not arbitrary timeout.
        b.loadedInventoryFailed();
        QTRY_COMPARE(c.pendingCount(), 0);
        c.request(v, false);
        QTRY_COMPARE(b.reads.size(), 1);
        b.finish({}, true, true);
        QTRY_COMPARE(launched.size(), 2);
        p.environmentId = "changed-mounts";
        c.updateProfile(p);
        c.request(v, false);
        QTRY_COMPARE(c.pendingCount(), 0);
        QCOMPARE(b.count, 3); // No attempt against changed execution environment.
    }
};
QTEST_GUILESS_MAIN(WorkspaceTests)
#include "test_workspace.moc"
