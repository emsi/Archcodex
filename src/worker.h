// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QObject>
#include <QThread>
#include <functional>
#include <memory>

namespace Archcodex {
// Shared event loop per host. Destruction never joins a thread on the GUI thread.
class Worker {
  public:
    Worker();
    ~Worker();
    QObject *context;
    void post(std::function<void()> operation);
    static std::shared_ptr<Worker> forHost(const QString &key);
    // After facade destruction, service completion signals before application exit.
    static bool finishPending(int timeoutMs = 10000);

  private:
    QThread *thread;
};
} // namespace Archcodex
