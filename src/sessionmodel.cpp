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
    setHorizontalHeaderLabels({"Hosts, servers, folders and sessions"});
}
void SessionModel::updateItem(QStandardItem *item, QMap<int, QVariant> values) {
    for (auto it = values.begin(); it != values.end();)
        if (item->data(it.key()) == it.value())
            it = values.erase(it);
        else
            ++it;
    if (values.isEmpty())
        return;
    if (item->model())
        setItemData(item->index(), values);
    else
        for (auto it = values.cbegin(); it != values.cend(); ++it)
            item->setData(it.value(), it.key());
}
bool SessionModel::visible(const Session &s) const {
    if (query.scoped && (!query.backends.contains(s.backendId) ||
                         (!query.path.isEmpty() && !within(normalized(s.cwd), query.path))))
        return false;
    if (!query.text.isEmpty() && !s.title.contains(query.text, Qt::CaseInsensitive) &&
        !s.cwd.contains(query.text, Qt::CaseInsensitive) &&
        !s.id.contains(query.text, Qt::CaseInsensitive))
        return false;
    // Opened is an inventory of GUI terminal views, even if another visibility
    // setting excludes their history or the client has exited.
    if (view == SessionView::Opened)
        return s.terminalOpen;
    if (myCodexWorkdirOnly && containerWorkdirs.contains(s.backendId) &&
        !s.inWorkdir(containerWorkdirs.value(s.backendId)))
        return false;
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
    case SessionView::Opened:
        return s.terminalOpen;
    }
    return false;
}
QModelIndex SessionModel::sessionIndex(const QString &key) const {
    const auto *item = threads.value(key);
    return item ? item->index() : QModelIndex{};
}
QModelIndex SessionModel::nodeIndex(const QString &key) const {
    auto *item = threads.value(key, folders.value(key));
    if (!item && key.startsWith("server:"))
        item = roots.value(key.mid(7));
    if (!item && key.startsWith("group:"))
        item = groups.value(key.mid(6));
    return item ? item->index() : QModelIndex{};
}
void SessionModel::setView(SessionView value, const SessionQuery &selection) {
    if (view == value && query == selection)
        return;
    view = value;
    query = selection;
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
void SessionModel::backendGroup(const QString &backend, const QString &group, const QString &label,
                                int order) {
    const bool moved = backendGroups.value(backend) != group;
    backendGroups[backend] = group;
    groupLabels[group] = label;
    groupOrders[group] = order;
    if (moved && roots.contains(backend)) {
        emit topologyAboutToChange();
        placeRoot(backend);
        pruneGroups();
        emit topologyChanged();
    }
    if (auto *item = groups.value(group)) {
        updateItem(item, {{Qt::DisplayRole, label}, {GroupOrderRole, order}});
        aggregate(item);
    }
}
void SessionModel::placeRoot(const QString &backend) {
    auto *root = roots.value(backend);
    const auto key = backendGroups.value(backend);
    if (!root || key.isEmpty())
        return;
    auto *group = groups.value(key);
    if (!group) {
        group = new QStandardItem(groupLabels.value(key));
        group->setEditable(false);
        group->setColumnCount(1);
        group->setData("group:" + key, KeyRole);
        group->setData(int(NodeKind::Group), KindRole);
        group->setData(groupOrders.value(key), GroupOrderRole);
        appendRow(group);
        groups[key] = group;
    }
    if (root->parent() != group) {
        auto *previous = root->parent();
        auto row = previous ? previous->takeRow(root->row()) : takeRow(root->row());
        group->appendRow(row);
        if (previous)
            aggregate(previous);
    }
    aggregate(group);
}
void SessionModel::pruneGroups() {
    for (const auto &key : groups.keys())
        if (!groups[key]->rowCount()) {
            auto *item = groups.take(key);
            removeRow(item->row());
        }
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
        {BackendsRole, QStringList{s.backendId}},
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
    publishAggregate(p, {{CountRole, count},
                         {ActiveCountRole, active},
                         {RunningCountRole, running},
                         {WaitingCountRole, waiting},
                         {UnknownCountRole, unknown},
                         {RecencyRole, recency},
                         {AttentionRole, attention}});
}
void SessionModel::publishAggregate(QStandardItem *p, QMap<int, QVariant> values) {
    const auto backend = p->data(BackendRole).toString();
    const auto path = p->data(PathRole).toString();
    const auto kind = static_cast<NodeKind>(p->data(KindRole).toInt());
    const auto count = values[CountRole].toInt(), active = values[ActiveCountRole].toInt(),
               running = values[RunningCountRole].toInt(),
               waiting = values[WaitingCountRole].toInt(),
               unknown = values[UnknownCountRole].toInt(),
               attention = values[AttentionRole].toInt();
    if (kind != NodeKind::Group)
        values[BackendsRole] = QStringList{backend};
    else {
        QStringList backends;
        for (int i = 0; i < p->rowCount(); ++i)
            backends.append(p->child(i)->data(BackendRole).toString());
        backends.removeDuplicates();
        backends.sort();
        values[BackendsRole] = backends;
    }
    const auto prefix =
        kind == NodeKind::Group ? p->text()
        : kind == NodeKind::Server
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
    values[ActiveRole] = active > 0;
    values[RunningRole] = running > 0;
    values[WaitingRole] = waiting > 0;
    values[Qt::ToolTipRole] = text;
    values[Qt::AccessibleDescriptionRole] = text;
    if (p->data(AttentionRole) != attention)
        values[Qt::DecorationRole] = dot(attention);
    updateItem(p, values);
}
void SessionModel::aggregateAncestors(QStandardItem *item) {
    for (; item; item = item->parent())
        aggregate(item);
}
QStandardItem *SessionModel::sessionParent(const Session &s) const {
    auto *root = roots.value(s.backendId);
    const auto path = normalized(s.cwd);
    return root && root->data(PathRole).toString() == path
               ? root
               : folders.value(folderKey(s.backendId, path));
}
void SessionModel::updateFolder(QStandardItem *item) {
    const auto backend = item->data(BackendRole).toString();
    const auto path = item->data(PathRole).toString();
    const auto relative =
        path.isEmpty() ? QString("Unassigned")
                       : QDir(item->parent()->data(PathRole).toString()).relativeFilePath(path);
    updateItem(item, {{Qt::DisplayRole, projectNames.value(projectKey(backend, path), relative)}});
}
void SessionModel::moveItem(QStandardItem *item, QStandardItem *parent) {
    if (item->parent() == parent)
        return;
    emit branchAboutToMove(item->index());
    parent->appendRow(item->parent()->takeRow(item->row()));
    if (item->data(KindRole).toInt() == int(NodeKind::Folder))
        updateFolder(item);
    emit branchMoved(item->index());
}
QStandardItem *SessionModel::createDirectory(const QString &backend, const QString &path,
                                             qint64 recency) {
    auto *root = roots.value(backend);
    const auto common = root->data(PathRole).toString();
    auto *parent = root;
    if (!path.isEmpty()) {
        auto ancestor = QFileInfo(path).path();
        while (ancestor != common) {
            if (auto *folder = folders.value(folderKey(backend, ancestor))) {
                parent = folder;
                break;
            }
            ancestor = QFileInfo(ancestor).path();
        }
    }
    auto *item = new QStandardItem(0, 1);
    item->setEditable(false);
    const auto label = path.isEmpty()
                           ? QString("Unassigned")
                           : QDir(parent->data(PathRole).toString()).relativeFilePath(path);
    updateItem(item, {{KeyRole, folderKey(backend, path)},
                      {KindRole, int(NodeKind::Folder)},
                      {BackendRole, backend},
                      {PathRole, path},
                      {EnvironmentRole, environment(backend)},
                      {RecencyRole, recency},
                      {Qt::DisplayRole, projectNames.value(projectKey(backend, path), label)}});
    folders[folderKey(backend, path)] = item;
    parent->appendRow(item);
    return item;
}
QStandardItem *SessionModel::ensureDirectory(const QString &backend, const QString &path,
                                             qint64 recency) {
    auto *root = roots.value(backend);
    auto &paths = directoryPaths[backend];
    if (paths.contains(path))
        return path == root->data(PathRole).toString() ? root
                                                       : folders.value(folderKey(backend, path));
    if (path.isEmpty()) {
        paths.insert(path, true);
        return root->data(PathRole).toString().isEmpty() ? root
                                                         : createDirectory(backend, path, recency);
    }
    const auto commonParent = [](QString a, const QString &b) {
        while (!within(b, a))
            a = QFileInfo(a).path();
        return a;
    };
    const auto oldCommon = root->data(PathRole).toString();
    auto first = paths.upperBound(QString()); // Ignore Unassigned.
    const auto absoluteCount = paths.size() - int(paths.contains(QString()));
    const auto common = absoluteCount == 0   ? QFileInfo(path).path()
                        : absoluteCount == 1 ? commonParent(first.key(), path)
                                             : commonParent(oldCommon, path);
    if (common != oldCommon) {
        root->setData(common, PathRole);
        if (oldCommon.isEmpty() && paths.contains(QString())) {
            auto *unassigned = createDirectory(backend, {});
            QList<QStandardItem *> moving;
            for (int i = 0; i < root->rowCount(); ++i) {
                auto *child = root->child(i);
                if (child->data(KindRole).toInt() == int(NodeKind::Session))
                    moving << child;
            }
            for (auto *child : moving)
                moveItem(child, unassigned);
            aggregate(unassigned);
        }
        if (!oldCommon.isEmpty() && within(common, oldCommon)) {
            // The second cwd can turn the first project into the common root.
            if (auto *promoted = folders.take(folderKey(backend, common))) {
                while (promoted->rowCount())
                    moveItem(promoted->child(0), root);
                promoted->parent()->removeRow(promoted->row());
            }
        } else if (!oldCommon.isEmpty() && (absoluteCount > 1 || paths.contains(oldCommon))) {
            // Retain the old branch when the common root moves upwards.
            auto *wrapped =
                createDirectory(backend, oldCommon, root->data(RecencyRole).toLongLong());
            const auto children = root->rowCount();
            QList<QStandardItem *> moving;
            for (int i = 0; i < children; ++i) {
                auto *child = root->child(i);
                if (child != wrapped && !child->data(PathRole).toString().isEmpty())
                    moving << child;
            }
            for (auto *child : moving)
                moveItem(child, wrapped);
            aggregate(wrapped);
        }
        for (int i = 0; i < root->rowCount(); ++i)
            if (root->child(i)->data(KindRole).toInt() == int(NodeKind::Folder))
                updateFolder(root->child(i));
        aggregate(root);
    }
    if (path == common) {
        paths.insert(path, true);
        return root;
    }
    QString neighbour, branch = common;
    auto next = paths.lowerBound(path);
    const auto consider = [&](const QString &candidate) {
        if (candidate.isEmpty())
            return;
        const auto shared = commonParent(path, candidate);
        if (shared.size() > branch.size()) {
            branch = shared;
            neighbour = candidate;
        }
    };
    // A sibling such as /a-b sorts before descendants of /a. Look up the
    // component-delimited prefix as well when inserting an ancestor directory.
    const auto descendant = paths.lowerBound(path + '/');
    if (descendant != paths.end() && within(descendant.key(), path))
        consider(descendant.key());
    if (next != paths.end())
        consider(next.key());
    if (next != paths.begin())
        consider(std::prev(next).key());
    if (branch != common && !folders.contains(folderKey(backend, branch))) {
        auto *old = neighbour == common ? root : folders.value(folderKey(backend, neighbour));
        auto *created =
            createDirectory(backend, branch, old ? old->data(RecencyRole).toLongLong() : 0);
        // The adjacent cwd identifies the one compressed child being split.
        while (old && old->parent() != created->parent())
            old = old->parent();
        if (old)
            moveItem(old, created);
        aggregate(created);
    }
    paths.insert(path, true);
    auto *item = folders.value(folderKey(backend, path));
    return item ? item : createDirectory(backend, path, recency);
}
void SessionModel::addAggregates(QStandardItem *parent, const QList<const Session *> &added) {
    // Additions cannot lower a maximum. Existing-row changes/removals use the
    // general aggregate path; appending history never rescans all siblings.
    QMap<int, QVariant> values;
    for (int role : {CountRole, ActiveCountRole, RunningCountRole, WaitingCountRole,
                     UnknownCountRole, RecencyRole, AttentionRole})
        values[role] = parent->data(role);
    for (const auto *s : added) {
        for (const auto &[role, increment] : {std::pair{CountRole, 1},
                                              {ActiveCountRole, int(s->isActive())},
                                              {RunningCountRole, int(s->isRunning())},
                                              {WaitingCountRole, int(s->isWaiting())},
                                              {UnknownCountRole, int(!s->known)}})
            values[role] = values[role].toInt() + increment;
        values[RecencyRole] =
            std::max(values[RecencyRole].toLongLong(), std::max(s->opened, s->recency));
        values[AttentionRole] = std::max(values[AttentionRole].toInt(), s->attention());
    }
    // Use the same presentation code as full aggregation, with cached totals.
    publishAggregate(parent, values);
}
void SessionModel::rebuild(const QString &backend, const QSet<QString> *changed) {
    // Membership/path changes come here. Other status/title updates retain every index.
    emit topologyAboutToChange();
    auto *root = roots.value(backend);
    if (!root) {
        root = new QStandardItem(0, 1);
        root->setEditable(false);
        root->setData("server:" + backend, KeyRole);
        root->setData(backend, BackendRole);
        root->setData(int(NodeKind::Server), KindRole);
        appendRow(root);
        roots[backend] = root;
        placeRoot(backend);
    }
    root->setText(backendNames.value(backend, backend));
    root->setData(environment(backend), EnvironmentRole);
    QSet<QString> paths;
    auto &indexedPaths = directoryPaths[backend];
    indexedPaths.clear();
    QList<Session> sessions;
    for (const auto &key : backendRecords.value(backend)) {
        const auto &s = records[key];
        if (visible(s)) {
            sessions << s;
            paths.insert(normalized(s.cwd));
            indexedPaths.insert(normalized(s.cwd), true);
        }
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
            item = new QStandardItem(0, 1);
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
        const bool inserted = !item;
        if (!item) {
            item = new QStandardItem(0, 1);
            item->setEditable(false);
            item->setFlags(item->flags() | Qt::ItemNeverHasChildren);
            updateSession(item, s);
            parent->appendRow(item);
            threads[key] = item;
        } else if (item->parent() != parent)
            parent->appendRow(item->parent()->takeRow(item->row()));
        if (inserted || !changed || changed->contains(key))
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
    aggregateAncestors(root);
    emit topologyChanged();
}
void SessionModel::upsert(const Session &s) {
    upsertMany({s});
}
void SessionModel::remove(const QString &backend, const QString &id) {
    const auto key = sessionKey(backend, id);
    if (records.remove(key)) {
        backendRecords[backend].remove(key);
        rebuild(backend);
    }
}
void SessionModel::upsertMany(const QList<Session> &items) {
    QSet<QString> structural, changed;
    for (const auto &s : items) {
        const auto key = sessionKey(s.backendId, s.id);
        const auto old = records.value(key);
        const bool wasVisible = records.contains(key) && visible(old);
        const bool isVisible = visible(s);
        // Existing folders can accept rows without regrouping or invalidating
        // navigation. Removing/moving a visible row may compact its old path.
        if (!roots.contains(s.backendId) ||
            (wasVisible && (!isVisible || normalized(old.cwd) != normalized(s.cwd))))
            structural.insert(s.backendId);
        records[key] = s;
        backendRecords[s.backendId].insert(key);
        changed.insert(key);
        if (!backendNames.contains(s.backendId))
            backendNames[s.backendId] = s.backendId;
    }
    for (const auto &backend : structural)
        rebuild(backend, &changed);
    // Prepare paths before publishing any new session rows. Branch moves then
    // operate only on previously aggregated children.
    QHash<QString, qint64> directoryRecency;
    for (const auto &key : changed) {
        const auto &s = records[key];
        if (!structural.contains(s.backendId) && visible(s)) {
            auto &recency = directoryRecency[folderKey(s.backendId, normalized(s.cwd))];
            recency = std::max(recency, std::max(s.opened, s.recency));
        }
    }
    for (const auto &key : changed) {
        const auto &s = records[key];
        if (!structural.contains(s.backendId) && visible(s))
            ensureDirectory(s.backendId, normalized(s.cwd),
                            directoryRecency.value(folderKey(s.backendId, normalized(s.cwd))));
    }
    QSet<QStandardItem *> parents, recalculate;
    QHash<QStandardItem *, QList<const Session *>> additions;
    QHash<QStandardItem *, QList<QStandardItem *>> rows;
    for (const auto &key : changed) {
        const auto &s = records[key];
        if (structural.contains(s.backendId) || !visible(s))
            continue;
        auto *item = threads.value(key);
        const bool inserted = !item;
        auto *parent = sessionParent(s);
        if (inserted) {
            item = new QStandardItem(0, 1);
            item->setEditable(false);
            item->setFlags(item->flags() | Qt::ItemNeverHasChildren);
            rows[parent] << item;
            threads[key] = item;
        }
        updateSession(item, s);
        for (auto *p = parent; p; p = p->parent()) {
            parents.insert(p);
            if (inserted)
                additions[p] << &s;
            else
                recalculate.insert(p);
        }
    }
    for (auto it = rows.cbegin(); it != rows.cend(); ++it)
        it.key()->appendRows(it.value());
    auto ordered = parents.values();
    auto depth = [](QStandardItem *item) {
        int d = 0;
        for (; item; item = item->parent())
            ++d;
        return d;
    };
    std::sort(ordered.begin(), ordered.end(),
              [&](auto *a, auto *b) { return depth(a) > depth(b); });
    for (auto *p : ordered) {
        if (recalculate.contains(p))
            aggregate(p);
        else
            addAggregates(p, additions.value(p));
    }
}
void SessionModel::removeBackend(const QString &backend) {
    emit topologyAboutToChange();
    directoryPaths.remove(backend);
    for (const auto &key : backendRecords.take(backend))
        records.remove(key);
    for (const auto &key : threads.keys())
        if (threads[key]->data(BackendRole) == backend)
            threads.remove(key);
    for (const auto &key : folders.keys())
        if (folders[key]->data(BackendRole) == backend)
            folders.remove(key);
    if (auto *root = roots.take(backend)) {
        if (auto *parent = root->parent()) {
            parent->removeRow(root->row());
            aggregate(parent);
        } else
            removeRow(root->row());
    }
    backendNames.remove(backend);
    details.remove(backend);
    environments.remove(backend);
    containerWorkdirs.remove(backend);
    backendGroups.remove(backend);
    pruneGroups();
    emit topologyChanged();
}
void SessionModel::resetResults() {
    emit topologyAboutToChange();
    records.clear();
    directoryPaths.clear();
    backendRecords.clear();
    roots.clear();
    groups.clear();
    folders.clear();
    threads.clear();
    clear(); // One reset, rather than thousands of proxy removal/index updates.
    setHorizontalHeaderLabels({"Hosts, servers, folders and sessions"});
    emit topologyChanged();
}
SessionFilter::SessionFilter(QObject *parent) : QSortFilterProxyModel(parent) {
    setSortRole(RecencyRole);
    // Membership and ancestor counts are maintained by SessionModel.
    setRecursiveFilteringEnabled(false);
    setDynamicSortFilter(true);
    sort(0, Qt::DescendingOrder);
}
void SessionFilter::setView(View value, const SessionQuery &query) {
    if (auto *model = qobject_cast<SessionModel *>(sourceModel()))
        model->setView(value, query);
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
    // Host groups stay in a predictable order while sessions beneath them use
    // recency. Comparisons are reversed here because the proxy sorts descending.
    if (a.data(KindRole).toInt() == int(NodeKind::Group) &&
        b.data(KindRole).toInt() == int(NodeKind::Group)) {
        if (a.data(GroupOrderRole) != b.data(GroupOrderRole))
            return a.data(GroupOrderRole).toInt() > b.data(GroupOrderRole).toInt();
        const auto name = QString::localeAwareCompare(a.data().toString(), b.data().toString());
        return name ? name > 0 : a.data(KeyRole).toString() > b.data(KeyRole).toString();
    }
    const auto x = a.data(RecencyRole).toLongLong(), y = b.data(RecencyRole).toLongLong();
    if (x != y)
        return x < y;
    return a.data(KeyRole).toString() < b.data(KeyRole).toString();
}
} // namespace Archcodex
