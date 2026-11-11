// SPDX-License-Identifier: GPL-3.0-or-later
#include "asyncbackend.h"
#include "asyncstore.h"
#include "containerdiscovery.h"
#include "fixture.h"
#include "jsonprocess.h"
#include "launch.h"
#include "testenvironment.h"
#include <QElapsedTimer>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QtTest>

static BackendProfile containerProfile(const Fixture &fixture) {
    auto p = fixture.profile();
    p.kind = "mycodex";
    p.containerId = QString(64, 'a');
    p.containerEpoch = "epoch-1";
    p.environmentId = "environment-1";
    p.dockerContext = "default";
    p.runtimeUser = "alice";
    p.runtimeHome = "/home/alice";
    p.discoveryHost = "local-mycodex";
    p.relaySocket = p.socketPath;
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
    TestHome home;
  private slots:
    void waitingNotificationsOnlyFollowLiveRunningTransitions() {
        Fixture f;
        LocalBackend b(f.profile(), BRIDGE_PATH);
        QSignalSpy events(&b, &Backend::attentionRequested);
        b.connectServer();
        QTRY_VERIFY(f.methods.contains("thread/read"));
        QTest::qWait(50);
        QCOMPARE(events.size(), 0);
        const auto status = [&](const QJsonObject &value) {
            f.notify("thread/status/changed", {{"threadId", "thread-1"}, {"status", value}});
        };
        const QJsonObject running{{"type", "active"}, {"activeFlags", QJsonArray{}}};
        const QJsonObject waiting{{"type", "active"},
                                  {"activeFlags", QJsonArray{"waitingOnUserInput"}}};
        const auto initialReads = f.methods.count("thread/read");
        status({{"type", "idle"}});
        status(running);
        status(waiting);
        status(waiting);
        status({{"type", "idle"}});
        QTRY_COMPARE(events.size(), 1);
        const auto event = qvariant_cast<SessionAttention>(events.first()[0]);
        QCOMPARE(event.session.backendId, b.profile.id);
        QCOMPARE(event.session.id, "thread-1");
        QCOMPARE(event.reason, SessionAttention::Reason::Waiting);
        status(running);
        status({{"type", "notLoaded"}});
        status({{"type", "idle"}});
        QTest::qWait(50);
        QCOMPARE(events.size(), 1);
        QCOMPARE(f.methods.count("thread/read"), initialReads);
        b.disconnectServer();
        b.connectServer();
        QTRY_VERIFY(b.ready());
        status({{"type", "idle"}});
        QTest::qWait(50);
        QCOMPARE(events.size(), 1);
        status(running);
        status({{"type", "idle"}});
        QTRY_COMPARE(events.size(), 2);
        QVERIFY(!f.methods.contains("thread/resume"));
        QVERIFY(!f.methods.contains("thread/items/list"));
    }
    void rapidTransitionsSurviveInventoryCoalescing() {
        Fixture f;
        AsyncBackend b(f.profile(), BRIDGE_PATH);
        QSignalSpy events(&b, &Backend::attentionRequested);
        QSignalSpy batches(&b, &Backend::sessionsChanged);
        b.connectServer();
        QTRY_VERIFY(b.ready() && !batches.isEmpty());
        for (int i = 0; i < 20; ++i) {
            f.notify("thread/status/changed",
                     {{"threadId", "thread-1"},
                      {"status", QJsonObject{{"type", "active"}, {"activeFlags", QJsonArray{}}}}});
            f.notify("thread/status/changed",
                     {{"threadId", "thread-1"}, {"status", QJsonObject{{"type", "idle"}}}});
        }
        QTRY_COMPARE(events.size(), 20);
        b.disconnectServer();
        const auto count = events.size();
        QTest::qWait(100);
        QCOMPARE(events.size(), count);
    }
    void transitionsBeforeMetadataReadsAreNotLost_data() {
        QTest::addColumn<QString>("storage");
        for (const auto &value : {"saved", "paginated", "ephemeral", "unsaved"})
            QTest::newRow(value) << QString(value);
    }
    void transitionsBeforeMetadataReadsAreNotLost() {
        QFETCH(QString, storage);
        Fixture f;
        f.delayRead = true;
        f.delaySnapshot = true;
        LocalBackend b(f.profile(), BRIDGE_PATH);
        QSignalSpy events(&b, &Backend::attentionRequested);
        b.connectServer();
        QTRY_VERIFY(b.ready());
        f.notify("thread/status/changed",
                 {{"threadId", "new-live-thread"},
                  {"status", QJsonObject{{"type", "active"}, {"activeFlags", QJsonArray{}}}}});
        f.notify("thread/status/changed",
                 {{"threadId", "new-live-thread"}, {"status", QJsonObject{{"type", "idle"}}}});
        QTRY_COMPARE(f.readIds.size(), 1);
        QCOMPARE(events.size(), 0);
        const auto firstRead = f.readId;
        auto thread = f.thread("new-live-thread");
        thread["path"] = QJsonValue::Null;
        // An older read can precede first-turn persistence. The overlapping idle
        // event already queued a refresh; do not discard the occurrence early.
        f.reply(firstRead, {{"thread", thread}});
        QTRY_COMPARE(f.readIds.size(), 2);
        QCOMPARE(events.size(), 0);
        thread["sessionId"] = "shared-tree";
        thread["forkedFromId"] = "ancestor";
        if (storage == "saved")
            thread["path"] = "/saved/fork.jsonl";
        if (storage == "paginated" || storage == "ephemeral")
            thread["historyMode"] = "paginated";
        thread["ephemeral"] = storage == "ephemeral";
        QSignalSpy metadata(&b, &Backend::sessionChanged);
        f.reply(f.readId, {{"thread", thread}});
        QTRY_VERIFY(!metadata.isEmpty());
        const bool saved = storage == "saved" || storage == "paginated";
        QCOMPARE(events.size(), saved ? 1 : 0);
        if (saved) {
            const auto event = qvariant_cast<SessionAttention>(events.first()[0]);
            QCOMPARE(event.session.id, "new-live-thread");
            QCOMPARE(event.session.backendId, b.profile.id);
            QCOMPARE(event.session.cwd, "/work/project");
            QVERIFY(event.session.isSaved());
        }
        QCOMPARE(f.readIds.size(), 2); // Only the existing inventory refreshes.
        QVERIFY(!f.methods.contains("thread/resume"));
        QVERIFY(!f.methods.contains("thread/items/list"));
    }
    void startedSnapshotDoesNotRearmWaiting() {
        Fixture f;
        LocalBackend b(f.profile(), BRIDGE_PATH);
        QSignalSpy events(&b, &Backend::attentionRequested), metadata(&b, &Backend::sessionChanged);
        b.connectServer();
        QTRY_VERIFY(!metadata.isEmpty());
        const auto status = [&](const QString &type) {
            f.notify("thread/status/changed",
                     {{"threadId", "thread-1"},
                      {"status", QJsonObject{{"type", type}, {"activeFlags", QJsonArray{}}}}});
        };
        status("active");
        status("idle");
        QTRY_COMPARE(events.size(), 1);
        auto snapshot = f.thread();
        snapshot["status"] = QJsonObject{{"type", "active"}, {"activeFlags", QJsonArray{}}};
        f.notify("thread/started", {{"thread", snapshot}});
        status("idle");
        QTest::qWait(30);
        QCOMPARE(events.size(), 1);
        status("active");
        status("idle");
        QTRY_COMPARE(events.size(), 2);
        f.notify("thread/closed", {{"threadId", "thread-1"}});
        f.notify("thread/started", {{"thread", f.thread()}});
        QTRY_VERIFY(qvariant_cast<Session>(metadata.last()[0]).isWaiting());
        QCOMPARE(events.size(), 2);
    }
    void pendingAttentionIsBoundedAndInvalidated_data() {
        QTest::addColumn<QString>("outcome");
        for (const auto &value : {"saved", "failure", "archived", "closed", "disconnect"})
            QTest::newRow(value) << QString(value);
    }
    void pendingAttentionIsBoundedAndInvalidated() {
        QFETCH(QString, outcome);
        Fixture f;
        f.delaySnapshot = f.delayRead = true;
        LocalBackend b(f.profile(), BRIDGE_PATH);
        QSignalSpy events(&b, &Backend::attentionRequested), metadata(&b, &Backend::sessionChanged);
        b.connectServer();
        QTRY_VERIFY(b.ready());
        QTest::ignoreMessage(QtWarningMsg,
                             "Notification metadata backlog exceeded 256; oldest event discarded.");
        for (int i = 0; i < 300; ++i) {
            for (const auto &type : {"active", "idle"})
                f.notify("thread/status/changed",
                         {{"threadId", "pending"},
                          {"status", QJsonObject{{"type", type}, {"activeFlags", QJsonArray{}}}}});
        }
        QTRY_COMPARE(f.readIds.size(), 1);
        // Fence all preceding notifications through an event on this same socket.
        f.notify("thread/started", {{"thread", f.thread("fence")}});
        QTRY_VERIFY(!metadata.isEmpty());
        QCOMPARE(events.size(), 0);
        if (outcome == "disconnect") {
            b.disconnectServer();
            b.connectServer();
            QTRY_VERIFY(b.ready());
            f.notify("thread/started", {{"thread", f.thread("pending")}});
        } else {
            if (outcome == "archived" || outcome == "closed")
                f.notify("thread/" + outcome, {{"threadId", "pending"}});
            if (outcome == "failure")
                f.reject(f.readId, "Metadata unavailable");
            else
                f.reply(f.readId, {{"thread", f.thread("pending")}});
            QTRY_COMPARE(f.readIds.size(), 2);
            f.reply(f.readId, {{"thread", f.thread("pending")}});
        }
        QTest::qWait(30);
        QCOMPARE(events.size(), outcome == "saved" ? 256 : 0);
        QVERIFY(!f.methods.contains("thread/resume"));
    }
    void blockedFacadeRetainsTransitionsButReconnectDiscardsThem() {
        Fixture f;
        AsyncBackend b(f.profile(), BRIDGE_PATH);
        QSignalSpy events(&b, &Backend::attentionRequested);
        b.connectServer();
        QTRY_VERIFY(f.methods.contains("thread/read"));
        QTest::qWait(50);
        const auto change = [&](const QString &type) {
            f.notify("thread/status/changed",
                     {{"threadId", "thread-1"},
                      {"status", QJsonObject{{"type", type}, {"activeFlags", QJsonArray{}}}}});
        };
        b.blockSignals(true); // Withhold batchAccepted, exactly as a busy GUI would.
        change("active");
        QTest::qWait(50);
        change("idle");
        change("active");
        change("idle");
        QTest::qWait(50);
        b.blockSignals(false);
        emit b.batchAccepted();
        QTRY_COMPARE(events.size(), 2);
        b.blockSignals(true);
        change("active");
        QTest::qWait(50);
        change("idle");
        QTest::qWait(50);
        b.disconnectServer();
        b.blockSignals(false);
        emit b.batchAccepted();
        b.connectServer();
        QTRY_VERIFY(b.ready());
        QTest::qWait(50);
        QCOMPARE(events.size(), 2);
    }
    void attentionBackpressureIsBoundedWithoutExtraRequests() {
        Fixture f;
        AsyncBackend b(f.profile(), BRIDGE_PATH);
        QSignalSpy events(&b, &Backend::attentionRequested);
        b.connectServer();
        QTRY_VERIFY(f.methods.contains("thread/read"));
        QTest::qWait(50);
        const auto change = [&](const QString &type) {
            f.notify("thread/status/changed",
                     {{"threadId", "thread-1"},
                      {"status", QJsonObject{{"type", type}, {"activeFlags", QJsonArray{}}}}});
        };
        b.blockSignals(true);
        change("active");
        QTest::qWait(50);
        const auto requests = f.methods.size();
        QTest::ignoreMessage(QtWarningMsg,
                             "Session notification backlog exceeded 256; oldest event discarded.");
        for (int i = 0; i < 300; ++i) {
            change("idle");
            change("active");
        }
        QTest::qWait(250);
        b.blockSignals(false);
        emit b.batchAccepted();
        QTRY_COMPARE(events.size(), 256);
        QCOMPARE(f.methods.size(), requests);
        QVERIFY(b.ready());
    }
    void asynchronousQuestionEventsAreStructuredLiveAndDeduplicated() {
        Fixture f;
        LocalBackend b(f.profile(), BRIDGE_PATH);
        QSignalSpy events(&b, &Backend::attentionRequested);
        b.connectServer();
        QTRY_VERIFY(f.methods.contains("thread/read"));
        QTest::qWait(30);
        const QJsonObject question{
            {"id", "question-item"},
            {"type", "agentMessage"},
            {"delivery", "async"},
            {"questions",
             QJsonArray{QJsonObject{{"id", "choice"}, {"question", "PRIVATE QUESTION"}}}}};
        const auto item = [&](const QJsonObject &value) {
            f.notify("item/completed",
                     {{"threadId", "thread-1"}, {"turnId", "turn-1"}, {"item", value}});
        };
        item(question); // No live turn: reconnect/history must not alert.
        f.notify("turn/started",
                 {{"threadId", "thread-1"}, {"turn", QJsonObject{{"id", "turn-1"}}}});
        f.notify("thread/status/changed",
                 {{"threadId", "thread-1"},
                  {"status", QJsonObject{{"type", "active"}, {"activeFlags", QJsonArray{}}}}});
        item({{"id", "text"}, {"type", "agentMessage"}, {"text", "Are you there?"}});
        item(question);
        item(question);
        QTRY_COMPARE(events.size(), 1);
        QCOMPARE(qvariant_cast<SessionAttention>(events.first()[0]).reason,
                 SessionAttention::Reason::Question);
        QVERIFY(!qvariant_cast<SessionAttention>(events.first()[0])
                     .session.metadata()
                     .contains("questions"));
        f.notify("turn/completed",
                 {{"threadId", "thread-1"}, {"turn", QJsonObject{{"id", "turn-1"}}}});
        item(question);
        QTest::qWait(30);
        QCOMPARE(events.size(), 1);
        QCOMPARE(f.methods.count("thread/resume"), 0);
        QCOMPARE(f.methods.count("thread/items/list"), 0);
        QCOMPARE(f.methods.count("turn/start"), 0);
    }
    void discoveryStartupKeepsNewestHostConfiguration_data() {
        QTest::addColumn<QString>("firstHost");
        QTest::newRow("remote-first") << QString("remote");
        QTest::newRow("local-first") << QString("local-mycodex");
    }
    void discoveryStartupKeepsNewestHostConfiguration() {
        QFETCH(QString, firstHost);
        JsonProcess process(this);
        QSignalSpy messages(&process, &JsonProcess::message);
        QSignalSpy failures(&process, &JsonProcess::failed);
        connect(
            &process, &JsonProcess::message, &process, [&process] { process.acknowledge(); },
            Qt::QueuedConnection);
        const QJsonObject initial{{"hosts", QJsonArray{firstHost}}};
        const QJsonObject both{{"hosts", QJsonArray{"remote", "local-mycodex"}}};
        process.start({"/usr/bin/python3", "-u", "-c",
                       "import sys\nfor line in sys.stdin: print(line, end='', flush=True)"},
                      initial);
        // Another discovery facade registers before QProcess emits started.
        process.request(both);
        QTRY_VERIFY(!messages.isEmpty() || !failures.isEmpty());
        QTest::qWait(100);
        QVERIFY(failures.isEmpty());
        const auto received = qvariant_cast<QJsonObject>(messages.last()[0]);
        process.stop();
        QTRY_VERIFY(findChildren<QProcess *>().isEmpty());
        QCOMPARE(received, both);
    }
    void statusEventsAdvanceRecencyWithoutPollingOrStaleReadRollback() {
        Fixture f;
        LocalBackend b(f.profile(), BRIDGE_PATH);
        QSignalSpy changed(&b, &Backend::sessionChanged);
        b.connectServer();
        QTRY_VERIFY(!changed.isEmpty() && f.methods.contains("thread/read"));
        QTRY_COMPARE(qvariant_cast<Session>(changed.last()[0]).recency, 123);
        const int reads = f.methods.count("thread/read");
        f.notify("thread/status/changed",
                 {{"threadId", "thread-1"},
                  {"status", QJsonObject{{"type", "active"}, {"activeFlags", QJsonArray{}}}}});
        QTRY_VERIFY(qvariant_cast<Session>(changed.last()[0]).isRunning());
        const auto runningRecency = qvariant_cast<Session>(changed.last()[0]).recency;
        QVERIFY(runningRecency > 123);
        f.notify("thread/status/changed",
                 {{"threadId", "thread-1"}, {"status", QJsonObject{{"type", "idle"}}}});
        QTRY_VERIFY(qvariant_cast<Session>(changed.last()[0]).isWaiting());
        QVERIFY(qvariant_cast<Session>(changed.last()[0]).recency >= runningRecency);
        QCOMPARE(f.methods.count("thread/read"), reads);
        QSignalSpy verified(&b, &Backend::sessionVerified);
        b.verifySession("thread-1");
        QTRY_COMPARE(verified.size(), 1);
        QVERIFY(qvariant_cast<Session>(changed.last()[0]).recency >= runningRecency);
        b.disconnectServer();
    }
    void newSessionUsesServerIdentityAndDeliversMetadataBeforeCompletion() {
        Fixture f;
        AsyncBackend b(f.profile(), BRIDGE_PATH);
        QSignalSpy started(&b, &Backend::sessionStarted);
        QSet<QString> received;
        bool ordered = false;
        connect(&b, &Backend::sessionChanged, this,
                [&](const Session &s) { received.insert(s.id); });
        connect(
            &b, &Backend::sessionStarted, this,
            [&](quint64, const Session &s, const QString &) { ordered = received.contains(s.id); });
        b.connectServer();
        QTRY_VERIFY(b.ready());
        b.startSession("/work/project with spaces", 41);
        QTRY_COMPARE(started.size(), 1);
        QVERIFY(ordered);
        QCOMPARE(started[0][0].toULongLong(), 41);
        const auto s = qvariant_cast<Session>(started[0][1]);
        QCOMPARE(s.id, "created-1");
        QCOMPARE(s.cwd, "/work/project with spaces");
        QCOMPARE(s.title, "New session");
        QVERIFY(s.isWaiting());
        QVERIFY(started[0][2].toString().isEmpty());
        QCOMPARE(f.starts[0]["params"].toObject(),
                 (QJsonObject{{"cwd", s.cwd}, {"ephemeral", false}, {"threadSource", "user"}}));
        QCOMPARE(f.renames[0]["params"].toObject(),
                 (QJsonObject{{"threadId", s.id}, {"name", "New session"}}));
        b.disconnectServer();
    }
    void concurrentNewSessionsDoNotGuessFromGlobalEventsOrWorkingDirectories() {
        Fixture f;
        f.delayStart = true;
        ProtocolFixtureBackend b(containerProfile(f));
        QSignalSpy started(&b, &Backend::sessionStarted);
        b.connectServer();
        QTRY_VERIFY(b.ready());
        b.startSession("/work/project", 1);
        b.startSession("/work/project", 2);
        b.startSession("/work/project",
                       2); // Re-delivery of an outstanding token is not a new intent.
        QTRY_COMPARE(f.starts.size(), 2);
        f.notify("thread/started", {{"thread", f.thread("unrelated-client")}});
        for (int index : {1, 0}) {
            const auto id = "actual-" + QString::number(index);
            f.createdThreads[id] = f.thread(id);
            f.reply(f.starts[index]["id"], {{"thread", f.createdThreads[id]}});
            QTRY_COMPARE(started.size(), 2 - index);
            QCOMPARE(started.last()[0].toULongLong(), quint64(index + 1));
            const auto s = qvariant_cast<Session>(started.last()[1]);
            QCOMPARE(s.id, id);
            QVERIFY(s.executionObserved);
            QCOMPARE(s.executionEnvironment, b.profile.environmentId);
        }
        QCOMPARE(f.methods.count("thread/start"), 2);
        b.disconnectServer();
    }
    void newSessionFailureDoesNotRetryOrLoseKnownIdentity_data() {
        QTest::addColumn<bool>("afterCreation");
        QTest::newRow("create-request") << false;
        QTest::newRow("initial-name-request") << true;
    }
    void newSessionFailureDoesNotRetryOrLoseKnownIdentity() {
        QFETCH(bool, afterCreation);
        Fixture f;
        f.delayStart = !afterCreation;
        f.delayName = afterCreation;
        LocalBackend b(f.profile(), BRIDGE_PATH);
        QSignalSpy started(&b, &Backend::sessionStarted);
        b.connectServer();
        QTRY_VERIFY(b.ready());
        b.startSession("/work/project", 11);
        QTRY_COMPARE(f.starts.size(), 1);
        if (afterCreation)
            QTRY_COMPARE(f.renames.size(), 1);
        const auto oldRequest = afterCreation ? f.renames.first() : f.starts.first();
        b.disconnectServer();
        QCOMPARE(started.size(), 1);
        const auto s = qvariant_cast<Session>(started[0][1]);
        QCOMPARE(s.id, afterCreation ? "created-1" : "");
        QVERIFY(
            started[0][2].toString().contains(afterCreation ? "created-1" : "result is unknown"));
        b.connectServer();
        QTRY_VERIFY(b.ready());
        f.reply(oldRequest["id"], {{"thread", f.thread("late-reply")}});
        b.verifySession("thread-1"); // Drain an ordered response after the obsolete result.
        QSignalSpy verified(&b, &Backend::sessionVerified);
        QTRY_COMPARE(verified.size(), 1);
        QCOMPARE(started.size(), 1);
        QCOMPARE(f.methods.count("thread/start"), 1);
        b.disconnectServer();
    }
    void newSessionValidatesRepliesAndReportsPartialFailure() {
        Fixture f;
        LocalBackend b(f.profile(), BRIDGE_PATH);
        QSignalSpy started(&b, &Backend::sessionStarted);
        b.startSession("/work/project", 1);
        QCOMPARE(started.size(), 1);
        QVERIFY(!started.last()[2].toString().isEmpty());
        b.connectServer();
        QTRY_VERIFY(b.ready());
        b.startSession("relative", 2);
        QCOMPARE(started.size(), 2);
        QVERIFY(f.starts.isEmpty());
        f.startError = "Creation denied";
        b.startSession("/work/project", 3);
        QTRY_COMPARE(started.size(), 3);
        QCOMPARE(started.last()[2].toString(), f.startError);
        f.startError.clear();
        f.startFields = {{"id", ""}};
        b.startSession("/work/project", 4);
        QTRY_COMPARE(started.size(), 4);
        QVERIFY(started.last()[2].toString().contains("invalid"));
        QVERIFY(f.renames.isEmpty());
        f.startFields = {};
        f.nameError = "Name could not be saved";
        b.startSession("/work/project", 5);
        QTRY_COMPARE(started.size(), 5);
        QCOMPARE(qvariant_cast<Session>(started.last()[1]).id, "created-3");
        QVERIFY(started.last()[2].toString().contains(f.nameError));
        QVERIFY(started.last()[2].toString().contains("creation was not retried"));
        QCOMPARE(f.methods.count("thread/start"), 3);
        b.disconnectServer();
    }
    void newSessionTimeoutDoesNotRetryCreation() {
        Fixture f;
        f.delayStart = true;
        LocalBackend b(f.profile(), BRIDGE_PATH);
        QSignalSpy started(&b, &Backend::sessionStarted);
        b.connectServer();
        QTRY_VERIFY(b.ready());
        b.startSession("/work/project", 77);
        QTRY_COMPARE_WITH_TIMEOUT(started.size(), 1, 33000);
        QCOMPARE(started[0][0].toULongLong(), 77);
        QVERIFY(started[0][2].toString().contains("result is unknown"));
        QCOMPARE(f.methods.count("thread/start"), 1);
        b.disconnectServer();
        QCOMPARE(started.size(), 1);
    }
    void forkIsNativeAsyncAndMetadataPrecedesCompletion() {
        Fixture f;
        AsyncBackend b(f.profile(), BRIDGE_PATH);
        QSignalSpy changed(&b, &Backend::sessionChanged);
        QSignalSpy forked(&b, &Backend::sessionForked);
        bool metadataArrived = false, ordered = false;
        connect(&b, &Backend::sessionChanged, this,
                [&](const Session &s) { metadataArrived |= s.id == "fork-1"; });
        connect(&b, &Backend::sessionForked, this, [&] { ordered = metadataArrived; });
        b.connectServer();
        QTRY_VERIFY(b.ready());
        b.forkSession("thread-1");
        b.forkSession("thread-1"); // A second click cannot create a concurrent duplicate.
        QTRY_COMPARE(forked.size(), 1);
        QVERIFY(ordered);
        QCOMPARE(forked.last()[0].toString(), "thread-1");
        QCOMPARE(qvariant_cast<Session>(forked.last()[1]).id, "fork-1");
        QVERIFY(forked.last()[2].toString().isEmpty());
        QCOMPARE(f.methods.count("thread/fork"), 1);
        QCOMPARE(f.forkParams, (QJsonObject{{"threadId", "thread-1"}, {"excludeTurns", true}}));
        f.forkError = "Fork not supported";
        b.forkSession("thread-1");
        QTRY_COMPARE(forked.size(), 2);
        QCOMPARE(forked.last()[2].toString(), f.forkError);
        b.disconnectServer();
    }
    void forkRejectsSharedHistoryAndLostConnectionsAreNotRetried() {
        Fixture f;
        ProtocolFixtureBackend b(containerProfile(f));
        QSignalSpy changed(&b, &Backend::sessionChanged);
        QSignalSpy forked(&b, &Backend::sessionForked);
        b.connectServer();
        QTRY_VERIFY(!changed.isEmpty() && qvariant_cast<Session>(changed.last()[0]).known);
        b.forkSession("thread-1");
        QTRY_COMPARE(forked.size(), 1);
        QVERIFY(forked.last()[2].toString().contains("shared history"));
        QVERIFY(!f.methods.contains("thread/fork"));
        b.forkSession("thread-1", "obsolete-environment");
        QTRY_COMPARE(forked.size(), 2);
        QVERIFY(forked.last()[2].toString().contains("shared history"));
        b.forkSession("thread-1", b.profile.environmentId);
        QTRY_COMPARE(forked.size(), 3);
        QVERIFY(forked.last()[2].toString().isEmpty());
        f.notify("thread/started", {{"thread", f.thread()}});
        QTRY_VERIFY(qvariant_cast<Session>(changed.last()[0]).executionObserved);
        b.forkSession("thread-1");
        QTRY_COMPARE(forked.size(), 4);
        QVERIFY(forked.last()[2].toString().isEmpty());
        QVERIFY(qvariant_cast<Session>(forked.last()[1]).executionObserved);
        f.delayRead = true;
        b.forkSession("thread-1");
        QTRY_VERIFY(!f.readId.isUndefined());
        b.disconnectServer();
        QCOMPARE(forked.size(), 5);
        QVERIFY(forked.last()[2].toString().contains("result is unknown"));
        b.connectServer();
        QTRY_VERIFY(b.ready());
        QCOMPARE(f.methods.count("thread/fork"), 2);
        b.disconnectServer();
    }
    void cleanup() {
        QVERIFY(Worker::finishPending());
    }
    void containerIdentityAndSshArgumentBoundaries() {
        Fixture f;
        auto p = containerProfile(f);
        p.sshHost = "alice@build-host";
        QVERIFY(p.validate().isEmpty());
        QCOMPARE(BackendProfile::fromJson(p.json()).json(), p.json());
        QVERIFY(BackendProfile::fromJson(p.json()).relaySocket.isEmpty());
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
    void containerObserverUsesDesktopRelayAndReceivesRealWebSocketEvents() {
        Fixture f;
        f.loaded = {"thread-1"};
        auto p = containerProfile(f);
        p.sshHost = "must-not-be-contacted";
        p.executable = "/container-only/codex";
        LocalBackend b(p, BRIDGE_PATH);
        QSignalSpy changed(&b, &Backend::sessionChanged);
        b.connectServer();
        QTRY_VERIFY(b.ready());
        QTRY_VERIFY(!changed.isEmpty());
        f.notify("thread/status/changed",
                 {{"threadId", "thread-1"}, {"status", QJsonObject{{"type", "idle"}}}});
        QTRY_VERIFY(qvariant_cast<Session>(changed.last()[0]).isWaiting());
        b.disconnectServer();
    }
    void containerClientExecutesDesktopCodexWithExplicitRemoteArguments() {
        Fixture f;
        auto p = containerProfile(f);
        p.executable = "/container-only/codex";
        p.sshHost = "must-not-be-contacted";
        QTemporaryDir directory;
        QFile executable(directory.path() + "/codex");
        QVERIFY(executable.open(QIODevice::WriteOnly));
        executable.write("#!/usr/bin/python3\nimport json,sys\n"
                         "print('codex-cli 0.166.0' if '--version' in sys.argv else "
                         "json.dumps(sys.argv[1:]))\n");
        executable.close();
        QVERIFY(executable.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        for (const bool restrictedPath : {false, true}) {
            for (const auto &id : {QString("thread ; $(false)"), QString()}) {
                const QString cwd = "/remote directory that does not exist locally";
                const auto command =
                    clientArguments(p, id, cwd, restrictedPath ? executable.fileName() : QString());
                QProcess process;
                auto environment = QProcessEnvironment::systemEnvironment();
                environment.insert("PATH", restrictedPath ? "/usr/bin:/bin"
                                                          : directory.path() + ":" +
                                                                environment.value("PATH"));
                process.setProcessEnvironment(environment);
                process.start(command.first(), command.mid(1));
                QVERIFY(process.waitForFinished(10000));
                QCOMPARE(process.exitCode(), 0);
                const auto actual =
                    QJsonDocument::fromJson(process.readAllStandardOutput()).array();
                QJsonArray expected{"--remote", "unix://" + p.relaySocket};
                if (id.isEmpty()) {
                    expected.append("--cd");
                    expected.append(cwd);
                } else {
                    expected.append("resume");
                    expected.append("--");
                    expected.append(id);
                }
                QCOMPARE(actual, expected);
            }
        }
        p.relaySocket.clear();
        QVERIFY(clientArguments(p, "thread").isEmpty());
    }
    void localExecutableSelectionNeverUsesContainerPaths() {
        const auto endpoint = [](const QString &path, bool local, bool primary = false) {
            return QJsonObject{
                {"executable", path}, {"hostEnvironment", local}, {"default", primary}};
        };
        const auto container = endpoint("/container-only/codex", false, true);
        const auto relative = endpoint("codex", true);
        QVERIFY(detectedLocalCodex({container, relative}).isEmpty());
        const auto first = endpoint("/local/a/codex", true);
        const auto second = endpoint("/local/b/codex", true);
        QCOMPARE(detectedLocalCodex({second, container, first, relative}), "/local/a/codex");
        QCOMPARE(detectedLocalCodex({first, second}), detectedLocalCodex({second, first}));
        QCOMPARE(detectedLocalCodex({first, endpoint("/local/main/codex", true, true)}),
                 "/local/main/codex");
        Fixture f;
        const auto args = clientArguments(f.profile(), "thread", "/work", "/different/codex");
        QVERIFY(args.contains(f.profile().executable));
        QVERIFY(!args.contains("/different/codex"));
    }
    void supervisorUsesBoundedArgumentsAndValidCleanupUnitSyntax() {
        const auto command = supervisedTransportCommand(QString(32, 'a'));
        QVERIFY(command.contains("--property=KillMode=control-group"));
        QVERIFY(command.contains("--property=TimeoutStopSec=5s"));
        QString cleanup;
        for (const auto &arg : command) {
            QVERIFY(arg.toUtf8().size() < 128 * 1024);
            if (arg.startsWith("--property=ExecStopPost="))
                cleanup = arg.mid(QString("--property=").size());
        }
        QVERIFY(!cleanup.isEmpty());
        const auto analyze = QStandardPaths::findExecutable("systemd-analyze");
        if (analyze.isEmpty())
            QSKIP("systemd-analyze is unavailable in this build root");
        QTemporaryDir directory;
        QFile unit(directory.path() + "/archcodex-fixture.service");
        QVERIFY(unit.open(QIODevice::WriteOnly));
        unit.write(("[Service]\nType=exec\nExecStart=/usr/bin/true\n" + cleanup + '\n').toUtf8());
        unit.close();
        QProcess process;
        process.start(analyze, {"verify", "--man=no", unit.fileName()});
        QVERIFY(process.waitForFinished(10000));
        QVERIFY2(process.exitCode() == 0, process.readAllStandardError().constData());
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
