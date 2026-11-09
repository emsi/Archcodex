// SPDX-License-Identifier: GPL-3.0-or-later
#include "asyncbackend.h"
#include "asyncstore.h"
#include "containerdiscovery.h"
#include "fixture.h"
#include "launch.h"
#include <QElapsedTimer>
#include <QSqlQuery>
#include <QtTest>

static BackendProfile containerProfile(const Fixture &fixture) {
    auto p = fixture.profile();
    p.kind = "mycodex";
    p.containerId = QString(64, 'a');
    p.containerEpoch = "epoch-1";
    p.dockerContext = "default";
    p.runtimeUser = "alice";
    p.runtimeHome = "/home/alice";
    p.discoveryHost = "local-mycodex";
    return p;
}
class ProtocolFixtureBackend : public CodexBackend {
    bool exec;
    QStringList bridgeArguments() const override {
        if (!exec)
            return {"--socket", profile.socketPath};
        // A raw bidirectional stdio proxy, matching native Codex proxy framing.
        return {"--exec",
                "/usr/bin/python3",
                "-I",
                "-u",
                "-c",
                "import socket,selectors,os\n"
                "s=socket.socket(socket.AF_UNIX);s.connect(__import__('sys').argv[1])\n"
                "poll=selectors.DefaultSelector();poll.register(0,1);poll.register(s,1)\n"
                "while True:\n"
                " for key,_ in poll.select():\n"
                "  data=os.read(key.fd,65536)\n"
                "  if not data: raise SystemExit()\n"
                "  if key.fd==0: s.sendall(data)\n"
                "  else: os.write(1,data)\n",
                profile.socketPath};
    }

  public:
    ProtocolFixtureBackend(const BackendProfile &p, bool useExec = false)
        : CodexBackend(p, BRIDGE_PATH), exec(useExec) {}
};

