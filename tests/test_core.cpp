// SPDX-License-Identifier: GPL-3.0-or-later
#include "backend.h"
#include "discovery.h"
#include "fixture.h"
#include "hosts.h"
#include "store.h"
#include "testenvironment.h"
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
    TestHome home;
  private slots:
    void unreadStateSurvivesMetadataAndHasAnExactSource() {
        QTemporaryDir directory;
        const auto path = directory.filePath("metadata.sqlite");
        const BackendProfile first{"first", "First"}, second{"second", "Second"};
        {
            Store store(path);
            store.save(first);
            store.save(second);
            store.save(SessionReadState{first.id, "thread", "source-a", true});
            store.save(SessionReadState{second.id, "thread", "source-b", true});
            auto session = Session::fromThread(first.id, {{"id", "thread"}, {"name", "Renamed"}});
            store.save(session); // Metadata arriving after the event cannot erase unread state.
            QCOMPARE(store.readStates().size(), 2);
        }
        {
            Store store(path);
            const auto states = store.readStates();
            QCOMPARE(states.size(), 2);
            for (const auto &s : states) {
                QVERIFY(s.unread);
                QCOMPARE(s.sourceIdentity, s.backendId == first.id ? "source-a" : "source-b");
            }
            store.save(SessionReadState{first.id, "thread", "source-a", false});
            QCOMPARE(store.readStates().size(), 1);
            store.removeBackend(second.id);
            QVERIFY(store.readStates().isEmpty());
            store.save(SessionReadState{first.id, "thread", "source-a", true});
            store.removeSession(first.id, "thread");
            QVERIFY(store.readStates().isEmpty());
        }
    }
    void hostConfigurationMigratesWithoutDisablingThisComputer() {
        const QJsonArray legacy{
            QJsonObject{{"id", "local-mycodex"}, {"ssh", ""}, {"enabled", false}},
            QJsonObject{{"id", "remote"}, {"ssh", "alice@server"}, {"enabled", false}}};
        const auto migrated = readHosts(legacy, true);
        QCOMPARE(migrated.size(), 2);
        QVERIFY(migrated["local-mycodex"].enabled);
        QVERIFY(migrated["local-mycodex"].servers);
        QVERIFY(!migrated["local-mycodex"].containers);
        QVERIFY(!migrated["remote"].enabled);
        QVERIFY(migrated["remote"].servers);
        QJsonArray current;
        for (const auto &host : migrated)
            current.append(host.json());
        const auto restored = readHosts(current);
        QCOMPARE(restored["local-mycodex"].json(), migrated["local-mycodex"].json());
        QCOMPARE(restored["remote"].json(), migrated["remote"].json());
        auto tampered = migrated["local-mycodex"].json();
        tampered["enabled"] = tampered["servers"] = false;
        QVERIFY(readHosts({tampered})["local-mycodex"].enabled);
        QVERIFY(readHosts({tampered})["local-mycodex"].servers);
        // A remote-only or empty saved list must still acquire the implicit local host.
        QCOMPARE(readHosts({current[1]}).size(), 2);
        QCOMPARE(readHosts({}).size(), 1);
        const auto malformed = readHosts({QJsonObject{{"id", "invalid"}, {"ssh", 42}}});
        QVERIFY(!malformed.contains("invalid"));
        QCOMPARE(malformed.size(), 1);
    }
    void sshHostOptionsRoundTripAndValidate() {
        HostProfile host{"one", "server"};
        host.user = "alice";
        host.port = 2222;
        host.identityFile = "~/.ssh/key with spaces";
        QVERIFY(host.validate().isEmpty());
        QCOMPARE(readHosts({host.json()})["one"].json(), host.json());
        auto duplicate = host;
        duplicate.id = "duplicate";
        duplicate.user.clear();
        duplicate.ssh = "alice@server";
        QCOMPARE(duplicate.connectionKey(), host.connectionKey());
        auto otherPort = host;
        otherPort.id = "other-port";
        otherPort.port = 2223;
        QCOMPARE(readHosts({host.json(), duplicate.json(), otherPort.json()}).size(), 3);
        host.ssh = "bob@server";
        QVERIFY(!host.validate().isEmpty());
        host.ssh = "server";
        for (const auto &path : {"relative", "/key\ninvalid"}) {
            host.identityFile = path;
            QVERIFY(!host.validate().isEmpty());
        }
        auto invalid = duplicate.json();
        invalid["sshOptions"] = QJsonObject{{"port", 2.5}};
        QVERIFY(!readHosts({invalid}).contains(duplicate.id));
        invalid["sshOptions"] = QJsonObject{{"port", 65536}};
        QVERIFY(!readHosts({invalid}).contains(duplicate.id));
        invalid["sshOptions"] = QJsonObject{{"password", "never accepted"}};
        QVERIFY(!readHosts({invalid}).contains(duplicate.id));
    }
    void remoteHostProfilesRequireExplicitIdentityAndNeverPersistRelayPaths() {
        BackendProfile p{"remote", "Remote"};
        p.kind = "ssh-unix";
        QVERIFY(!p.validate().isEmpty());
        p.sshHost = "server";
        p.discoveryHost = "host-id";
        p.socketPath = "/remote/.codex/server.sock";
        QVERIFY(p.validate().isEmpty());
        QVERIFY(p.usesRelay());
        QVERIFY(!p.isContainer());
        QVERIFY(!p.isLocal());
        QVERIFY(clientArguments(p, "thread").isEmpty());
        p.relaySocket = "/private/local/relay";
        p.executable = "/remote-only/bin/codex";
        const auto args = clientArguments(p, "thread", "/remote/work", "/desktop/bin/codex");
        const auto request =
            QJsonDocument::fromJson(QByteArray::fromBase64(args.last().toUtf8())).object();
        QCOMPARE(request["socket"].toString(), p.relaySocket);
        QCOMPARE(request["executable"].toString(), "/desktop/bin/codex");
        QVERIFY(BackendProfile::fromJson(p.json()).relaySocket.isEmpty());
    }
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
        QProcess process;
        process.start(args.first(), args.mid(1));
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
        QCOMPARE(serverTreeLabel(p), "myCodex · example-project");
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
        QCOMPARE(qvariant_cast<ConnectionState>(states.last()[0]).description(), "Connected");
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
