// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "backend.h"
#include "worker.h"
#include <atomic>
#include <optional>
namespace Archcodex {
class BackendPump;
class SearchDelivery;
class AsyncBackend final : public Backend {
    Q_OBJECT
  public:
    AsyncBackend(BackendProfile profile, const QString &bridge, QObject *parent = nullptr);
    ~AsyncBackend() override;
    void connectServer() override;
    void disconnectServer() override;
    bool ready() const override {
        return connected;
    }
    void loadMore() override;
    void search(const QString &query, quint64 token) override;
    void verifySession(const QString &id, quint64 token = 0) override;
    void startSession(const QString &cwd, quint64 token) override;
    void forkSession(const QString &id, const QString &priorExecutionEnvironment = {}) override;
  signals:
    void batchAccepted();

  private:
    friend class BackendPump;
    friend class SearchDelivery;
    struct SearchPage {
        QList<Session> items;
        QString error;
        qsizetype offset = 0;
        bool complete = false;
    };
    std::optional<SearchPage> searchPage;
    quint64 searchRevision = 0, searchToken = 0;
    SearchDelivery *searchDelivery;
    void cancelSearchDelivery();
    void deliverSearch();
    struct State {
        BackendPump *pump = nullptr;
    };
    std::shared_ptr<State> state;
    std::shared_ptr<Worker> worker;
    std::shared_ptr<std::atomic<quint64>> attentionEpoch;
    bool connected = false;
};
} // namespace Archcodex
