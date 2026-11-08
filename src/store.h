// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "types.h"
#include <QList>
#include <QSqlDatabase>

namespace Archcodex {
class Store {
  public:
    explicit Store(const QString &path);
    ~Store();
    Store(const Store &) = delete;
    Store &operator=(const Store &) = delete;
    QList<BackendProfile> backends() const;
    QList<Project> projects() const;
    QList<Session> sessions() const;
    void save(const BackendProfile &profile);
    void save(const Project &project);
    void save(const Session &session);
    void removeBackend(const QString &id);
    void removeSession(const QString &backend, const QString &id);

  private:
    QSqlDatabase db;
};
} // namespace Archcodex
