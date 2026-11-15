// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "fixture.h"
#include "store.h"
#include "terminal.h"
#include "window.h"
#include <QAccessible>
#include <QElapsedTimer>
#include <QLabel>
#include <QLineEdit>
#include <QSplitter>
#include <QThread>
#include <QTreeView>
#include <QtTest>
#include <atomic>
#include <chrono>
#include <thread>

// Construct servers on a separate event loop: a blocked GUI must not throttle
// the fixture's producer and accidentally conceal an unbounded delivery queue.
class SearchServers {
    QThread thread;
    QObject *context = new QObject;

  public:
    QList<BackendProfile> profiles;
    explicit SearchServers(int delay) {
        context->moveToThread(&thread);
        QObject::connect(&thread, &QThread::finished, context, &QObject::deleteLater);
        thread.start();
        QMetaObject::invokeMethod(
            context,
            [&] {
                for (int i = 0; i < 3; ++i) {
                    auto *server = new Fixture;
                    server->setParent(context);
                    server->searchMatches = 2400;
                    server->searchDelayMs = i == 2 ? delay : 0;
                    auto p = server->profile();
                    p.id = QString("search-%1").arg(i);
                    p.name = "Search server " + QString::number(i);
                    profiles << p;
                }
            },
            Qt::BlockingQueuedConnection); // Fixture setup only, never application code.
    }
    ~SearchServers() {
        thread.quit();
        thread.wait();
    }
};

inline qint64 searchClock() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

inline QJsonObject searchDistribution(QList<qint64> samples) {
    std::sort(samples.begin(), samples.end());
    if (samples.isEmpty())
        return {};
    const auto at = [&](double p) {
        return samples[qMin(samples.size() - 1, qsizetype(p * samples.size()))] / 1000.0;
    };
    return {{"samples", samples.size()},
            {"p50_ms", at(.50)},
            {"p95_ms", at(.95)},
            {"p99_ms", at(.99)},
            {"max_ms", samples.last() / 1000.0}};
}

