// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "types.h"
#include <QHash>
#include <QSortFilterProxyModel>
#include <QStandardItemModel>

namespace Archcodex {
enum ModelRole {
    KeyRole = Qt::UserRole + 1,
    BackendRole,
    PathRole,
    ThreadRole,
    ActiveRole,
    RecencyRole,
    AttentionRole,
    ProjectNameRole
};
class SessionModel : public QStandardItemModel {
  public:
    explicit SessionModel(QObject *parent = nullptr);
    void backendName(const QString &id, const QString &name);
    void project(const Project &project);
    void upsert(const Session &session);
    void remove(const QString &backend, const QString &thread);
    void removeBackend(const QString &backend);
    void resetResults();

  private:
    QHash<QString, QString> backendNames;
    QHash<QString, QStandardItem *> projects, threads;
    QStandardItem *ensureProject(const QString &backend, const QString &path);
    void aggregate(QStandardItem *project);
};
class SessionFilter : public QSortFilterProxyModel {
  public:
    explicit SessionFilter(QObject *parent = nullptr);
    void setActiveOnly(bool active);

  protected:
    bool filterAcceptsRow(int row, const QModelIndex &parent) const override;
    bool lessThan(const QModelIndex &left, const QModelIndex &right) const override;

  private:
    bool activeOnly = false;
};
} // namespace Archcodex
