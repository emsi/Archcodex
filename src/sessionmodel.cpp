// SPDX-License-Identifier: GPL-3.0-or-later
#include "sessionmodel.h"
#include <QApplication>
#include <QFileInfo>
#include <QPainter>
#include <QPalette>

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
void SessionModel::backendName(const QString &id, const QString &name) {
    backendNames[id] = name;
    for (auto *p : projects)
        if (p->data(BackendRole).toString() == id)
            p->setText(p->data(ProjectNameRole).toString() + "  ·  " + name);
}
QStandardItem *SessionModel::ensureProject(const QString &backend, const QString &path) {
    const auto key = projectKey(backend, path);
    if (projects.contains(key))
        return projects[key];
    auto *item = new QStandardItem;
    item->setEditable(false);
    item->setData(key, KeyRole);
    item->setData(backend, BackendRole);
    item->setData(path, PathRole);
    QString name = QFileInfo(path).fileName();
    if (name.isEmpty())
        name = path.isEmpty() ? "Unassigned" : path;
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
    auto *item = ensureProject(p.backendId, p.path);
    item->setData(p.name, ProjectNameRole);
    item->setText(p.name + "  ·  " + backendNames.value(p.backendId, p.backendId));
}
void SessionModel::aggregate(QStandardItem *p) {
    int active = 0, waiting = 0, unknown = 0, attention = 0;
    qint64 recency = 0;
    for (int i = 0; i < p->rowCount(); ++i) {
        auto *s = p->child(i);
        const int a = s->data(AttentionRole).toInt();
        attention = std::max(attention, a);
        active += s->data(ActiveRole).toBool();
        waiting += a == 4;
        unknown += a == 0;
        recency = std::max(recency, s->data(RecencyRole).toLongLong());
    }
    p->setData(active > 0, ActiveRole);
    p->setData(recency, RecencyRole);
    p->setIcon(dot(attention));
    const auto detail = QString("%1\n%2 sessions · %3 active/last active · %4 waiting · %5 unknown")
                            .arg(p->data(PathRole).toString())
                            .arg(p->rowCount())
                            .arg(active)
                            .arg(waiting)
                            .arg(unknown);
    p->setToolTip(detail);
    p->setData(detail, Qt::AccessibleDescriptionRole);
}
void SessionModel::upsert(const Session &s) {
    const auto key = sessionKey(s.backendId, s.id);
    auto *p = ensureProject(s.backendId, s.cwd);
    auto *item = threads.value(key, nullptr);
    if (!item) {
        item = new QStandardItem;
        item->setEditable(false);
        p->appendRow(item);
        threads[key] = item;
    } else if (item->parent() != p) {
        auto *old = item->parent();
        p->appendRow(old->takeRow(item->row()));
        aggregate(old);
    }
    item->setText(s.title);
    item->setIcon(dot(s.attention()));
    item->setData(key, KeyRole);
    item->setData(s.backendId, BackendRole);
    item->setData(s.cwd, PathRole);
    item->setData(s.id, ThreadRole);
    item->setData(s.isActive(), ActiveRole);
    item->setData(std::max(s.opened, s.recency), RecencyRole);
    item->setData(s.attention(), AttentionRole);
    const auto detail =
        s.stateText() + "\n" + s.cwd + "\n" + backendNames.value(s.backendId) + " · " + s.id;
    item->setToolTip(detail);
    item->setData(s.title + " — " + s.stateText(), Qt::AccessibleTextRole);
    aggregate(p);
}
void SessionModel::remove(const QString &backend, const QString &id) {
    auto *s = threads.take(sessionKey(backend, id));
    if (!s)
        return;
    auto *p = s->parent();
    p->removeRow(s->row());
    aggregate(p);
}
void SessionModel::removeBackend(const QString &backend) {
    for (auto it = threads.begin(); it != threads.end();)
        if (it.value()->data(BackendRole).toString() == backend)
            it = threads.erase(it);
        else
            ++it;
    for (auto it = projects.begin(); it != projects.end();)
        if (it.value()->data(BackendRole).toString() == backend) {
            auto *p = it.value();
            it = projects.erase(it);
            removeRow(p->row());
        } else
            ++it;
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
void SessionFilter::setActiveOnly(bool active) {
    activeOnly = active;
    invalidate();
}
bool SessionFilter::filterAcceptsRow(int row, const QModelIndex &parent) const {
    if (!activeOnly)
        return true;
    return sourceModel()->index(row, 0, parent).data(ActiveRole).toBool();
}
bool SessionFilter::lessThan(const QModelIndex &a, const QModelIndex &b) const {
    const auto x = a.data(RecencyRole).toLongLong(), y = b.data(RecencyRole).toLongLong();
    if (x != y)
        return x < y;
    return a.data(KeyRole).toString() < b.data(KeyRole).toString();
}
} // namespace Archcodex
