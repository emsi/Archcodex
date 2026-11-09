// SPDX-License-Identifier: GPL-3.0-or-later
#include "sessionmodel.h"
#include <QApplication>
#include <QFileInfo>
#include <QPainter>
#include <QPalette>
#include <QSet>

namespace Archcodex {
static QIcon dot(int attention) {
    QPixmap p(16, 16);
    p.fill(Qt::transparent);
    QPainter paint(&p);
    paint.setRenderHint(QPainter::Antialiasing);
    const QColor color = attention == 4   ? QColor("#bd7519")
                         : attention == 3 ? QColor("#c64b4b")
                         : attention == 2 ? QColor("#3986cc")
                                          : QApplication::palette().color(QPalette::Mid);
    paint.setPen(QPen(color, 2));
    paint.setBrush(attention ? QBrush(color) : Qt::NoBrush);
    paint.drawEllipse(QRectF(3, 3, 10, 10));
    return QIcon(p);
}
SessionModel::SessionModel(QObject *parent) : QStandardItemModel(parent) {
    setHorizontalHeaderLabels({"Projects and sessions"});
}
void SessionModel::updateItem(QStandardItem *item, QMap<int, QVariant> values) {
    // QStandardItemModel::setItemData emits even for an identical map. Send one
    // notification containing only actual changes; don't invalidate hover/sort
    // state for duplicate snapshots or status events.
    for (auto it = values.begin(); it != values.end();)
        if (item->data(it.key()) == it.value())
            it = values.erase(it);
        else
            ++it;
    if (!values.isEmpty())
        setItemData(item->index(), values);
}
void SessionModel::backendName(const QString &id, const QString &name) {
    backendNames[id] = name;
    for (auto *p : projects)
        aggregate(p);
}
QString SessionModel::environment(const QString &backend) const {
    return environments.value(backend, backend);
}
void SessionModel::backendEnvironment(const QString &id, const QString &value) {
    environments[id] = value.isEmpty() ? id : value;
    // Existing rows move on their next upsert; callers refresh their cached metadata.
}
QStandardItem *SessionModel::ensureProject(const QString &backend, const QString &path) {
    const auto key = projectKey(environment(backend), path);
    if (projects.contains(key))
        return projects[key];
    auto *item = new QStandardItem;
    item->setEditable(false);
    item->setData(key, KeyRole);
    item->setData(backend, BackendRole);
    item->setData(environment(backend), EnvironmentRole);
    item->setData(path, PathRole);
    QString name = QFileInfo(path).fileName();
    if (name.isEmpty())
        name = path.isEmpty() ? "Unassigned" : path;
    name = projectNames.value(projectKey(backend, path), name);
    item->setData(name, ProjectNameRole);
    item->setText(name + "  ·  " + backendNames.value(backend, backend));
    item->setToolTip(path);
    item->setData(0, RecencyRole);
    item->setIcon(dot(0));
    appendRow(item);
    projects[key] = item;
    return item;
}
void SessionModel::project(const Project &p) {
    // Legacy display names are retained, but a project exists only with sessions.
    projectNames[projectKey(p.backendId, p.path)] = p.name;
    if (auto *item = projects.value(projectKey(environment(p.backendId), p.path))) {
        item->setData(p.name, ProjectNameRole);
        aggregate(item);
    }
}
void SessionModel::aggregate(QStandardItem *p) {
    int active = 0, running = 0, waiting = 0, unknown = 0, attention = 0;
    qint64 recency = 0;
    QSet<QString> backends;
    for (int i = 0; i < p->rowCount(); ++i) {
        auto *s = p->child(i);
        backends.insert(s->data(BackendRole).toString());
        const int a = s->data(AttentionRole).toInt();
        attention = std::max(attention, a);
        active += s->data(ActiveRole).toBool();
        running += s->data(RunningRole).toBool();
        waiting += s->data(WaitingRole).toBool();
        unknown += a == 0;
        recency = std::max(recency, s->data(RecencyRole).toLongLong());
    }
    auto ids = backends.values();
    ids.sort();
    const auto location = p->data(EnvironmentRole).toString() == "local-host"
                              ? QString("This host")
                              : backendNames.value(ids.value(0), ids.value(0));
    const auto detail =
        QString("%1\n%2 sessions · %3 active/last active · %4 running · %5 waiting · %6 unknown")
            .arg(p->data(PathRole).toString())
            .arg(p->rowCount())
            .arg(active)
            .arg(running)
            .arg(waiting)
            .arg(unknown);
    QMap<int, QVariant> values{
        {ActiveRole, active > 0},
        {RunningRole, running > 0},
        {WaitingRole, waiting > 0},
        {BackendsRole, ids},
        {BackendRole, ids.size() == 1 ? ids.first() : QString()},
        {Qt::DisplayRole, p->data(ProjectNameRole).toString() + "  ·  " + location},
        {RecencyRole, recency},
        {AttentionRole, attention},
        {Qt::ToolTipRole, detail},
        {Qt::AccessibleDescriptionRole, detail}};
    if (p->data(AttentionRole) != attention)
        values[Qt::DecorationRole] = dot(attention);
    updateItem(p, values);
}
void SessionModel::prune(QStandardItem *p) {
    if (p->rowCount())
        aggregate(p);
    else {
        projects.remove(p->data(KeyRole).toString());
        removeRow(p->row());
    }
}
void SessionModel::upsert(const Session &s) {
    const auto key = sessionKey(s.backendId, s.id);
    auto *p = ensureProject(s.backendId, s.cwd);
    auto *item = threads.value(key, nullptr);
    const bool fresh = !item;
    if (fresh) {
        item = new QStandardItem;
        item->setEditable(false);
        threads[key] = item;
    } else if (item->parent() != p) {
        auto *old = item->parent();
        p->appendRow(old->takeRow(item->row()));
        prune(old);
    }
    const auto detail =
        s.stateText() + "\n" + s.cwd + "\n" + backendNames.value(s.backendId) + " · " + s.id;
    QMap<int, QVariant> values{{Qt::DisplayRole, s.title},
                               {KeyRole, key},
                               {BackendRole, s.backendId},
                               {EnvironmentRole, environment(s.backendId)},
                               {PathRole, s.cwd},
                               {ThreadRole, s.id},
                               {ActiveRole, s.isActive()},
                               {RunningRole, s.isRunning()},
                               {WaitingRole, s.isWaiting()},
                               {RecencyRole, std::max(s.opened, s.recency)},
                               {AttentionRole, s.attention()},
                               {Qt::ToolTipRole, detail},
                               {Qt::AccessibleTextRole, s.title + " — " + s.stateText()}};
    if (item->data(AttentionRole) != s.attention())
        values[Qt::DecorationRole] = dot(s.attention());
    if (fresh) {
        for (auto it = values.cbegin(); it != values.cend(); ++it)
            item->setData(it.value(), it.key());
        p->appendRow(item);
    } else
        updateItem(item, values);
    aggregate(p);
}
void SessionModel::remove(const QString &backend, const QString &id) {
    auto *s = threads.take(sessionKey(backend, id));
    if (!s)
        return;
    auto *p = s->parent();
    p->removeRow(s->row());
    prune(p);
}
void SessionModel::removeBackend(const QString &backend) {
    QStringList ids;
    for (auto *thread : threads)
        if (thread->data(BackendRole).toString() == backend)
            ids << thread->data(ThreadRole).toString();
    for (const auto &id : ids)
        remove(backend, id);
}
void SessionModel::resetResults() {
    threads.clear();
    projects.clear();
    removeRows(0, rowCount());
}
SessionFilter::SessionFilter(QObject *parent) : QSortFilterProxyModel(parent) {
    setRecursiveFilteringEnabled(true);
    setDynamicSortFilter(true);
    sort(0, Qt::DescendingOrder);
}
void SessionFilter::setView(View value) {
    if (view == value)
        return;
    view = value;
    invalidate();
}
bool SessionFilter::filterAcceptsRow(int row, const QModelIndex &parent) const {
    if (view == View::All)
        return true;
    const auto role = view == View::Active    ? ActiveRole
                      : view == View::Running ? RunningRole
                                              : WaitingRole;
    return sourceModel()->index(row, 0, parent).data(role).toBool();
}
bool SessionFilter::lessThan(const QModelIndex &a, const QModelIndex &b) const {
    const auto x = a.data(RecencyRole).toLongLong(), y = b.data(RecencyRole).toLongLong();
    if (x != y)
        return x < y;
    return a.data(KeyRole).toString() < b.data(KeyRole).toString();
}
} // namespace Archcodex
