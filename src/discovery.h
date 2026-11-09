// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QDateTime>
#include <QJsonArray>
#include <QObject>
#include <QProcess>
#include <QTimer>

namespace Archcodex {
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
    QProcess process;
    QTimer startup;
    QByteArray buffer, stderrTail;
    QString failureDetails(const QString &summary);
    QStringList paths;
    bool stopping = false;
    void sendRequest();
};
} // namespace Archcodex
