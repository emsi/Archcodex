// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "fixture.h"
#include "store.h"
#include "terminal.h"
#include "window.h"
#include <QAccessible>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QLabel>
#include <QLibrary>
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
    explicit SearchServers(int delay, const QString &shape = {}) {
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
                    server->searchShape = shape;
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

// XTest is used only by the opt-in X11 benchmark. No application dependency.
class SearchNativeKeys {
    QLibrary x11{"X11", 6}, xtst{"Xtst", 6};
    void *display = nullptr;
    using Open = void *(*)(const char *);
    using Close = int (*)(void *);
    using Code = unsigned char (*)(void *, unsigned long);
    using Fake = int (*)(void *, unsigned int, int, unsigned long);
    using Flush = int (*)(void *);
    using Focus = int (*)(void *, unsigned long, int, unsigned long);
    Close close = nullptr;
    Code code = nullptr;
    Fake fake = nullptr;
    Flush flush = nullptr;

  public:
    bool initialize(WId window) {
        auto open = reinterpret_cast<Open>(x11.resolve("XOpenDisplay"));
        close = reinterpret_cast<Close>(x11.resolve("XCloseDisplay"));
        code = reinterpret_cast<Code>(x11.resolve("XKeysymToKeycode"));
        fake = reinterpret_cast<Fake>(xtst.resolve("XTestFakeKeyEvent"));
        flush = reinterpret_cast<Flush>(x11.resolve("XFlush"));
        auto focus = reinterpret_cast<Focus>(x11.resolve("XSetInputFocus"));
        if (!open || !close || !code || !fake || !flush || !focus)
            return false;
        display = open(nullptr);
        if (!display)
            return false;
        focus(display, window, 2, 0);
        flush(display);
        return true;
    }
    void send(int value) {
        const auto text = QString::number(value).toLatin1();
        for (const auto c : text) {
            const auto key = code(display, c);
            fake(display, key, true, 0);
            fake(display, key, false, 0);
        }
        const auto enter = code(display, 0xff0d);
        fake(display, enter, true, 0);
        fake(display, enter, false, 0);
        flush(display);
    }
    ~SearchNativeKeys() {
        if (display)
            close(display);
    }
};
class SearchPaintObserver : public QObject {
  public:
    std::function<void()> painted;
    bool eventFilter(QObject *, QEvent *event) override {
        if (event->type() == QEvent::Paint && painted)
            painted();
        return false;
    }
};

inline void runSearchBenchmark(int delay, const QString &shape = {}) {
    const auto output = qEnvironmentVariable("ARCHCODEX_SEARCH_BENCHMARK");
    if (output.isEmpty())
        QSKIP("Opt-in large Window/PTY benchmark; set ARCHCODEX_SEARCH_BENCHMARK to a JSONL output "
              "file.");
    SearchServers servers(delay, shape);
    QTemporaryDir data;
    QFile child(data.filePath("echo-client"));
    QVERIFY(child.open(QIODevice::WriteOnly));
    child.write(R"PY(#!/usr/bin/python3
import os,tty
tty.setraw(0)
os.write(1,b'BENCH READY\r\n')
b=b''
acks=b''
while True:
 c=os.read(0,1)
 if not c: break
 if c in (b'\r',b'\n'):
  acks+=b'ECHO:'+b+b';'
  os.write(1,b'\x1b[H\x1b[2J'+acks)
  b=b''
 else: b+=c
)PY");
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
    if (!shape.isEmpty()) {
        session.id = "match-1"; // Exercise arrival of the displayed conversation.
        session.cwd = shape == "single"   ? "/work/one-project"
                      : shape == "nested" ? "/work/group-1/nested/nested/project-1"
                      : shape == "distinct"
                          ? "/work/group-1/project-1/deep/worktree"
                          : "/work/large-search/group-0/project-1/long/path/to/worktree";
    }
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
    QEventLoop wait;
    QSet<QString> completed;
    int delivered = 0;
    for (auto *backend : backends)
        QObject::connect(backend, &Backend::searchResults, &window,
                         [&, id = backend->profile.id](quint64, const QList<Session> &items,
                                                       const QString &, bool done) {
                             delivered += items.size();
                             if (done) {
                                 completed.insert(id);
                                 if (completed.size() == 27)
                                     wait.quit();
                             }
                         });
    SearchNativeKeys native;
    if (!shape.isEmpty())
        QVERIFY2(native.initialize(window.winId()), "Native benchmark requires X11 and libXtst");
    QList<qint64> delays, echoes, paints;
    QHash<int, qint64> sent, unpainted;
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
                        unpainted[key] = start;
                        if (shape.isEmpty()) {
                            QTest::keyClicks(input, QString::number(key));
                            QTest::keyClick(input, Qt::Key_Return);
                        } else {
                            native.send(key);
                        }
                    }
                },
                Qt::QueuedConnection);
        }
    });
    SearchPaintObserver paintObserver;
    paintObserver.painted = [&] {
        const auto text = screen->text(0, screen->characterCount()).remove(QLatin1Char('\n'));
        for (auto it = unpainted.begin(); it != unpainted.end();) {
            if (text.contains("ECHO:" + QString::number(it.key()) + ';')) {
                const auto start = it.value();
                // Observe a buffer containing this echo before paint, then time
                // completion after the paint handler returns. Not display scanout.
                QMetaObject::invokeMethod(
                    &measurements, [&, start] { paints << searchClock() - start; },
                    Qt::QueuedConnection);
                it = unpainted.erase(it);
            } else {
                ++it;
            }
        }
    };
    input->installEventFilter(&paintObserver);
    QTimer echoCheck;
    QObject::connect(&echoCheck, &QTimer::timeout, &measurements, [&] {
        const auto text = screen->text(0, screen->characterCount()).remove(QLatin1Char('\n'));
        for (auto it = sent.begin(); it != sent.end();) {
            if (text.contains("ECHO:" + QString::number(it.key()) + ';')) {
                echoes << searchClock() - it.value();
                it = sent.erase(it);
            } else
                ++it;
        }
    });
    echoCheck.start(5);
    QElapsedTimer elapsed;
    elapsed.start();
    int rounds = 0;
    QTimer deadline;
    deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, &wait, &QEventLoop::quit);
    if (shape == "idle") {
        deadline.start(10000);
        wait.exec();
    } else {
        do {
            completed.clear();
            delivered = 0;
            query->setText("needle");
            QTest::keyClick(query, Qt::Key_Return);
            area->focusCurrent();
            deadline.start(120000);
            if (completed.size() != 27)
                wait.exec();
            deadline.stop();
            QCOMPARE(completed.size(), 27);
            QCOMPARE(delivered, 7200);
            ++rounds;
        } while (!shape.isEmpty() && elapsed.elapsed() < 10000);
    }
    const auto duration = elapsed.elapsed();
    producer.request_stop();
    producer.join();
    deadline.start(150); // Drain the final offered input, without offering new keys.
    wait.exec();
    QVERIFY(sent.isEmpty());
    collecting = false;
    echoCheck.stop();
    input->removeEventFilter(&paintObserver);
    if (!shape.isEmpty())
        QTRY_VERIFY_WITH_TIMEOUT(unpainted.isEmpty(), 5000);
    auto *source = qobject_cast<SessionModel *>(
        qobject_cast<QSortFilterProxyModel *>(tree->model())->sourceModel());
    int visible = 0;
    for (int i = 0; i < source->rowCount(); ++i)
        visible += source->item(i)->data(CountRole).toInt();
    if (shape != "idle")
        QCOMPARE(visible, 5760);
    QVERIFY(!echoes.isEmpty());
    QJsonObject result{
        {"version", ARCHCODEX_VERSION},
        {"delay_ms", delay},
        {"shape", shape.isEmpty() ? "legacy" : shape},
        {"rounds", rounds},
        {"input", shape.isEmpty() ? "Qt" : "XTest"},
        {"elapsed_ms", duration},
        {"delivered", delivered},
        {"visible", visible},
        {"gui_delay", searchDistribution(delays)},
        {"terminal_echo", searchDistribution(echoes)},
        {"terminal_paint", searchDistribution(paints)},
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
