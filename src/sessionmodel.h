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
    RunningRole,
    WaitingRole,
    RecencyRole,
    AttentionRole,
    ProjectNameRole,
    EnvironmentRole,
    BackendsRole
};
class SessionModel : public QStandardItemModel {
  public:
    explicit SessionModel(QObject *parent = nullptr);
    void backendName(const QString &id, const QString &name);
    void backendEnvironment(const QString &id, const QString &environment);
    QString environment(const QString &backend) const;
    void project(const Project &project);
    void upsert(const Session &session);
    void remove(const QString &backend, const QString &thread);
    void removeBackend(const QString &backend);
    void resetResults();

  private:
    QHash<QString, QString> backendNames, environments, projectNames;
    QHash<QString, QStandardItem *> projects, threads;
    QStandardItem *ensureProject(const QString &backend, const QString &path);
    void aggregate(QStandardItem *project);
    void prune(QStandardItem *project);
    void updateItem(QStandardItem *item, QMap<int, QVariant> values);
};
class SessionFilter : public QSortFilterProxyModel {
  public:
    enum class View { All, Active, Running, Waiting };
    explicit SessionFilter(QObject *parent = nullptr);
    void setView(View view);

  protected:
    bool filterAcceptsRow(int row, const QModelIndex &parent) const override;
    bool lessThan(const QModelIndex &left, const QModelIndex &right) const override;

  private:
    View view = View::All;
};
} // namespace Archcodex