inline void runSearchBenchmark(int delay) {
    const auto output = qEnvironmentVariable("ARCHCODEX_SEARCH_BENCHMARK");
    if (output.isEmpty())
        QSKIP("Opt-in large Window/PTY benchmark; set ARCHCODEX_SEARCH_BENCHMARK to a JSONL output "
              "file.");
    SearchServers servers(delay);
    QTemporaryDir data;
    QFile child(data.filePath("echo-client"));
    QVERIFY(child.open(QIODevice::WriteOnly));
    child.write("#!/usr/bin/python3\nimport os,tty\ntty.setraw(0)\nos.write(1,b'BENCH "
                "READY\\r\\n')\nb=b''\nwhile True:\n c=os.read(0,1)\n if not c: break\n if c in "
                "(b'\\r',b'\\n'):\n  os.write(1,b'ECHO:'+b+b'\\r\\n'); b=b''\n else: b+=c\n");
    child.close();
    QVERIFY(child.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    {
        Store store(data.filePath("metadata.sqlite"));
        for (auto p : servers.profiles) {
            p.executable = child.fileName();
            store.save(p);
        }
        for (int i = 0; i < 24; ++i) {
            auto p = servers.profiles.first();
            p.id = "offline-" + QString::number(i);
            p.name = QString("Disconnected connection %1 with a very long host and container "
                             "description for coverage")
                         .arg(i);
            p.socketPath = data.filePath("missing-" + QString::number(i));
            store.save(p);
        }
    }
    Window window(data.path(), false);
    window.resize(1400, 950);
    window.show();
    window.activateWindow();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    window.findChild<QSplitter *>()->setSizes({360, 1040});
    QTRY_COMPARE(window.findChildren<Backend *>().size(), 27);
    auto backends = window.findChildren<Backend *>();
    QList<Backend *> connected;
    for (auto *backend : backends)
        if (backend->profile.id.startsWith("search-")) {
            backend->connectServer();
            connected << backend;
        }
    for (auto *backend : connected)
        QTRY_VERIFY(backend->ready());
    auto *area = window.findChild<TerminalArea *>();
    auto p = connected.first()->profile;
    auto session = Session::fromThread(p.id, QJsonObject{{"id", "benchmark-terminal"},
                                                         {"cwd", "/work"},
                                                         {"name", "Benchmark terminal"},
                                                         {"path", "/saved/benchmark.jsonl"}});
    QString error;
    QVERIFY2(area->open(p, session, &error), qPrintable(error));
    area->focusCurrent();
    QTRY_VERIFY(QApplication::focusWidget() && area->isAncestorOf(QApplication::focusWidget()));
    auto *input = QApplication::focusWidget();
    auto *accessible = QAccessible::queryAccessibleInterface(input);
    QVERIFY(accessible && accessible->textInterface());
    auto *screen = accessible->textInterface();
    QTRY_VERIFY(screen->text(0, screen->characterCount()).contains("BENCH READY"));
    auto *query = window.findChild<QLineEdit *>("sessionSearch");
    auto *tree = window.findChild<QTreeView *>();
    auto *feedback = window.findChild<QLabel *>("searchFeedback");
    QSet<QString> completed;
    int delivered = 0;
    for (auto *backend : backends)
        QObject::connect(backend, &Backend::searchResults, &window,
                         [&, id = backend->profile.id](quint64, const QList<Session> &items,
                                                       const QString &, bool done) {
                             delivered += items.size();
                             if (done)
                                 completed.insert(id);
                         });
    QList<qint64> delays, echoes;
    QHash<int, qint64> sent;
    QObject measurements;
    bool collecting = true;
    // Fixed 20 ms offered load, independent of GUI stalls. Every fifth message
    // is a real widget key sequence; screen accessibility observes the PTY echo.
    std::jthread producer([&](std::stop_token stop) {
        auto next = std::chrono::steady_clock::now();
        int tick = 0;
        while (!stop.stop_requested()) {
            next += std::chrono::milliseconds(20);
            std::this_thread::sleep_until(next);
            const auto start = searchClock();
            const int key = ++tick % 5 == 0 ? tick / 5 : 0;
            QMetaObject::invokeMethod(
                &measurements,
                [&, start, key] {
                    if (!collecting)
                        return;
                    delays << searchClock() - start;
                    if (key) {
                        sent[key] = start;
                        QTest::keyClicks(input, QString::number(key));
                        QTest::keyClick(input, Qt::Key_Return);
                    }
                },
                Qt::QueuedConnection);
        }
    });
    QTimer echoCheck;
    QObject::connect(&echoCheck, &QTimer::timeout, &measurements, [&] {
        const auto text = screen->text(0, screen->characterCount());
        for (auto it = sent.begin(); it != sent.end();) {
            if (text.contains("ECHO:" + QString::number(it.key()) + '\n') ||
                text.contains("ECHO:" + QString::number(it.key()) + ' ')) {
                echoes << searchClock() - it.value();
                it = sent.erase(it);
            } else
                ++it;
        }
    });
    echoCheck.start(5);
    QElapsedTimer elapsed;
    elapsed.start();
    query->setText("needle");
    QTest::keyClick(query, Qt::Key_Return);
    area->focusCurrent();
    QTRY_COMPARE_WITH_TIMEOUT(completed.size(), 27, 120000);
    const auto duration = elapsed.elapsed();
    producer.request_stop();
    producer.join();
    QTest::qWait(120); // Drain the final offered input, without offering new keys.
    QTRY_VERIFY_WITH_TIMEOUT(sent.isEmpty(), 5000);
    collecting = false;
    echoCheck.stop();
    QCOMPARE(delivered, 7200);
    auto *source = qobject_cast<SessionModel *>(
        qobject_cast<QSortFilterProxyModel *>(tree->model())->sourceModel());
    int visible = 0;
    for (int i = 0; i < source->rowCount(); ++i)
        visible += source->item(i)->data(CountRole).toInt();
    QCOMPARE(visible, 5760);
    QVERIFY(!echoes.isEmpty());
    QJsonObject result{
        {"version", ARCHCODEX_VERSION},
        {"delay_ms", delay},
        {"elapsed_ms", duration},
        {"delivered", delivered},
        {"visible", visible},
        {"gui_delay", searchDistribution(delays)},
        {"terminal_echo", searchDistribution(echoes)},
        {"unobserved_echoes", sent.size()},
        {"tree_height", tree->viewport()->height()},
        {"feedback_height", window.findChild<QWidget *>("searchFeedbackArea")
                                ? window.findChild<QWidget *>("searchFeedbackArea")->height()
                                : feedback->height()},
        {"window_height", window.height()},
        {"font_points", window.font().pointSizeF()}};
    QFile log(output);
    QVERIFY(log.open(QIODevice::WriteOnly | QIODevice::Append));
    log.write(QJsonDocument(result).toJson(QJsonDocument::Compact) + '\n');
    qInfo().noquote() << QJsonDocument(result).toJson(QJsonDocument::Compact);
    window.close();
}
