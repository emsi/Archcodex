// SPDX-License-Identifier: GPL-3.0-or-later
#include "discovery.h"
#include "jsonprocess.h"
#include "launch.h"
namespace Archcodex {
HostDiscovery::HostDiscovery(QObject *parent)
    : QObject(parent), worker(Worker::forHost("discovery:local")),
      runner(new JsonProcess(worker->context)) {
    runner->moveToThread(worker->context->thread());
    connect(runner, &JsonProcess::message, this, [this](const QJsonObject &data) {
        if (data["endpoints"].isArray() && data["issues"].isArray())
            emit snapshot(data["endpoints"].toArray(), data["issues"].toArray());
        else
            emit failed("Invalid host discovery response.");
        worker->post([r = runner] { r->acknowledge(); });
    });
    connect(runner, &JsonProcess::failed, this, [this](const QString &error) {
        started = false;
        emit failed(error);
    });
}
HostDiscovery::~HostDiscovery() {
    worker->post([r = runner] { delete r; });
}
void HostDiscovery::stop() {
    started = false;
    worker->post([r = runner] { r->stop(); });
}
void HostDiscovery::refresh(const QStringList &paths) {
    QJsonObject request{{"paths", QJsonArray::fromStringList(paths)}};
    if (started)
        worker->post([r = runner, request] { r->request(request); });
    else {
        started = true;
        const auto command =
            hostCommand({"/usr/bin/python3", "-I", "-u", "-c", resourceScript("discover.py")});
        worker->post([r = runner, command, request] { r->start(command, request); });
    }
}
} // namespace Archcodex
