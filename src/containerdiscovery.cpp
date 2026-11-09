// SPDX-License-Identifier: GPL-3.0-or-later
#include "containerdiscovery.h"
#include "jsonprocess.h"
#include "launch.h"
namespace Archcodex {
ContainerDiscovery::ContainerDiscovery(const QString &key, const QString &host, QObject *parent)
    : QObject(parent), id(key), sshHost(host), worker(Worker::forHost("discovery:" + host)),
      runner(new JsonProcess(worker->context)) {
    runner->moveToThread(worker->context->thread());
    retry.setSingleShot(true);
    connect(&retry, &QTimer::timeout, this, &ContainerDiscovery::refresh);
    connect(runner, &JsonProcess::message, this, [this](const QJsonObject &data) {
        if (wanted) {
            retry.stop();
            retryDelay = 1000;
            if (data["containers"].isArray() && data["issues"].isArray())
                emit snapshot(data);
            else
                emit failed("Invalid container inventory.");
        }
        worker->post([r = runner] { r->acknowledge(); });
    });
    connect(runner, &JsonProcess::failed, this, [this](const QString &message) {
        if (!wanted)
            return;
        emit failed(message);
        retry.start(retryDelay);
        retryDelay = qMin(30000, retryDelay * 2);
    });
}
ContainerDiscovery::~ContainerDiscovery() {
    worker->post([r = runner] { delete r; });
}
void ContainerDiscovery::stop() {
    wanted = false;
    retry.stop();
    worker->post([r = runner] { r->stop(); });
}
void ContainerDiscovery::refresh() {
    wanted = true;
    retry.stop();
    if (!sshHost.isEmpty() && !validSshHost(sshHost)) {
        emit failed("Enter an SSH host alias or user@host, without command options.");
        return;
    }
    const auto command = hostCommand(sshCommand(
        sshHost, {"/usr/bin/python3", "-I", "-u", "-c", resourceScript("containers.py"), "watch"}));
    const QJsonObject request{{"script", resourceScript("discover.py")}};
    worker->post([r = runner, command, request] { r->start(command, request); });
}
} // namespace Archcodex
