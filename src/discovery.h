// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "worker.h"
#include <QDateTime>
#include <QJsonArray>
#include <QObject>
#include <QProcess>
#include <QTimer>

namespace Archcodex {
class JsonProcess;
struct DiscoveryReport {
    QJsonArray issues;
    QString failure;
    QDateTime checked;
    int endpoints = 0;
    bool inProgress = false;
};
class HostDiscovery : public QObject {
    Q_OBJECT
  public:
    explicit HostDiscovery(QObject *parent = nullptr);
    ~HostDiscovery() override;
    void refresh(const QStringList &paths = {});
    void stop();
  signals:
    void snapshot(const QJsonArray &endpoints, const QJsonArray &issues);
    void failed(const QString &error);

  private:
    std::shared_ptr<Worker> worker;
    JsonProcess *runner;
    bool started = false;
};
} // namespace Archcodex
