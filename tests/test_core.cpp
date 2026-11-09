// SPDX-License-Identifier: GPL-3.0-or-later
#include "backend.h"
#include "discovery.h"
#include "fixture.h"
#include "store.h"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QWebSocket>
#include <QWebSocketServer>
#include <QtTest>

using namespace Archcodex;

class CoreTests : public QObject {
    Q_OBJECT
  private slots:
    void validationAndArgumentIsolation() {
        BackendProfile p{"id", "name", "local-unix", "/tmp/socket with spaces", "/bin/echo", false};
        QVERIFY(p.validate().isEmpty());
        p.socketPath = "relative";
        QVERIFY(!p.validate().isEmpty());
        p.socketPath = "/tmp/socket";
        p.executable = "codex --unsafe";
        QVERIFY(!p.validate().isEmpty());
        p.executable = "/bin/echo";
        QTemporaryDir d;
        const auto marker = d.path() + "/INJECTED";
        const auto id = "$(touch " + marker + "); --help";
        auto args = terminalArguments(p, id);
        QCOMPARE(args.last(), id);
        QCOMPARE(args.at(args.size() - 2), "--");
        args.removeFirst();
        QProcess process;
        process.start("/bin/sh", args);
        QVERIFY(process.waitForFinished());
        QVERIFY(QString::fromUtf8(process.readAllStandardOutput()).contains(id));
        QVERIFY(!QFile::exists(marker));
    }
    void metadataPersistenceAndCascade() {
        QTemporaryDir d;
        const auto db = d.path() + "/state.sqlite";
        {
            Store s(db);
            BackendProfile p{"b", "Local"};
            s.save(p);
            s.save(Project{"b", "/work", "Project"});
            auto thread = QJsonObject{
                {"id", "t"},
                {"cwd", "/work"},
                {"name", "Title"},
                {"status", QJsonObject{{"type", "active"}, {"activeFlags", QJsonArray{}}}}};
            auto session = Session::fromThread("b", thread);
            session.opened = 456;
            s.save(session);
        }
        {
            Store s(db);
            QCOMPARE(s.backends().size(), 1);
            QCOMPARE(s.projects().size(), 1);
            QCOMPARE(s.sessions().size(), 1);
            QVERIFY(!s.sessions()[0].known);
            QVERIFY(s.sessions()[0].isActive());
            QCOMPARE(s.sessions()[0].opened, 456);
            s.removeBackend("b");
            QVERIFY(s.projects().isEmpty());
            QVERIFY(s.sessions().isEmpty());
        }
        QVERIFY(!(QFile::permissions(db) & QFile::ReadOther));
    }
    void humanReadableServerIdentity() {
        BackendProfile p{"b", "Custom name"};
        p.identity = {{"user", "alice"},
                      {"host", "workstation"},
                      {"pid", 12345},
                      {"ownerHome", "/home/alice"},
                      {"codexHome", "/home/alice/.codex"},
                      {"cwd", "/home/alice/git/project"}};
        QCOMPARE(serverLabel(p), "Custom name");
        QCOMPARE(serverTreeLabel(p), "Custom name");
        p.discovered = true;
        QCOMPARE(serverLabel(p), "Codex · project · alice@workstation");
        p.identity["default"] = true;
        QCOMPARE(serverLabel(p), "Main · alice@workstation");
        QCOMPARE(serverTreeLabel(p), "Main");
        p.identity["default"] = false;
        p.identity["codexHome"] = "/home/alice/.codex-work";
        QCOMPARE(serverLabel(p), "~/.codex-work · alice@workstation");
        QCOMPARE(serverTreeLabel(p), "~/.codex-work");
        QVERIFY(!serverLabel(p).contains("12345"));
        QVERIFY(serverDetails(p).contains("12345"));
        QCOMPARE(BackendProfile::fromJson(p.json()).identity, p.identity);
        p.kind = "mycodex";
        p.identity["containerName"] = "example-project";
        p.identity["stateMode"] = "Shared";
        QCOMPARE(serverTreeLabel(p), "example-project");
        QCOMPARE(serverLabel(p), "myCodex · example-project · This computer · Shared");
    }
    void inventoryValidationAndOpenPreflight() {
        Fixture f;
        f.readErrors["thread-1"] = "thread not loaded: thread-1";
        LocalBackend b(f.profile(), BRIDGE_PATH);
        QSignalSpy updates(&b, &Backend::sessionChanged);
        QSignalSpy verified(&b, &Backend::sessionVerified);
        QSignalSpy identity(&b, &Backend::identityReceived);
        b.connectServer();
        QTRY_VERIFY(!updates.isEmpty() && qvariant_cast<Session>(updates.last()[0]).unavailable);
        QCOMPARE(identity.size(), 1);
        QCOMPARE(identity[0][0].toJsonObject()["codexHome"].toString(), "/home/test/.codex-work");
        const auto count = f.methods.size();
        QTest::qWait(100);
        QCOMPARE(f.methods.size(), count);
        f.readErrors.clear();
        b.verifySession("thread-1");
        QTRY_COMPARE(verified.size(), 1);
        QVERIFY(verified.last()[1].toString().isEmpty());
        QVERIFY(qvariant_cast<Session>(updates.last()[0]).isSaved());
        f.readErrors["thread-1"] = "Permission denied";
        b.verifySession("thread-1");
        QTRY_COMPARE(verified.size(), 2);
        QCOMPARE(verified.last()[1].toString(), "Permission denied");
        QVERIFY(!qvariant_cast<Session>(updates.last()[0]).unavailable);
        b.disconnectServer();
    }
    void persistenceEventDuringReadIsNotLost() {
        Fixture f;
        f.delayRead = true;
        f.threadFields = {{"path", QJsonValue::Null}, {"ephemeral", false}};
        LocalBackend b(f.profile(), BRIDGE_PATH);
        Session latest;
        connect(&b, &Backend::sessionChanged, this, [&](const Session &s) { latest = s; });
        b.connectServer();
        QTRY_VERIFY(!f.readId.isUndefined());
        auto oldMetadata = f.thread();
        f.notify("thread/status/changed",
                 {{"threadId", "thread-1"},
                  {"status", QJsonObject{{"type", "active"}, {"activeFlags", QJsonArray{}}}}});
        QTRY_VERIFY(latest.isRunning());
        f.delayRead = false;
        f.threadFields = {};
        f.reply(f.readId, {{"thread", oldMetadata}});
        QTRY_VERIFY(latest.isSaved());
        QCOMPARE(f.readIds.size(), 2);
        const auto count = f.methods.size();
        QTest::qWait(100);
        QCOMPARE(f.methods.size(), count);
        b.disconnectServer();
    }
    void pushedEventsAndNoPolling() {
        Fixture f;
        LocalBackend b(f.profile(), BRIDGE_PATH);
        QSignalSpy updates(&b, &Backend::sessionChanged);
        b.connectServer();
        QTRY_VERIFY_WITH_TIMEOUT(b.ready() && !updates.isEmpty(), 5000);
        QCOMPARE(qvariant_cast<Session>(updates.last().at(0)).stateText(),
                 "Waiting for input — ready for a prompt");
        f.notify("thread/status/changed",
                 {{"threadId", "thread-1"},
                  {"status", QJsonObject{{"type", "active"},
                                         {"activeFlags", QJsonArray{"waitingOnUserInput"}}}}});
        QTRY_COMPARE(qvariant_cast<Session>(updates.last().at(0)).stateText(), "Waiting for input");
        const auto count = f.methods.size();
        QTest::qWait(1200);
        QCOMPARE(f.methods.size(), count);
        QVERIFY(!f.methods.contains("thread/resume"));
        QVERIFY(!f.methods.contains("turn/start"));
        b.disconnectServer();
        QVERIFY(!b.ready());
        QVERIFY(!qvariant_cast<Session>(updates.last().at(0)).known);
    }
    void eventWinsOverOlderSnapshot() {
        Fixture f;
        f.delaySnapshot = true;
        LocalBackend b(f.profile(), BRIDGE_PATH);
        QSignalSpy updates(&b, &Backend::sessionChanged);
        b.connectServer();
        QTRY_VERIFY_WITH_TIMEOUT(!f.snapshotId.isUndefined(), 5000);
        f.notify("thread/status/changed",
                 {{"threadId", "thread-1"},
                  {"status", QJsonObject{{"type", "active"}, {"activeFlags", QJsonArray{}}}}});
        f.reply(f.snapshotId, {{"data", QJsonArray{f.thread()}}});
        QTRY_VERIFY(!updates.isEmpty());
        QTest::qWait(100);
        QVERIFY(qvariant_cast<Session>(updates.last().at(0)).isActive());
        b.disconnectServer();
    }
    void closedThreadAndRestartOverrideOldSnapshots() {
        Fixture f;
        f.delaySnapshot = true;
        LocalBackend b(f.profile(), BRIDGE_PATH);
        QSignalSpy updates(&b, &Backend::sessionChanged);
        b.connectServer();
        QTRY_VERIFY(!f.snapshotId.isUndefined());
        f.notify("thread/closed", {{"threadId", "thread-1"}});
        f.reply(f.snapshotId, {{"data", QJsonArray{f.thread()}}});
        QTRY_VERIFY(!updates.isEmpty());
        QTest::qWait(100);
        QVERIFY(!qvariant_cast<Session>(updates.last().at(0)).isActive());
        f.notify("thread/started", {{"thread", f.thread()}});
        QTRY_VERIFY(qvariant_cast<Session>(updates.last().at(0)).isWaiting());
        b.disconnectServer();
    }
    void removedThreadDoesNotReappearFromSnapshot() {
        Fixture f;
        f.delaySnapshot = true;
        LocalBackend b(f.profile(), BRIDGE_PATH);
        QSignalSpy updates(&b, &Backend::sessionChanged);
        b.connectServer();
        QTRY_VERIFY_WITH_TIMEOUT(!f.snapshotId.isUndefined(), 5000);
        f.notify("thread/archived", {{"threadId", "thread-1"}});
        f.reply(f.snapshotId, {{"data", QJsonArray{f.thread()}}});
        QTest::qWait(150);
        QVERIFY(updates.isEmpty());
        b.disconnectServer();
    }
    void fullTextPagination() {
        Fixture f;
        LocalBackend b(f.profile(), BRIDGE_PATH);
        QSignalSpy results(&b, &Backend::searchResults);
        b.connectServer();
        QTRY_VERIFY_WITH_TIMEOUT(b.ready(), 5000);
        b.search("needle", 42);
        QTRY_COMPARE(results.size(), 2);
        QCOMPARE(results[0][0].toULongLong(), 42);
        QCOMPARE(results[1][3].toBool(), true);
        QCOMPARE(f.methods.count("thread/search"), 2);
        b.disconnectServer();
    }
    void reconnectReconcilesOldLiveRows_data() {
        QTest::addColumn<bool>("inventoryFails");
        QTest::newRow("complete") << false;
        QTest::newRow("unavailable") << true;
    }
    void reconnectReconcilesOldLiveRows() {
        QFETCH(bool, inventoryFails);
        Fixture f;
        f.loaded = {"old-thread"};
        LocalBackend b(f.profile(), BRIDGE_PATH);
        Session old;
        connect(&b, &Backend::sessionChanged, this, [&](const Session &s) {
            if (s.id == "old-thread")
                old = s;
        });
        b.connectServer();
        QTRY_VERIFY(old.known && old.isWaiting());
        b.disconnectServer();
        QVERIFY(!old.known);
        QVERIFY(old.isActive());
        f.loaded.clear();
        f.failLoadedInventory = inventoryFails;
        b.connectServer();
        QTRY_COMPARE(f.methods.count("thread/loaded/list"), 2);
        if (inventoryFails) {
            QTest::qWait(100);
            QVERIFY(!old.known);
            QVERIFY(old.isActive());
        } else {
            QTRY_VERIFY(old.known);
            QVERIFY(!old.isActive());
        }
        b.disconnectServer();
    }
    void disconnectMarksUnknown() {
        Fixture f;
        LocalBackend b(f.profile(), BRIDGE_PATH);
        QSignalSpy updates(&b, &Backend::sessionChanged);
        b.connectServer();
        QTRY_VERIFY_WITH_TIMEOUT(!updates.isEmpty(), 5000);
        f.peer->close();
        QTRY_VERIFY(!b.ready());
        QVERIFY(!qvariant_cast<Session>(updates.last().at(0)).known);
        b.disconnectServer();
    }
    void discoveryStructuredReport() {
        if (QFile::exists("/.flatpak-info"))
            QSKIP("Host-command access is an explicit host acceptance test outside the build "
                  "sandbox.");
        HostDiscovery discovery;
        QSignalSpy snapshots(&discovery, &HostDiscovery::snapshot);
        QSignalSpy failures(&discovery, &HostDiscovery::failed);
        discovery.refresh();
        QTRY_VERIFY_WITH_TIMEOUT(!snapshots.isEmpty() || !failures.isEmpty(), 5000);
        QVERIFY2(failures.isEmpty(),
                 failures.isEmpty() ? "" : qPrintable(failures.last()[0].toString()));
        for (const auto &value : snapshots.last()[1].toJsonArray()) {
            const auto issue = value.toObject();
            QVERIFY(!issue["code"].toString().isEmpty());
            QVERIFY(!issue["summary"].toString().isEmpty());
            QVERIFY(issue["details"].isArray());
        }
        discovery.stop();
    }
    void isolatedRealCodexIfRequested() {
        const auto executable = qEnvironmentVariable("ARCHCODEX_TEST_CODEX");
        if (executable.isEmpty())
            QSKIP("Set ARCHCODEX_TEST_CODEX to test an isolated real app-server.");
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QProcess server;
        QProcessEnvironment env;
        env.insert("PATH", "/usr/bin:/bin");
        env.insert("LANG", "C.UTF-8");
        env.insert("CODEX_HOME", directory.path() + "/codex-home");
        QDir().mkpath(directory.path() + "/codex-home");
        server.setProcessEnvironment(env);
        const auto socket = directory.path() + "/server.sock";
        server.start(executable, {"app-server", "--listen", "unix://" + socket, "-c",
                                  "analytics.enabled=false"});
        QVERIFY(server.waitForStarted());
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(socket), 10000);
        HostDiscovery discovery;
        QSignalSpy snapshots(&discovery, &HostDiscovery::snapshot);
        discovery.refresh({socket});
        auto found = [&] {
            for (const auto &snapshot : snapshots)
                for (const auto &endpoint : snapshot[0].value<QJsonArray>())
                    if (endpoint.toObject()["aliases"].toArray().contains(socket))
                        return true;
            return false;
        };
        QTRY_VERIFY_WITH_TIMEOUT(found(), 10000);
        discovery.stop();
        BackendProfile profile{"real", "Isolated Codex", "local-unix", socket, executable, false};
        LocalBackend backend(profile, BRIDGE_PATH);
        QSignalSpy states(&backend, &Backend::connectionChanged);
        QSignalSpy search(&backend, &Backend::searchResults);
        backend.connectServer();
        QTRY_VERIFY_WITH_TIMEOUT(backend.ready(), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(states.size() >= 3, 10000);
        QCOMPARE(states.last()[0].toString(), "Connected");
        backend.search("archcodex-isolated-test", 9);
        QTRY_VERIFY_WITH_TIMEOUT(!search.isEmpty(), 10000);
        QVERIFY2(search.last()[2].toString().isEmpty(), qPrintable(search.last()[2].toString()));
        backend.disconnectServer();
        QCOMPARE(server.state(), QProcess::Running);
        server.terminate();
        if (!server.waitForFinished(3000)) {
            server.kill();
            server.waitForFinished();
        }
    }
};
QTEST_GUILESS_MAIN(CoreTests)
#include "test_core.moc"
