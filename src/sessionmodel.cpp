// SPDX-License-Identifier: GPL-3.0-or-later
#include "sessionmodel.h"
#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QPainter>
#include <QPalette>
#include <QSet>
#include <algorithm>

namespace Archcodex {
static QIcon dot(int attention) {
    QPixmap p(16, 16);
    p.fill(Qt::transparent);
    QPainter paint(&p);
    paint.setRenderHint(QPainter::Antialiasing);
    const QColor color = attention == 4   ? QColor("#27ae60")
                         : attention == 3 ? QColor("#c64b4b")
                         : attention == 2 ? QColor("#3986cc")
                                          : QApplication::palette().color(QPalette::Mid);
    paint.setPen(QPen(color, 2));
    paint.setBrush(attention ? QBrush(color) : Qt::NoBrush);
    paint.drawEllipse(QRectF(3, 3, 10, 10));
    // These colours communicate state. Supplying every mode prevents the style
    // from tinting an error dot when a row is selected or gains keyboard focus.
    QIcon icon;
    for (const auto mode : {QIcon::Normal, QIcon::Selected, QIcon::Active, QIcon::Disabled})
        for (const auto state : {QIcon::Off, QIcon::On})
            icon.addPixmap(p, mode, state);
    return icon;
}
static QString folderKey(const QString &backend, const QString &path) {
    return "folder:" + projectKey(backend, path);
}
static bool within(const QString &path, const QString &parent) {
    return path == parent || path.startsWith(parent == "/" ? "/" : parent + "/");
}
static QString normalized(const QString &path) {
    return QDir::isAbsolutePath(path) ? QDir::cleanPath(path) : QString();
}
SessionModel::SessionModel(QObject *parent) : QStandardItemModel(parent) {
    setHorizontalHeaderLabels({"Servers, folders and sessions"});
}
void SessionModel::updateItem(QStandardItem *item, QMap<int, QVariant> values) {
    for (auto it = values.begin(); it != values.end();)
        if (item->data(it.key()) == it.value())
            it = values.erase(it);
        else
            ++it;
    if (!values.isEmpty())
        setItemData(item->index(), values);
}
bool SessionModel::visible(const Session &s) const {
    if (myCodexWorkdirOnly && containerWorkdirs.contains(s.backendId)) {
        const auto workdir = containerWorkdirs.value(s.backendId);
        const auto path = normalized(s.cwd);
        if (workdir.isEmpty() || path.isEmpty() || !within(path, workdir))
            return false;
    }
    if (!s.terminalOpen && hideUnsaved && (s.unavailable || (s.persistenceKnown && !s.isSaved())))
        return false;
    switch (view) {
    case SessionView::Active:
        return s.isActive();
    case SessionView::Running:
        return s.isRunning();
    case SessionView::Waiting:
        return s.isWaiting();
    case SessionView::All:
        return true;
    }
    return false;
}
void SessionModel::setView(SessionView value) {
    if (view == value)
        return;
    view = value;
    // Build paths and aggregates from the selected sessions, not the full history.
    // Records remain intact so switching views never discards metadata or clients.
    for (const auto &backend : backendNames.keys())
        rebuild(backend);
}
void SessionModel::setHideUnsaved(bool hide) {
    if (hideUnsaved == hide)
        return;
    hideUnsaved = hide;
    for (const auto &backend : backendNames.keys())
        rebuild(backend);
}
void SessionModel::setContainerWorkdir(const QString &backend, const QString &path) {
    const auto workdir = normalized(path);
    if (containerWorkdirs.contains(backend) && containerWorkdirs.value(backend) == workdir)
        return;
    containerWorkdirs[backend] = workdir;
    if (myCodexWorkdirOnly)
        rebuild(backend);
}
void SessionModel::setMyCodexWorkdirOnly(bool enabled) {
    if (myCodexWorkdirOnly == enabled)
        return;
    myCodexWorkdirOnly = enabled;
    for (const auto &backend : containerWorkdirs.keys())
        rebuild(backend);
}
void SessionModel::backendName(const QString &id, const QString &name) {
    backendNames[id] = name;
    if (auto *root = roots.value(id)) {
        updateItem(root, {{Qt::DisplayRole, name}});
        aggregate(root);
    } else
        rebuild(id);
}
void SessionModel::backendDetails(const QString &id, const QString &value) {
    details[id] = value;
    if (auto *root = roots.value(id))
        aggregate(root);
}
QString SessionModel::environment(const QString &backend) const {
    return environments.value(backend, backend);
}
void SessionModel::backendEnvironment(const QString &id, const QString &value) {
    const auto environment = value.isEmpty() ? id : value;
    if (environments.value(id) == environment)
        return;
    environments[id] = environment;
    if (roots.contains(id))
        rebuild(id);
}
void SessionModel::project(const Project &p) {
    projectNames[projectKey(p.backendId, p.path)] = p.name;
    rebuild(p.backendId);
}
void SessionModel::updateSession(QStandardItem *item, const Session &s) {
    const auto persistence = s.unavailable        ? "Saved session unavailable"
                             : s.isSaved()        ? "Saved session"
                             : s.persistenceKnown ? "Unsaved session"
                                                  : "Persistence unknown";
    const auto text = s.stateText() + "\n" + s.cwd + "\n" + persistence + "\n" + s.id;
    QMap<int, QVariant> values{
        {Qt::DisplayRole,
         s.title + (s.contextRequired && !s.executionObserved ? " · History only" : "")},
        {KeyRole, sessionKey(s.backendId, s.id)},
        {KindRole, int(NodeKind::Session)},
        {BackendRole, s.backendId},
        {EnvironmentRole, environment(s.backendId)},
        {PathRole, s.cwd},
        {ThreadRole, s.id},
        {ActiveRole, s.isActive()},
        {RunningRole, s.isRunning()},
        {WaitingRole, s.isWaiting()},
        {RecencyRole, std::max(s.opened, s.recency)},
        {AttentionRole, s.attention()},
        {CountRole, 1},
        {ActiveCountRole, int(s.isActive())},
        {RunningCountRole, int(s.isRunning())},
        {WaitingCountRole, int(s.isWaiting())},
        {UnknownCountRole, int(!s.known)},
        {Qt::ToolTipRole, text},
        {Qt::AccessibleTextRole, s.title + " — " + s.stateText()}};
    if (item->data(AttentionRole) != s.attention())
        values[Qt::DecorationRole] = dot(s.attention());
    updateItem(item, values);
}
void SessionModel::aggregate(QStandardItem *p) {
    int count = 0, active = 0, running = 0, waiting = 0, unknown = 0, attention = 0;
    qint64 recency = 0;
    for (int i = 0; i < p->rowCount(); ++i) {
        const auto *s = p->child(i);
        count += s->data(CountRole).toInt();
        active += s->data(ActiveCountRole).toInt();
        running += s->data(RunningCountRole).toInt();
        waiting += s->data(WaitingCountRole).toInt();
        unknown += s->data(UnknownCountRole).toInt();
        attention = std::max(attention, s->data(AttentionRole).toInt());
        recency = std::max(recency, s->data(RecencyRole).toLongLong());
    }
    const auto backend = p->data(BackendRole).toString();
    const auto path = p->data(PathRole).toString();
    const auto prefix =
        p->data(KindRole).toInt() == int(NodeKind::Server)
            ? details.value(backend, backendNames.value(backend)) + "\nCommon folder: " + path
            : path;
    const auto text =
        prefix +
        QString("\n%1 sessions · %2 active/last active · %3 running · %4 waiting · %5 unknown")
            .arg(count)
            .arg(active)
            .arg(running)
            .arg(waiting)
            .arg(unknown);
    QMap<int, QVariant> values{{CountRole, count},          {ActiveCountRole, active},
                               {RunningCountRole, running}, {WaitingCountRole, waiting},
                               {UnknownCountRole, unknown}, {ActiveRole, active > 0},
                               {RunningRole, running > 0},  {WaitingRole, waiting > 0},
                               {RecencyRole, recency},      {AttentionRole, attention},
                               {Qt::ToolTipRole, text},     {Qt::AccessibleDescriptionRole, text}};
    if (p->data(AttentionRole) != attention)
        values[Qt::DecorationRole] = dot(attention);
    updateItem(p, values);
}
void SessionModel::rebuild(const QString &backend) {
    // Membership/path changes come here. Other status/title updates retain every index.
    emit topologyAboutToChange();
    auto *root = roots.value(backend);
    if (!root) {
        root = new QStandardItem;
        root->setEditable(false);
        root->setData("server:" + backend, KeyRole);
        root->setData(backend, BackendRole);
        root->setData(int(NodeKind::Server), KindRole);
        appendRow(root);
        roots[backend] = root;
    }
    root->setText(backendNames.value(backend, backend));
    root->setData(environment(backend), EnvironmentRole);
    QSet<QString> paths;
    QList<Session> sessions;
    for (const auto &s : records)
        if (s.backendId == backend && visible(s)) {
            sessions << s;
            paths.insert(normalized(s.cwd));
        }
    QStringList absolutePaths = paths.values();
    absolutePaths.removeAll(QString());
    absolutePaths.sort();
    QString common = absolutePaths.isEmpty() ? QString() : absolutePaths.first();
    for (const auto &path : absolutePaths)
        while (!within(path, common))
            common = QFileInfo(common).path();
    // A single project should retain its own folder label under the server.
    if (absolutePaths.size() == 1)
        common = QFileInfo(common).path();
    root->setData(common, PathRole);
    QMap<QString, QSet<QString>> children;
    for (const auto &path : absolutePaths) {
        auto child = path;
        while (child != common) {
            const auto parent = QFileInfo(child).path();
            children[parent].insert(child);
            child = parent;
        }
    }
    QSet<QString> retained = paths;
    retained.remove(common);
    for (auto it = children.cbegin(); it != children.cend(); ++it)
        if (it.key() != common && it.value().size() > 1)
            retained.insert(it.key());
    auto ordered = retained.values();
    std::sort(ordered.begin(), ordered.end(), [](const auto &a, const auto &b) {
        return a.size() == b.size() ? a < b : a.size() < b.size();
    });
    QSet<QString> wantedFolders, wantedThreads;
    for (const auto &path : ordered) {
        const auto key = folderKey(backend, path);
        wantedFolders.insert(key);
        auto *item = folders.value(key);
        QString parentPath = path.isEmpty() ? common : QFileInfo(path).path();
        while (parentPath != common && !retained.contains(parentPath))
            parentPath = QFileInfo(parentPath).path();
        auto *parent = parentPath == common ? root : folders.value(folderKey(backend, parentPath));
        if (!item) {
            item = new QStandardItem;
            item->setEditable(false);
            parent->appendRow(item);
            folders[key] = item;
        } else if (item->parent() != parent)
            parent->appendRow(item->parent()->takeRow(item->row()));
        const auto relative =
            path.isEmpty() ? QString("Unassigned") : QDir(parentPath).relativeFilePath(path);
        const auto name = projectNames.value(projectKey(backend, path), relative);
        updateItem(item, {{KeyRole, key},
                          {KindRole, int(NodeKind::Folder)},
                          {BackendRole, backend},
                          {PathRole, path},
                          {EnvironmentRole, environment(backend)},
                          {Qt::DisplayRole, name}});
    }
    for (const auto &s : sessions) {
        const auto key = sessionKey(backend, s.id);
        wantedThreads.insert(key);
        const auto path = normalized(s.cwd);
        auto *parent = path == common ? root : folders.value(folderKey(backend, path));
        auto *item = threads.value(key);
        if (!item) {
            item = new QStandardItem;
            item->setEditable(false);
            parent->appendRow(item);
            threads[key] = item;
        } else if (item->parent() != parent)
            parent->appendRow(item->parent()->takeRow(item->row()));
        updateSession(item, s);
    }
    // Move retained descendants first, then remove obsolete leaves/branches.
    for (const auto &key : threads.keys()) {
        auto *item = threads[key];
        if (item->data(BackendRole) == backend && !wantedThreads.contains(key)) {
            threads.remove(key);
            item->parent()->removeRow(item->row());
        }
    }
    QList<QStandardItem *> obsolete;
    for (auto it = folders.cbegin(); it != folders.cend(); ++it)
        if (it.value()->data(BackendRole) == backend && !wantedFolders.contains(it.key()))
            obsolete << it.value();
    std::sort(obsolete.begin(), obsolete.end(), [](auto *a, auto *b) {
        return a->data(PathRole).toString().size() > b->data(PathRole).toString().size();
    });
    for (auto *item : obsolete) {
        folders.remove(item->data(KeyRole).toString());
        item->parent()->removeRow(item->row());
    }
    std::reverse(ordered.begin(), ordered.end());
    for (const auto &path : ordered)
        aggregate(folders.value(folderKey(backend, path)));
    aggregate(root);
    emit topologyChanged();
}
void SessionModel::upsert(const Session &s) {
    const auto key = sessionKey(s.backendId, s.id);
    const auto old = records.value(key);
    const bool structural = !records.contains(key) || normalized(old.cwd) != normalized(s.cwd) ||
                            visible(old) != visible(s);
    records[key] = s;
    if (!backendNames.contains(s.backendId))
        backendNames[s.backendId] = s.backendId;
    if (structural)
        rebuild(s.backendId);
    else if (auto *item = threads.value(key)) {
        updateSession(item, s);
        for (auto *parent = item->parent(); parent; parent = parent->parent())
            aggregate(parent);
    }
}
void SessionModel::remove(const QString &backend, const QString &id) {
    if (records.remove(sessionKey(backend, id)))
        rebuild(backend);
}
void SessionModel::upsertMany(const QList<Session> &items) {
    QSet<QString> structural;
    for (const auto &s : items) {
        const auto key = sessionKey(s.backendId, s.id);
        const auto old = records.value(key);
        if (!records.contains(key) || normalized(old.cwd) != normalized(s.cwd) ||
            visible(old) != visible(s))
            structural.insert(s.backendId);
        records[key] = s;
        if (!backendNames.contains(s.backendId))
            backendNames[s.backendId] = s.backendId;
    }
    for (const auto &backend : structural)
        rebuild(backend);
    QSet<QStandardItem *> parents;
    for (const auto &s : items)
        if (!structural.contains(s.backendId))
            if (auto *item = threads.value(sessionKey(s.backendId, s.id))) {
                updateSession(item, s);
                for (auto *p = item->parent(); p; p = p->parent())
                    parents.insert(p);
            }
    auto ordered = parents.values();
    auto depth = [](QStandardItem *item) {
        int d = 0;
        for (; item; item = item->parent())
            ++d;
        return d;
    };
    std::sort(ordered.begin(), ordered.end(),
              [&](auto *a, auto *b) { return depth(a) > depth(b); });
    for (auto *p : ordered)
        aggregate(p);
}
void SessionModel::removeBackend(const QString &backend) {
    emit topologyAboutToChange();
    for (const auto &key : records.keys())
        if (records[key].backendId == backend)
            records.remove(key);
    for (const auto &key : threads.keys())
        if (threads[key]->data(BackendRole) == backend)
            threads.remove(key);
    for (const auto &key : folders.keys())
        if (folders[key]->data(BackendRole) == backend)
            folders.remove(key);
    if (auto *root = roots.take(backend))
        removeRow(root->row());
    backendNames.remove(backend);
    details.remove(backend);
    environments.remove(backend);
    containerWorkdirs.remove(backend);
    emit topologyChanged();
}
void SessionModel::resetResults() {
    emit topologyAboutToChange();
    records.clear();
    roots.clear();
    folders.clear();
    threads.clear();
    removeRows(0, rowCount());
    emit topologyChanged();
}
SessionFilter::SessionFilter(QObject *parent) : QSortFilterProxyModel(parent) {
    setRecursiveFilteringEnabled(true);
    setDynamicSortFilter(true);
    sort(0, Qt::DescendingOrder);
}
void SessionFilter::setView(View value) {
    if (auto *model = qobject_cast<SessionModel *>(sourceModel()))
        model->setView(value);
    invalidate();
}
bool SessionFilter::filterAcceptsRow(int row, const QModelIndex &parent) const {
    const auto *model = qobject_cast<const SessionModel *>(sourceModel());
    // SessionModel filters before grouping. The proxy only sorts and hides empty
    // configured server roots outside All; it never leaves historical branches.
    return !model || model->currentView() == View::All ||
           sourceModel()->index(row, 0, parent).data(CountRole).toInt() > 0;
}
bool SessionFilter::lessThan(const QModelIndex &a, const QModelIndex &b) const {
    const auto x = a.data(RecencyRole).toLongLong(), y = b.data(RecencyRole).toLongLong();
    if (x != y)
        return x < y;
    return a.data(KeyRole).toString() < b.data(KeyRole).toString();
}
} // namespace Archcodex
