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
    QString refreshSummary;
    QJsonArray refreshIssues;
};
class HostDiscovery : public QObject {
    Q_OBJECT
  public:
    explicit HostDiscovery(QObject *parent = nullptr);
    ~HostDiscovery() override;
    void refresh(const QStringList &paths = {}, const QString &revision = {});
    void stop();
  signals:
    void snapshot(const QJsonArray &endpoints, const QJsonArray &issues,
                  const QString &revision = {});
    void failed(const QString &error);

  private:
    std::shared_ptr<Worker> worker;
    JsonProcess *runner;
    bool started = false;
    QString revision;
};
} // namespace Archcodex
