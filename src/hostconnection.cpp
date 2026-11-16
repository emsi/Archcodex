// SPDX-License-Identifier: GPL-3.0-or-later
#include "hostconnection.h"
#include "jsonprocess.h"
#include "launch.h"
#include <QJsonArray>
#include <QMap>
#include <QUuid>

namespace Archcodex {
// Full snapshots preserve JsonProcess's coalescing without losing another host.
class SharedHostTransport : public QObject {
    Q_OBJECT
  public:
    std::shared_ptr<Worker> worker = Worker::forHost("host-transport");
    JsonProcess *runner = new JsonProcess(worker->context);
    QMap<QString, QJsonObject> hosts;
    QTimer retry;
    bool started = false;
    int delay = 1000;
    SharedHostTransport() {
        runner->moveToThread(worker->context->thread());
        retry.setSingleShot(true);
        connect(&retry, &QTimer::timeout, this, &SharedHostTransport::update);
        connect(runner, &JsonProcess::message, this, [this](const QJsonObject &message) {
            delay = 1000;
            const auto inventories = message["hosts"].toObject();
            for (auto it = inventories.begin(); it != inventories.end(); ++it)
                if (hosts.contains(it.key()) &&
                    it.value().toObject()["revision"] == hosts[it.key()]["revision"]) {
                    hosts[it.key()]["interactive"] = false;
                    emit snapshot(it.key(), it.value().toObject());
                }
            worker->post([r = runner] { r->acknowledge(); });
        });
        connect(runner, &JsonProcess::failed, this, [this](const QString &message) {
            started = false;
            for (auto &host : hosts)
                host["interactive"] = false;
            emit failed("Shared host transport: " + message +
                        "\nThis local computer needs Python 3 and a running systemd user manager.");
            if (!hosts.isEmpty()) {
                retry.start(delay);
                delay = qMin(30000, delay * 2);
            }
        });
    }
    ~SharedHostTransport() override {
        worker->post([r = runner] { delete r; });
    }
    void set(const QString &id, const QString &host, const QString &revision, bool servers,
             bool containers, const QJsonObject &options, bool interactive) {
        hosts[id] = {{"id", id},
                     {"sshHost", host},
                     {"revision", revision},
                     {"servers", servers && !host.isEmpty()},
                     {"containers", containers},
                     {"sshOptions", options},
                     {"interactive", interactive}};
        update();
    }
    void answer(const QJsonObject &reply) {
        worker->post([r = runner, reply] { r->request({{"authReply", reply}}); });
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

static std::shared_ptr<SharedHostTransport> sharedTransport() {
    static std::weak_ptr<SharedHostTransport> shared;
    auto instance = shared.lock();
    if (!instance) {
        instance = std::make_shared<SharedHostTransport>();
        shared = instance;
    }
    return instance;
}

SharedHostDiscovery::SharedHostDiscovery(const QString &key, const QString &host, QObject *parent)
    : QObject(parent), id(key), sshHost(host), transport(sharedTransport()) {
    connect(transport.get(), &SharedHostTransport::snapshot, this,
            [this](const QString &key, const QJsonObject &inventory) {
                if (wanted && key == id)
                    emit snapshot(inventory);
            });
    connect(transport.get(), &SharedHostTransport::failed, this, [this](const QString &message) {
        if (wanted)
            emit failed(message);
    });
}
SharedHostDiscovery::~SharedHostDiscovery() {
    stop();
}
void SharedHostDiscovery::stop() {
    wanted = false;
    transport->remove(id);
}
void SharedHostDiscovery::refresh(const QString &revision, bool servers, bool containers,
                                  const QJsonObject &options, bool interactive) {
    if (!sshHost.isEmpty() && !validSshHost(sshHost)) {
        emit failed("Enter an SSH host alias or user@host, without command options.");
        return;
    }
    wanted = true;
    transport->set(id, sshHost,
                   revision.isEmpty() ? QUuid::createUuid().toString(QUuid::Id128) : revision,
                   servers, containers, options, interactive);
}
void SharedHostDiscovery::answerAuthentication(const QJsonObject &response) {
    if (!wanted)
        return;
    auto reply = response;
    reply["host"] = id;
    transport->answer(reply);
}
} // namespace Archcodex
#include "hostconnection.moc"
