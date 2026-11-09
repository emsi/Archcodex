// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "types.h"
#include <QHash>
#include <QObject>
#include <QProcess>
#include <QQueue>
#include <QSet>
#include <QTimer>
#include <functional>

namespace Archcodex {
// UI and storage depend on this contract. Transport-specific setup belongs in adapters.
class Backend : public QObject {
    Q_OBJECT
  public:
    explicit Backend(BackendProfile p, QObject *parent = nullptr)
        : QObject(parent), profile(std::move(p)) {}
    const BackendProfile profile;
    virtual void connectServer() = 0;
    virtual void disconnectServer() = 0;
    virtual bool ready() const = 0;
    virtual void loadMore() = 0;
    virtual void search(const QString &query, quint64 token) = 0;
  signals:
    void connectionChanged(const QString &description, bool ready);
    void sessionChanged(const Archcodex::Session &session);
    void sessionRemoved(const QString &id);
    void loadedInventory(const QSet<QString> &ids);
    void moreAvailable(bool available);
    void searchResults(quint64 token, const QList<Archcodex::Session> &sessions,
                       const QString &error, bool complete);
};

// Shared protocol and event handling. Adapters supply only the transport launch plan.
class CodexBackend : public Backend {
    Q_OBJECT
  public:
    CodexBackend(BackendProfile profile, const QString &bridgePath, QObject *parent = nullptr);
    ~CodexBackend() override;
    void connectServer() override;
    void disconnectServer() override;
    bool ready() const override {
        return initialized;
    }
    void loadMore() override;
    void search(const QString &query, quint64 token) override;

  protected:
    virtual QStringList bridgeArguments() const = 0;

  private:
    using Reply = std::function<void(const QJsonObject &, const QString &)>;
    struct Pending {
        Reply reply;
        qint64 deadline;
    };
    QString bridgePath;
    QProcess *process = nullptr;
    QTimer reconnectTimer, deadlineTimer;
    QByteArray buffer;
    QHash<qint64, Pending> pending;
    QHash<QString, QJsonObject> statuses;
    QHash<QString, QString> names;
    QSet<QString> removed;
    QSet<QString> loaded;
    QHash<QString, Session> sessions;
    QQueue<QString> reads;
    QSet<QString> scheduledReads;
    int reading = 0;
    QString nextCursor;
    bool initialized = false, wanted = false, listing = false;
    int retry = 0;
    quint64 generation = 0, searchGeneration = 0;
    qint64 nextId = 1;
    void request(const QString &method, const QJsonObject &params, Reply reply);
    void send(const QJsonObject &message);
    void receive();
    void message(const QJsonObject &message);
    void failed(const QString &reason);
    void shutdownProcess();
    void armDeadline();
    void listPage(const QString &cursor);
    void loadedPage(const QString &cursor = {});
    void readThread(const QString &id);
    void pumpReads();
    void upsert(QJsonObject thread);
    void searchPage(const QString &query, const QString &cursor, quint64 token, quint64 searchId,
                    int pages);
};
class LocalBackend final : public CodexBackend {
  public:
    using CodexBackend::CodexBackend;

  protected:
    QStringList bridgeArguments() const override;
};
} // namespace Archcodex
Q_DECLARE_METATYPE(Archcodex::Session)
