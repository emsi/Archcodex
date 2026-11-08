// SPDX-License-Identifier: GPL-3.0-or-later
#include "backend.h"
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
class Fixture : public QObject {
  public:
    QTemporaryDir directory;
    QLocalServer local;
    QWebSocketServer server{"fixture", QWebSocketServer::NonSecureMode};
    QPointer<QWebSocket> peer;
    QStringList methods;
    bool delaySnapshot = false;
    QJsonValue snapshotId = QJsonValue::Undefined;
    Fixture() {
        if (!server.listen(QHostAddress::LocalHost, 0) || !local.listen(path()))
            qFatal("Fixture listen failed");
        connect(&local, &QLocalServer::newConnection, this, [this] {
            auto *u = local.nextPendingConnection();
            auto *tcp = new QTcpSocket(u);
            auto forward = [u, tcp] {
                if (tcp->state() == QAbstractSocket::ConnectedState)
                    tcp->write(u->readAll());
            };
            connect(u, &QLocalSocket::readyRead, this, forward);
            connect(tcp, &QTcpSocket::connected, this, forward);
            connect(tcp, &QTcpSocket::readyRead, u, [u, tcp] { u->write(tcp->readAll()); });
            connect(u, &QLocalSocket::disconnected, u, &QObject::deleteLater);
            connect(tcp, &QTcpSocket::disconnected, u, &QLocalSocket::disconnectFromServer);
            tcp->connectToHost(QHostAddress::LocalHost, server.serverPort());
        });
        connect(&server, &QWebSocketServer::newConnection, this, [this] {
            peer = server.nextPendingConnection();
            peer->setParent(this);
            connect(peer, &QWebSocket::textMessageReceived, this, [this](const QString &text) {
                const auto m = QJsonDocument::fromJson(text.toUtf8()).object();
                const auto method = m["method"].toString();
                methods << method;
                if (method == "initialize")
                    reply(m["id"], {{"userAgent", "fixture"}});
                else if (method == "thread/list") {
                    if (delaySnapshot)
                        snapshotId = m["id"];
                    else
                        reply(m["id"],
                              {{"data", QJsonArray{thread()}}, {"nextCursor", QJsonValue::Null}});
                } else if (method == "thread/loaded/list")
                    reply(m["id"], {{"data", QJsonArray{}}});
                else if (method == "thread/read")
                    reply(m["id"],
                          {{"thread", thread(m["params"].toObject()["threadId"].toString())}});
                else if (method == "thread/search") {
                    const bool second = m["params"].toObject().contains("cursor");
                    reply(m["id"],
                          {{"data",
                            QJsonArray{QJsonObject{{"thread", thread(second ? "second" : "first")},
                                                   {"snippet", "needle"}}}},
                           {"nextCursor",
                            second ? QJsonValue(QJsonValue::Null) : QJsonValue("page2")}});
                }
            });
        });
    }
    QString path() const {
        return directory.path() + "/server.sock";
    }
    QJsonObject thread(const QString &id = "thread-1") const {
        return {{"id", id},
                {"name", "Example"},
                {"cwd", "/work/project"},
                {"updatedAt", 123},
                {"status", QJsonObject{{"type", "idle"}}}};
    }
    void reply(QJsonValue id, const QJsonObject &result) {
        peer->sendTextMessage(
            QString::fromUtf8(QJsonDocument(QJsonObject{{"id", id}, {"result", result}})
                                  .toJson(QJsonDocument::Indented)));
    }
    void notify(const QString &method, const QJsonObject &params) {
        peer->sendTextMessage(
            QString::fromUtf8(QJsonDocument(QJsonObject{{"method", method}, {"params", params}})
                                  .toJson(QJsonDocument::Compact)));
    }
    BackendProfile profile() const {
        return {"backend-1", "Local", "local-unix", path(), "codex", false};
    }
};

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
    void pushedEventsAndNoPolling() {
        Fixture f;
        LocalBackend b(f.profile(), BRIDGE_PATH);
        QSignalSpy updates(&b, &Backend::sessionChanged);
        b.connectServer();
        QTRY_VERIFY_WITH_TIMEOUT(b.ready() && !updates.isEmpty(), 5000);
        QCOMPARE(qvariant_cast<Session>(updates.last().at(0)).stateText(), "Idle");
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
