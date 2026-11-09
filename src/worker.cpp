// SPDX-License-Identifier: GPL-3.0-or-later
#include "worker.h"
#include <QEventLoop>
#include <QHash>
#include <QPointer>
#include <QProcess>
#include <QSet>
#include <QTimer>

namespace Archcodex {
static QSet<QThread *> activeThreads;
static QPointer<QEventLoop> shutdownLoop;
Worker::Worker() : context(new QObject), thread(new QThread) {
    activeThreads.insert(thread);
    QObject::connect(thread, &QThread::finished, thread, [t = thread] {
        activeThreads.remove(t);
        if (activeThreads.isEmpty() && shutdownLoop)
            shutdownLoop->quit();
    });
    context->moveToThread(thread);
    QObject::connect(thread, &QThread::finished, context, &QObject::deleteLater);
    QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}
Worker::~Worker() {
    post([t = thread, owner = context] {
        const auto quitWhenDone = [t, owner] {
            for (auto *p : owner->findChildren<QProcess *>())
                if (p->state() != QProcess::NotRunning)
                    return;
            t->quit();
        };
        for (auto *p : owner->findChildren<QProcess *>())
            QObject::connect(p, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), owner,
                             quitWhenDone);
        QTimer::singleShot(2750, owner, [t, owner] {
            for (auto *p : owner->findChildren<QProcess *>())
                if (p->state() != QProcess::NotRunning) {
                    p->kill();
                    p->waitForFinished(1000); // Worker shutdown only; never the GUI thread.
                }
            t->quit();
        });
        quitWhenDone();
    });
}
void Worker::post(std::function<void()> operation) {
    QMetaObject::invokeMethod(context, std::move(operation), Qt::QueuedConnection);
}
bool Worker::finishPending(int timeoutMs) {
    if (activeThreads.isEmpty())
        return true;
    QEventLoop loop;
    shutdownLoop = &loop;
    QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
    loop.exec();
    shutdownLoop = nullptr;
    return activeThreads.isEmpty();
}
std::shared_ptr<Worker> Worker::forHost(const QString &key) {
    // Called by GUI-owned facades only; no global mutex or process-wide worker queue.
    static QHash<QString, std::weak_ptr<Worker>> workers;
    auto worker = workers.value(key).lock();
    if (!worker) {
        worker = std::make_shared<Worker>();
        workers[key] = worker;
    }
    return worker;
}
} // namespace Archcodex
