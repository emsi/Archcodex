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
    BackendsRole,
    KindRole,
    CountRole,
    ActiveCountRole,
    RunningCountRole,
    WaitingCountRole,
    UnknownCountRole,
    GroupOrderRole
};
enum class SessionView { All, Active, Running, Waiting, Opened };
enum class NodeKind { Server, Folder, Session, Group };
class SessionModel : public QStandardItemModel {
    Q_OBJECT
  public:
    explicit SessionModel(QObject *parent = nullptr);
    void backendName(const QString &id, const QString &name);
    void backendGroup(const QString &backend, const QString &group, const QString &label,
                      int order = 0);
    void backendDetails(const QString &id, const QString &details);
    void backendEnvironment(const QString &id, const QString &environment);
    QString environment(const QString &backend) const;
    void project(const Project &project);
    void upsert(const Session &session);
    void upsertMany(const QList<Session> &sessions);
    void remove(const QString &backend, const QString &thread);
    void removeBackend(const QString &backend);
    void resetResults();
    void setHideUnsaved(bool hide);
    void setContainerWorkdir(const QString &backend, const QString &path);
    void setMyCodexWorkdirOnly(bool enabled);
    void setView(SessionView view);
    SessionView currentView() const {
        return view;
    }
  signals:
    void topologyAboutToChange();
    void topologyChanged();

  private:
    bool hideUnsaved = true;
    bool myCodexWorkdirOnly = true;
    SessionView view = SessionView::All;
    QHash<QString, QString> backendNames, details, environments, projectNames;
    QHash<QString, QString> containerWorkdirs;
    QHash<QString, QStandardItem *> roots, folders, threads;
    QHash<QString, QString> backendGroups, groupLabels;
    QHash<QString, int> groupOrders;
    QHash<QString, QStandardItem *> groups;
    QHash<QString, Session> records;
    bool visible(const Session &s) const;
    void rebuild(const QString &backend);
    void updateSession(QStandardItem *item, const Session &s);
    void aggregate(QStandardItem *parent);
    void aggregateAncestors(QStandardItem *item);
    void placeRoot(const QString &backend);
    void pruneGroups();
    void updateItem(QStandardItem *item, QMap<int, QVariant> values);
};
class SessionFilter : public QSortFilterProxyModel {
  public:
    using View = SessionView;
    explicit SessionFilter(QObject *parent = nullptr);
    void setView(View view);

  protected:
    bool filterAcceptsRow(int row, const QModelIndex &parent) const override;
    bool lessThan(const QModelIndex &left, const QModelIndex &right) const override;
};
} // namespace Archcodex
