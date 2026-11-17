// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "worker.h"
#include <QJsonObject>
#include <QTimer>
namespace Archcodex {
class JsonProcess;
class SharedHostTransport;
class SharedHostDiscovery : public QObject {
    Q_OBJECT
  public:
    SharedHostDiscovery(const QString &id, const QString &sshHost = {}, QObject *parent = nullptr);
    ~SharedHostDiscovery() override;
    const QString id, sshHost;
    virtual void refresh(const QString &revision = {}, bool servers = true, bool containers = true,
                         const QJsonObject &sshOptions = {}, bool interactive = false);
    void answerAuthentication(const QJsonObject &reply);
    void stop();
  signals:
    void snapshot(const QJsonObject &inventory);
    void failed(const QString &error);

  private:
    std::shared_ptr<SharedHostTransport> transport;
    bool wanted = false;
};
} // namespace Archcodex
