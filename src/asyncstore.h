// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "store.h"
#include "worker.h"
namespace Archcodex {
struct StoreSnapshot {
    QList<BackendProfile> profiles;
    QList<Project> projects;
    QList<Session> sessions;
};
class StoreWriter;
class AsyncStore : public QObject {
    Q_OBJECT
  public:
    AsyncStore(const QString &path, QObject *parent = nullptr);
    ~AsyncStore() override;
    void save(const BackendProfile &profile);
    void save(const Session &session);
    void removeBackend(const QString &id);
    void removeSession(const QString &backend, const QString &id);
  signals:
    void loaded(const Archcodex::StoreSnapshot &snapshot);
    void failed(const QString &error);

  private:
    std::shared_ptr<Worker> worker;
    StoreWriter *writer;
};
} // namespace Archcodex
Q_DECLARE_METATYPE(Archcodex::StoreSnapshot)
