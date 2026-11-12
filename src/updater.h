// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QTimer>

namespace Archcodex {
class Updater : public QObject {
    Q_OBJECT
  public:
    enum class State {
        Idle,
        Checking,
        Current,
        Available,
        Downloading,
        Installing,
        Installed,
        Error,
        Restarting,
        RestartReady
    };
    Q_ENUM(State)
    Updater(const QString &version, const QString &dataDirectory, QObject *parent = nullptr,
            const QStringList &helperCommand = {});
    ~Updater() override;
    State state() const {
        return status;
    }
    bool busy() const;
    bool finishing() const;
    QString error() const {
        return failure;
    }
    QJsonObject release() const {
        return selected;
    }
    QString installReason() const {
        return reason;
    }
    QString installedVersion() const {
        return installed;
    }
    int progress() const {
        return percent;
    }
    void check();
    void install();
    void restart();
    void cancel();
  signals:
    void changed();
    void restartReady();

  private:
    State status = State::Idle;
    QJsonObject context, selected, completion;
    QStringList helper;
    QString operation, failure, reason, installed;
    QByteArray buffer, diagnostics;
    QProcess *process = nullptr;
    QTimer deadline;
    int percent = 0;
    bool cancelled = false;
    void start(const QString &operation);
    void receive();
    void fail(const QString &message);
    void setState(State state);
};
} // namespace Archcodex
