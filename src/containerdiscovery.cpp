// SPDX-License-Identifier: GPL-3.0-or-later
#include "containerdiscovery.h"
#include "jsonprocess.h"
#include "launch.h"
#include <QJsonArray>
#include <QMap>
#include <QUuid>

namespace Archcodex {
// Full snapshots preserve JsonProcess's coalescing without losing another host.
class ContainerTransport : public QObject {
    Q_OBJECT
  public:
    std::shared_ptr<Worker> worker = Worker::forHost("container-transport");
    JsonProcess *runner = new JsonProcess(worker->context);
    QMap<QString, QJsonObject> hosts;
    QTimer retry;
    bool started = false;
    int delay = 1000;
    ContainerTransport() {
        runner->moveToThread(worker->context->thread());
        retry.setSingleShot(true);
        connect(&retry, &QTimer::timeout, this, &ContainerTransport::update);
        connect(runner, &JsonProcess::message, this, [this](const QJsonObject &message) {
            delay = 1000;
            const auto inventories = message["hosts"].toObject();
            for (auto it = inventories.begin(); it != inventories.end(); ++it)
                if (hosts.contains(it.key()))
                    emit snapshot(it.key(), it.value().toObject());
            worker->post([r = runner] { r->acknowledge(); });
        });
        connect(runner, &JsonProcess::failed, this, [this](const QString &message) {
            started = false;
            emit failed("Shared container transport: " + message +
                        "\nThis local computer needs Python 3 and a running systemd user manager.");
            if (!hosts.isEmpty()) {
                retry.start(delay);
                delay = qMin(30000, delay * 2);
            }
        });
    }
    ~ContainerTransport() override {
        worker->post([r = runner] { delete r; });
    }
    void set(const QString &id, const QString &host) {
        const auto revision = hosts.value(id)["revision"].toInt() + 1;
        hosts[id] = {{"id", id}, {"sshHost", host}, {"revision", revision}};
        update();
    }
    void remove(const QString &id) {
        if (hosts.remove(id))
            update();
    }
    void update() {
        retry.stop();
        if (hosts.isEmpty()) {
            started = false;
            worker->post([r = runner] { r->stop(); });
            return;
        }
        QJsonArray list;
        for (const auto &entry : hosts)
            list.append(entry);
        QJsonObject request{{"hosts", list}};
        if (started) {
            worker->post([r = runner, request] { r->request(request); });
        } else {
            started = true;
            const auto token = QUuid::createUuid().toString(QUuid::Id128);
            const auto command = supervisedTransportCommand(token);
            worker->post([r = runner, command, request] { r->start(command, request); });
        }
    }
  signals:
    void snapshot(const QString &id, const QJsonObject &inventory);
    void failed(const QString &message);
};

static std::shared_ptr<ContainerTransport> sharedTransport() {
    static std::weak_ptr<ContainerTransport> shared;
    auto instance = shared.lock();
    if (!instance) {
        instance = std::make_shared<ContainerTransport>();
        shared = instance;
    }
    return instance;
}

ContainerDiscovery::ContainerDiscovery(const QString &key, const QString &host, QObject *parent)
    : QObject(parent), id(key), sshHost(host), transport(sharedTransport()) {
    connect(transport.get(), &ContainerTransport::snapshot, this,
            [this](const QString &key, const QJsonObject &inventory) {
                if (wanted && key == id)
                    emit snapshot(inventory);
            });
    connect(transport.get(), &ContainerTransport::failed, this, [this](const QString &message) {
        if (wanted)
            emit failed(message);
    });
}
ContainerDiscovery::~ContainerDiscovery() {
    stop();
}
void ContainerDiscovery::stop() {
    wanted = false;
    transport->remove(id);
}
void ContainerDiscovery::refresh() {
    if (!sshHost.isEmpty() && !validSshHost(sshHost)) {
        emit failed("Enter an SSH host alias or user@host, without command options.");
        return;
    }
    wanted = true;
    transport->set(id, sshHost);
}
} // namespace Archcodex
#include "containerdiscovery.moc"
