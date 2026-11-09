// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QTimer>
namespace Archcodex {
class JsonProcess : public QObject {
    Q_OBJECT
  public:
    explicit JsonProcess(QObject *reaper);
    ~JsonProcess() override;
    void start(const QStringList &command, const QJsonObject &request);
    void request(const QJsonObject &request);
    void stop();
    void acknowledge();
  signals:
    void message(const QJsonObject &message);
    void failed(const QString &error);

  private:
    QObject *reaper;
    QProcess *process = nullptr;
    QTimer deadline;
    QByteArray buffer, tail;
    QJsonObject latest;
    bool inFlight = false;
    quint64 generation = 0;
    void fail(const QString &reason);
    void receive();
};
} // namespace Archcodex
