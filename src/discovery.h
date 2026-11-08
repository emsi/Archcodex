// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QJsonArray>
#include <QObject>
#include <QProcess>
#include <QTimer>

namespace Archcodex {
class HostDiscovery : public QObject {
    Q_OBJECT
  public:
    explicit HostDiscovery(QObject *parent = nullptr);
    ~HostDiscovery() override;
    void refresh(const QStringList &paths = {});
    void stop();
  signals:
    void snapshot(const QJsonArray &endpoints, const QStringList &warnings);
    void failed(const QString &error);

  private:
    QProcess process;
    QTimer startup;
    QByteArray buffer;
    QStringList paths;
    bool stopping = false;
    void sendRequest();
};
} // namespace Archcodex
