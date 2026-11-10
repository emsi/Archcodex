// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "worker.h"
#include <QJsonObject>
#include <QTimer>
namespace Archcodex {
class JsonProcess;
class ContainerTransport;
class ContainerDiscovery : public QObject {
    Q_OBJECT
  public:
    ContainerDiscovery(const QString &id, const QString &sshHost = {}, QObject *parent = nullptr);
    ~ContainerDiscovery() override;
    const QString id, sshHost;
    void refresh();
    void stop();
  signals:
    void snapshot(const QJsonObject &inventory);
    void failed(const QString &error);

  private:
    std::shared_ptr<ContainerTransport> transport;
    bool wanted = false;
};
} // namespace Archcodex