class RuntimeTests : public QObject {
    Q_OBJECT
  private slots:
    void cleanup() {
        QVERIFY(Worker::finishPending());
    }
    void containerIdentityAndSshArgumentBoundaries() {
        Fixture f;
        auto p = containerProfile(f);
        p.sshHost = "alice@build-host";
        QVERIFY(p.validate().isEmpty());
        QCOMPARE(BackendProfile::fromJson(p.json()).json(), p.json());
        p.sshHost = "-oProxyCommand=bad";
        QVERIFY(!p.validate().isEmpty());
        QVERIFY(!validSshHost("host; touch bad"));
        QTemporaryDir directory;
        const auto marker = directory.path() + "/INJECTED";
        const auto argument = "spaces ' $(touch " + marker + ") `false`; --option";
        const auto args = sshCommand(
            "build-host", {"/usr/bin/python3", "-c",
                           "import json,sys; print(json.dumps(sys.argv[1:]))", argument});
        QVERIFY(args.contains("-T"));
        QVERIFY(args.contains("BatchMode=yes"));
        QProcess shell;
        shell.start("/bin/sh", {"-c", args.last()});
        QVERIFY(shell.waitForFinished());
        QCOMPARE(QJsonDocument::fromJson(shell.readAllStandardOutput()).array(),
                 QJsonArray{argument});
        QVERIFY(!QFile::exists(marker));
        QVERIFY(sshCommand("build-host", {"echo"}, true).contains("-tt"));
    }
    void sharedHistoryDoesNotGrantResumeAuthority() {
        Fixture first, second;
        first.loaded = {"thread-1"};
        // A history payload cannot forge app-owned provenance.
        second.threadFields = {{"archcodexExecutionObserved", true}};
        ProtocolFixtureBackend a(containerProfile(first)), b(containerProfile(second));
        QSignalSpy observed(&a, &Backend::sessionChanged), history(&b, &Backend::sessionChanged);
        a.connectServer();
        b.connectServer();
        QTRY_VERIFY(!observed.isEmpty() && !history.isEmpty());
        QTRY_VERIFY(qvariant_cast<Session>(observed.last()[0]).executionObserved);
        QTRY_VERIFY(qvariant_cast<Session>(history.last()[0]).known);
        auto s = qvariant_cast<Session>(history.last()[0]);
        QVERIFY(s.contextRequired);
        QVERIFY(!s.executionObserved);
        QVERIFY(!s.isActive());
        QVERIFY(s.stateText().contains("history", Qt::CaseInsensitive));
        QSignalSpy verified(&b, &Backend::sessionVerified);
        b.verifySession("thread-1");
        QTRY_COMPARE(verified.size(), 1);
        QVERIFY(!qvariant_cast<Session>(history.last()[0]).executionObserved);
        second.notify("thread/started", {{"thread", second.thread()}});
        QTRY_VERIFY(qvariant_cast<Session>(history.last()[0]).executionObserved);
        second.notify("thread/closed", {{"threadId", "thread-1"}});
        QTRY_VERIFY(!qvariant_cast<Session>(history.last()[0]).isActive());
        QVERIFY(qvariant_cast<Session>(history.last()[0]).executionObserved);
        a.disconnectServer();
        b.disconnectServer();
    }
    void genericExecTransportUsesRawProxyAndKeepsServerAlive() {
        Fixture f;
        ProtocolFixtureBackend backend(f.profile(), true);
        QSignalSpy changes(&backend, &Backend::sessionChanged);
        backend.connectServer();
        QTRY_VERIFY(backend.ready());
        QTRY_VERIFY(!changes.isEmpty());
        f.notify("thread/status/changed",
                 {{"threadId", "thread-1"},
                  {"status", QJsonObject{{"type", "active"}, {"activeFlags", QJsonArray{}}}}});
        QTRY_VERIFY(qvariant_cast<Session>(changes.last()[0]).isRunning());
        const auto requests = f.methods.size();
        QTest::qWait(100);
        QCOMPARE(f.methods.size(), requests);
        backend.disconnectServer();
        QVERIFY(f.server.isListening());
        QVERIFY(!f.methods.contains("thread/resume"));
    }
    void bulkEventsAndStalledRepliesLeaveMainEventLoopResponsive() {
        Fixture f;
        auto backend = std::make_unique<AsyncBackend>(f.profile(), BRIDGE_PATH);
        QSignalSpy batches(backend.get(), &Backend::sessionsChanged);
        QSet<QString> seen;
        connect(backend.get(), &Backend::sessionsChanged, this, [&](const QList<Session> &items) {
            QVERIFY(items.size() <= 64);
            for (const auto &s : items)
                seen.insert(s.id);
        });
        backend->connectServer();
        QTRY_VERIFY(backend->ready());
        QTRY_VERIFY(seen.contains("thread-1"));
        f.delayRead = true;
        int ticks = 0;
        QTimer heartbeat;
        connect(&heartbeat, &QTimer::timeout, this, [&] { ++ticks; });
        heartbeat.start(1);
        for (int i = 0; i < 80; ++i)
            f.notify("thread/started", {{"thread", f.thread(QString("live-%1").arg(i))}});
        QTRY_COMPARE(seen.size(), 81);
        backend->verifySession("live-0");
        QTest::qWait(100);
        QVERIFY(ticks >= 5);
        QElapsedTimer elapsed;
        elapsed.start();
        backend.reset();
        QVERIFY2(elapsed.elapsed() < 100, "Facade destruction blocked the UI thread");
    }
    void lockedDatabaseDoesNotBlockUiAndAcceptedWritesFlushOnExit() {
        QTemporaryDir directory;
        const auto path = directory.path() + "/metadata.sqlite";
        auto writer = std::make_unique<AsyncStore>(path);
        QSignalSpy loaded(writer.get(), &AsyncStore::loaded),
            failed(writer.get(), &AsyncStore::failed);
        QTRY_COMPARE(loaded.size(), 1);
        auto db = QSqlDatabase::addDatabase("QSQLITE", "runtime-lock");
        db.setDatabaseName(path);
        QVERIFY(db.open());
        {
            QSqlQuery query(db);
            QVERIFY(query.exec("BEGIN EXCLUSIVE"));
            writer->save(BackendProfile{"b", "Main"});
            auto session = Session::fromThread("b", {{"id", "t"}, {"cwd", "/work"}});
            session.contextRequired = true;
            session.executionObserved = true;
            for (int i = 0; i < 100; ++i) {
                session.title = QString::number(i);
                writer->save(session);
            }
            int ticks = 0;
            QTimer heartbeat;
            connect(&heartbeat, &QTimer::timeout, this, [&] { ++ticks; });
            heartbeat.start(1);
            QTest::qWait(100);
            QVERIFY(ticks >= 5);
            QVERIFY(query.exec("COMMIT"));
        }
        db.close();
        db = {};
        QSqlDatabase::removeDatabase("runtime-lock");
        QCOMPARE(failed.size(), 0);
        writer.reset();
        QVERIFY(Worker::finishPending());
        Store store(path);
        QCOMPARE(store.sessions().size(), 1);
        QCOMPARE(store.sessions()[0].title, "99");
        QVERIFY(store.sessions()[0].executionObserved);
    }
    void stalledSshDiscoveryDoesNotDelayLocalProtocolOrClose() {
        QTemporaryDir directory;
        QFile ssh(directory.path() + "/ssh");
        QVERIFY(ssh.open(QIODevice::WriteOnly));
        ssh.write("#!/usr/bin/python3\nimport time\ntime.sleep(60)\n");
        ssh.close();
        QVERIFY(ssh.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        const auto oldPath = qgetenv("PATH");
        struct RestorePath {
            QByteArray path;
            ~RestorePath() {
                qputenv("PATH", path);
            }
        } restore{oldPath};
        qputenv("PATH", directory.path().toUtf8() + ':' + oldPath);
        Fixture fixture;
        auto remote = std::make_unique<ContainerDiscovery>("fixture-remote", "test-host");
        auto local = std::make_unique<AsyncBackend>(fixture.profile(), BRIDGE_PATH);
        QSignalSpy updates(local.get(), &Backend::sessionsChanged);
        remote->refresh();
        local->connectServer();
        QTRY_VERIFY(local->ready() && !updates.isEmpty());
        const auto before = updates.size();
        fixture.notify("thread/started", {{"thread", fixture.thread("new-local-thread")}});
        QTRY_VERIFY(updates.size() > before);
        QElapsedTimer elapsed;
        elapsed.start();
        remote.reset();
        local.reset();
        QVERIFY2(elapsed.elapsed() < 100, "SSH cleanup blocked the GUI thread");
        QVERIFY(Worker::finishPending());
    }
};
QTEST_GUILESS_MAIN(RuntimeTests)
#include "test_runtime.moc"
