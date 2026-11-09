// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "backend.h"
#include "worker.h"
namespace Archcodex {
class BackendPump;
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
    void verifySession(const QString &id) override;
  signals:
    void batchAccepted();

  private:
    struct State {
        BackendPump *pump = nullptr;
    };
    std::shared_ptr<State> state;
    std::shared_ptr<Worker> worker;
    bool connected = false;
};
} // namespace Archcodex
