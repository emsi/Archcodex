// SPDX-License-Identifier: GPL-3.0-or-later
#include "window.h"
#include "discovery.h"
#include "sessionmodel.h"
#include "terminal.h"
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QHelpEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStatusBar>
#include <QTabBar>
#include <QToolTip>
#include <QTreeView>
#include <QUuid>
#include <QVBoxLayout>
#include <algorithm>

namespace Archcodex {
namespace {
// Keep help beside the navigator, anchored to the row rather than the pointer,
// and keep its text stable while metadata changes underneath it.
class SessionTree : public QTreeView {
    QPersistentModelIndex tooltipIndex;

  protected:
    bool viewportEvent(QEvent *event) override {
        if (event->type() != QEvent::ToolTip)
            return QTreeView::viewportEvent(event);
        auto *help = static_cast<QHelpEvent *>(event);
        const auto index = indexAt(help->pos());
        if (!index.isValid()) {
            QToolTip::hideText();
            tooltipIndex = QPersistentModelIndex();
            return true;
        }
        if (tooltipIndex != index || !QToolTip::isVisible()) {
            tooltipIndex = index;
            const auto rect = visualRect(index);
            const auto text =
                index.data(Qt::ToolTipRole).toString().toHtmlEscaped().replace('\n', "<br>");
            QToolTip::showText(viewport()->mapToGlobal(QPoint(viewport()->width() + 8, rect.top())),
                               "<p>" + text + "</p>", viewport(), rect);
        }
        return true;
    }
};
void walkTree(QAbstractItemModel *model, const QModelIndex &parent,
              const std::function<void(const QModelIndex &)> &visit) {
    for (int i = 0; i < model->rowCount(parent); ++i) {
        const auto child = model->index(i, 0, parent);
        visit(child);
        walkTree(model, child, visit);
    }
}
QStringList expandedKeys(QTreeView *tree) {
    QStringList keys;
    walkTree(tree->model(), {}, [&](const QModelIndex &index) {
        if (tree->isExpanded(index))
            keys << index.data(KeyRole).toString();
    });
    return keys;
}
void restoreTree(QTreeView *tree, const QStringList &keys, const QString &selected) {
    walkTree(tree->model(), {}, [&](const QModelIndex &index) {
        if (keys.contains(index.data(KeyRole).toString()))
            tree->expand(index);
        if (!selected.isEmpty() && index.data(KeyRole).toString() == selected) {
            tree->setCurrentIndex(index);
            for (auto parent = index.parent(); parent.isValid(); parent = parent.parent())
                tree->expand(parent);
        }
    });
}
bool expandsByDefault(int index) {
    return index == int(SessionView::Running) || index == int(SessionView::Waiting);
}
const QStringList viewDescriptions{
    "All sessions, including saved history. Hollow dots mean unknown state.",
    "Live sessions, including idle prompts and errors. Hollow dots mean last-known active.",
    "Sessions currently executing a turn.",
    "Live sessions ready for a prompt, an answer or an approval."};
} // namespace
Window::Window(const QString &directory, bool connectOnStart) : dataDirectory(directory) {
    if (!QDir(directory).exists()) {
        if (!QDir().mkpath(directory))
            throw std::runtime_error("Cannot create the application data directory.");
        QFile::setPermissions(directory, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    }
    store = std::make_unique<Store>(directory + "/metadata.sqlite");
    setWindowTitle("Archcodex");
    setWindowIcon(QIcon::fromTheme("io.github.emsi.Archcodex"));
    resize(1150, 760);
    model = new SessionModel(this);
    results = new SessionModel(this);
    filter = new SessionFilter(this);
    filter->setSourceModel(model);
    splitter = new QSplitter(this);
    setCentralWidget(splitter);
    auto *sidebar = new QWidget;
    sidebar->setMinimumWidth(240);
    auto *left = new QVBoxLayout(sidebar);
    auto *heading = new QHBoxLayout;
    auto *label = new QLabel("Projects");
    auto f = label->font();
    f.setBold(true);
    label->setFont(f);
    heading->addWidget(label);
    heading->addStretch();
    auto *discoverButton = new QPushButton("Discover");
    discoverButton->setToolTip("Discover existing local Codex servers and sessions");
    heading->addWidget(discoverButton);
    left->addLayout(heading);
    connect(discoverButton, &QPushButton::clicked, this, &Window::discover);
    tabs = new QTabBar;
    tabs->addTab("All");
    tabs->addTab("Active");
    tabs->addTab("Running");
    tabs->addTab("Waiting");
    for (int i = 0; i < tabs->count(); ++i)
        tabs->setTabToolTip(i, viewDescriptions[i]);
    tabs->setExpanding(false);
    left->addWidget(tabs);
    searchBox = new QLineEdit;
    searchBox->setPlaceholderText("Search conversation text — Enter");
    searchBox->setAccessibleName("Search conversations");
    searchBox->setClearButtonEnabled(true);
    left->addWidget(searchBox);
    scope = new QComboBox;
    scope->addItems({"All servers", "Selected server or folder"});
    scope->setAccessibleName("Search scope");
    left->addWidget(scope);
    connect(searchBox, &QLineEdit::returnPressed, this, &Window::search);
    connect(searchBox, &QLineEdit::textChanged, this, [this](const QString &text) {
        if (text.isEmpty())
            clearSearch();
    });
    notice = new QLabel("Discovering existing local Codex sessions…");
    notice->setTextFormat(Qt::PlainText);
    notice->setWordWrap(true);
    left->addWidget(notice);
    tree = new SessionTree;
    tree->setModel(filter);
    tree->setHeaderHidden(true);
    tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tree->setUniformRowHeights(true);
    tree->setAnimated(false);
    tree->setAccessibleName("Projects and sessions");
    left->addWidget(tree, 1);
    connect(tree, &QTreeView::activated, this, &Window::activate);
    connect(tabs, &QTabBar::currentChanged, this, [this](int index) {
        const QSignalBlocker block(tree);
        const auto saved = expandedKeys(tree);
        expanded[previousTab] = saved;
        const auto selectedKey = tree->currentIndex().data(KeyRole).toString();
        filter->setView(static_cast<SessionFilter::View>(index));
        tree->collapseAll();
        restoreTree(tree, expanded.value(index, saved), selectedKey);
        expandActivityViews();
        previousTab = index;
        if (!searchingView)
            updateSummary();
    });
    connect(tree, &QTreeView::collapsed, this, [this](const QModelIndex &index) {
        if (!searchingView && expandsByDefault(tabs->currentIndex()))
            collapsed[tabs->currentIndex()].insert(index.data(KeyRole).toString());
    });
    connect(tree, &QTreeView::expanded, this, [this](const QModelIndex &index) {
        if (!searchingView && expandsByDefault(tabs->currentIndex()))
            collapsed[tabs->currentIndex()].remove(index.data(KeyRole).toString());
    });
    for (auto *source : {model, results}) {
        connect(source, &SessionModel::topologyAboutToChange, this, [this, source] {
            if (filter->sourceModel() == source) {
                topologyExpanded = expandedKeys(tree);
                topologySelected = tree->currentIndex().data(KeyRole).toString();
            }
        });
        connect(source, &SessionModel::topologyChanged, this, [this, source] {
            if (filter->sourceModel() != source)
                return;
            const QSignalBlocker block(tree);
            restoreTree(tree, topologyExpanded, topologySelected);
            for (int i = 0; i < filter->rowCount(); ++i) {
                const auto root = filter->index(i, 0);
                const auto key = root.data(KeyRole).toString();
                if (!seenRoots.contains(key)) {
                    tree->expand(root);
                    seenRoots.insert(key);
                }
            }
            expandActivityViews();
        });
    }
    loadMore = new QPushButton("Load more history");
    loadMore->setEnabled(false);
    left->addWidget(loadMore);
    connect(loadMore, &QPushButton::clicked, this, [this] {
        for (auto *b : backends)
            b->loadMore();
    });
    auto *manage = new QPushButton("Connections…");
    left->addWidget(manage);
    connect(manage, &QPushButton::clicked, this, &Window::connectionsDialog);
    splitter->addWidget(sidebar);
    auto *workspace = new QWidget;
    auto *right = new QVBoxLayout(workspace);
    auto *top = new QHBoxLayout;
    terminalTitle = new QLabel("No terminal selected");
    terminalTitle->setTextFormat(Qt::PlainText);
    terminalTitle->setWordWrap(true);
    top->addWidget(terminalTitle, 1);
    auto *close = new QPushButton("Close terminal");
    top->addWidget(close);
    right->addLayout(top);
    terminal = new TerminalArea;
    right->addWidget(terminal, 1);
    splitter->addWidget(workspace);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({390, 760});
    connect(close, &QPushButton::clicked, terminal, &TerminalArea::closeCurrent);
    connect(terminal, &TerminalArea::currentChanged, terminalTitle, &QLabel::setText);
    connect(terminal, &TerminalArea::clientStateChanged, this, &Window::clientStateChanged);
    connections = new QLabel;
    connections->setTextFormat(Qt::PlainText);
    statusBar()->addWidget(connections, 1);
    auto *projectsMenu = menuBar()->addMenu("&Session");
    projectsMenu->addAction("New session in selected project", this, [this] {
        const auto index = tree->currentIndex();
        auto id = index.data(BackendRole).toString();
        if (!index.isValid() || !profiles.contains(id) ||
            index.data(KindRole).toInt() == int(NodeKind::Server)) {
            error("Select a project first.");
            return;
        }
        if (!backends[id]->ready()) {
            error("Connect the selected project's backend first.");
            return;
        }
        QString issue;
        if (!terminal->newSession(profiles[id], index.data(PathRole).toString(), &issue))
            error(issue);
    });
    projectsMenu->addAction("Close terminal", terminal, &TerminalArea::closeCurrent);
    projectsMenu->addSeparator();
    projectsMenu->addAction("Quit", this, &QWidget::close);
    auto *connectionMenu = menuBar()->addMenu("&Connections");
    connectionMenu->addAction("Discover local sessions", this, &Window::discover);
    connectionMenu->addAction("Manage connections…", this, &Window::connectionsDialog);
    auto *view = menuBar()->addMenu("&View");
    auto *hideUnsaved = view->addAction("Hide unsaved sessions");
    hideUnsaved->setObjectName("hideUnsavedSessions");
    hideUnsaved->setCheckable(true);
    QSettings settings(directory + "/window.ini", QSettings::IniFormat);
    hideUnsaved->setChecked(settings.value("hideUnsavedSessions", true).toBool());
    model->setHideUnsaved(hideUnsaved->isChecked());
    results->setHideUnsaved(hideUnsaved->isChecked());
    hideUnsaved->setToolTip(
        "Hide entries without saved history. Open terminals remain accessible.");
    connect(hideUnsaved, &QAction::toggled, this, [this](bool hide) {
        QSettings settings(dataDirectory + "/window.ini", QSettings::IniFormat);
        settings.setValue("hideUnsavedSessions", hide);
        settings.sync();
        model->setHideUnsaved(hide);
        results->setHideUnsaved(hide);
        if (searchingView)
            updateSearchSummary();
    });
    view->addAction("Focus project navigator", QKeySequence("Ctrl+Shift+P"), tree,
                    qOverload<>(&QWidget::setFocus));
    view->addAction("Focus search", QKeySequence("Ctrl+Shift+F"), searchBox,
                    qOverload<>(&QWidget::setFocus));
    auto *help = menuBar()->addMenu("&Help");
    help->addAction("About Archcodex", this, [this] {
        QMessageBox::about(
            this, "Archcodex",
            QString("Archcodex %1\nNative Codex session workspace\n\nLocal Unix sockets · "
                    "event-driven status · Konsole terminal\nGPL-3.0-or-later")
                .arg(ARCHCODEX_VERSION));
    });
    for (const auto &p : store->backends()) {
        profiles[p.id] = p;
        model->backendName(p.id, p.name);
        results->backendName(p.id, p.name);
        model->backendEnvironment(p.id, p.environmentId);
        results->backendEnvironment(p.id, p.environmentId);
    }
    refreshNames();
    for (const auto &p : store->projects())
        model->project(p);
    for (const auto &s : store->sessions()) {
        sessions[sessionKey(s.backendId, s.id)] = s;
        model->upsert(s);
    }
    for (const auto &p : profiles)
        attach(p, connectOnStart && p.autoConnect && !p.discovered);
    discovery = new HostDiscovery(this);
    connect(discovery, &HostDiscovery::snapshot, this, &Window::applyDiscovery);
    connect(discovery, &HostDiscovery::failed, this, [this](const QString &message) {
        discoveryMessage = message;
        updateSummary();
    });
    if (connectOnStart)
        QTimer::singleShot(0, this, &Window::discover);
    restoreGeometry(settings.value("geometry").toByteArray());
    splitter->restoreState(settings.value("splitter").toByteArray());
    if (settings.contains("expanded")) {
        tree->collapseAll();
        restoreTree(tree, settings.value("expanded").toStringList(), {});
    }
    updateSummary();
}
Window::~Window() {
    discovery->disconnect(this);
    discovery->stop();
    for (auto *b : backends) {
        b->disconnect(this);
        b->disconnectServer();
    }
}
bool Window::persist(const std::function<void()> &op) {
    try {
        op();
        return true;
    } catch (const std::exception &e) {
        if (!storageFailed) {
            storageFailed = true;
            error("Could not save application metadata: " + QString::fromUtf8(e.what()));
        }
        return false;
    }
}
void Window::error(const QString &text) {
    QMessageBox box(QMessageBox::Warning, "Archcodex", {}, QMessageBox::Ok, this);
    box.setTextFormat(Qt::PlainText);
    box.setText(text);
    box.exec();
}
void Window::attach(const BackendProfile &p, bool start) {
    auto *b =
        new LocalBackend(p, QCoreApplication::applicationDirPath() + "/archcodex-bridge", this);
    backends[p.id] = b;
    connect(b, &Backend::loadedInventory, this, [this, id = p.id](const QSet<QString> &loaded) {
        // Cached rows outside the first history page also need reconciliation.
        for (auto &s : sessions)
            if (s.backendId == id && !s.id.startsWith("new-") && !s.known &&
                !loaded.contains(s.id)) {
                s.status = {{"type", "notLoaded"}};
                s.known = true;
                model->upsert(s);
                persist([&] { store->save(s); });
                const auto key = sessionKey(id, s.id);
                if (searchSessions.contains(key)) {
                    searchSessions[key] = s;
                    results->upsert(s);
                }
            }
    });
    connect(b, &Backend::connectionChanged, this,
            [this, id = p.id](const QString &text, bool ready) {
                connectionStates[id] = text;
                connectionReady[id] = ready;
                if (!ready) {
                    if (pendingOpen.startsWith(id + QChar(0x1f)))
                        pendingOpen.clear();
                    for (auto it = sessions.begin(); it != sessions.end(); ++it)
                        if (it->backendId == id) {
                            it->known = false;
                            model->upsert(*it);
                        }
                    for (auto it = searchSessions.begin(); it != searchSessions.end(); ++it)
                        if (it->backendId == id) {
                            it->known = false;
                            results->upsert(*it);
                        }
                    if (searchingView && searching.remove(id)) {
                        searchErrors
                            << profiles.value(id).name + ": Search interrupted by disconnection.";
                        updateSearchSummary();
                    }
                }
                updateSummary();
            });
    connect(b, &Backend::sessionChanged, this, [this](Session s) {
        const auto key = sessionKey(s.backendId, s.id);
        s.opened = sessions.value(key).opened;
        s.terminalOpen = sessions.value(key).terminalOpen;
        s.clientIssue = sessions.value(key).clientIssue;
        sessions[key] = s;
        model->upsert(s);
        persist([&] { store->save(s); });
        if (searchSessions.contains(key)) {
            searchSessions[key] = s;
            results->upsert(s);
            if (searchingView)
                updateSearchSummary();
        }
    });
    connect(b, &Backend::sessionRemoved, this, [this, id = p.id](const QString &thread) {
        const auto key = sessionKey(id, thread);
        if (sessions.value(key).terminalOpen) {
            auto &s = sessions[key];
            s.unavailable = true;
            s.known = false;
            model->upsert(s);
            if (searchSessions.contains(key)) {
                searchSessions[key] = s;
                results->upsert(s);
            }
        } else {
            sessions.remove(key);
            model->remove(id, thread);
            searchSessions.remove(key);
            results->remove(id, thread);
        }
        if (searchingView)
            updateSearchSummary();
        persist([&] { store->removeSession(id, thread); });
    });
    connect(b, &Backend::moreAvailable, this, [this, id = p.id](bool available) {
        more[id] = available;
        loadMore->setEnabled(!searchingView &&
                             std::any_of(more.begin(), more.end(), [](bool x) { return x; }));
    });
    connect(b, &Backend::searchResults, this,
            [this, id = p.id](quint64 token, const QList<Session> &matches, const QString &error,
                              bool complete) {
                if (token != searchToken || !searchingView)
                    return;
                for (auto s : matches) {
                    if (!scopeBackend.isEmpty() &&
                        (s.backendId != scopeBackend ||
                         (!scopePath.isEmpty() && s.cwd != scopePath &&
                          !s.cwd.startsWith(scopePath == "/" ? "/" : scopePath + "/"))))
                        continue;
                    const auto key = sessionKey(s.backendId, s.id);
                    if (sessions.contains(key))
                        s = sessions[key];
                    else
                        s.known = false;
                    searchSessions[key] = s;
                    results->upsert(s);
                }
                if (!error.isEmpty())
                    searchErrors << profiles.value(id).name + ": " + error;
                if (complete)
                    searching.remove(id);
                updateSearchSummary();
                tree->expandAll();
            });
    connect(b, &Backend::identityReceived, this, [this, id = p.id](const QJsonObject &identity) {
        if (!profiles.contains(id))
            return;
        auto &profile = profiles[id];
        for (auto it = identity.begin(); it != identity.end(); ++it)
            profile.identity[it.key()] = it.value();
        refreshNames();
        persist([&] { store->save(profile); });
    });
    connect(b, &Backend::sessionVerified, this,
            [this, id = p.id](const QString &thread, const QString &issue) {
                const auto key = sessionKey(id, thread);
                if (pendingOpen != key)
                    return;
                pendingOpen.clear();
                if (!issue.isEmpty()) {
                    error("This server could not open the session:\n" + issue);
                    return;
                }
                auto s = sessions.value(key);
                s.opened = QDateTime::currentSecsSinceEpoch();
                sessions[key] = s;
                QString launchError;
                if (!terminal->open(profiles[id], s, &launchError)) {
                    error(launchError);
                    return;
                }
                persist([&] { store->save(s); });
            });
    if (start)
        QTimer::singleShot(0, b, &Backend::connectServer);
    else
        connectionStates[p.id] = "Disconnected";
}
void Window::expandActivityViews() {
    if (searchingView || !expandsByDefault(tabs->currentIndex()))
        return;
    const QSignalBlocker block(tree);
    const auto &closed = collapsed[tabs->currentIndex()];
    walkTree(filter, {}, [&](const QModelIndex &index) {
        if (filter->rowCount(index))
            tree->setExpanded(index, !closed.contains(index.data(KeyRole).toString()));
    });
}
void Window::refreshNames() {
    // Distinguish otherwise identical automatic labels with stable profile order,
    // never with a process ID. Manual names remain user-controlled.
    QStringList ids = profiles.keys();
    ids.sort();
    QHash<QString, int> totals, used;
    for (const auto &id : ids)
        ++totals[serverLabel(profiles[id])];
    for (const auto &id : ids) {
        auto &p = profiles[id];
        auto name = serverLabel(p);
        const int ordinal = ++used[name];
        if (p.discovered && totals[name] > 1)
            name += QString(" (%1)").arg(ordinal);
        const bool changed = p.name != name;
        p.name = name;
        for (auto *source : {model, results}) {
            source->backendName(id, name);
            source->backendDetails(id, serverDetails(p));
        }
        if (changed)
            persist([&] { store->save(p); });
    }
    updateSummary();
}
void Window::clientStateChanged(const Session &client) {
    const auto key = sessionKey(client.backendId, client.id);
    auto s = sessions.value(key, client);
    s.terminalOpen = client.terminalOpen;
    s.clientIssue = client.clientIssue;
    if (!s.terminalOpen && s.id.startsWith("new-")) {
        sessions.remove(key);
        model->remove(s.backendId, s.id);
        searchSessions.remove(key);
        results->remove(s.backendId, s.id);
        return;
    }
    sessions[key] = s;
    model->upsert(s);
    if (searchSessions.contains(key)) {
        searchSessions[key] = s;
        results->upsert(s);
    }
}
void Window::updateSummary() {
    int count = 0;
    QStringList descriptions;
    for (auto it = profiles.begin(); it != profiles.end(); ++it) {
        count += connectionReady.value(it.key());
        descriptions << it->name + ": " + connectionStates.value(it.key(), "Disconnected");
    }
    connections->setText(QString("%1 / %2 connections ready").arg(count).arg(profiles.size()));
    connections->setToolTip(descriptions.join('\n'));
    if (!searchingView)
        notice->setText(viewDescriptions[tabs->currentIndex()] +
                        (discoveryMessage.isEmpty() ? QString() : "\n" + discoveryMessage));
    emit connectionSummaryChanged();
}
void Window::editConnection(const QString &id) {
    BackendProfile p = profiles.value(id);
    if (id.isEmpty()) {
        p.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        p.name = "Local Codex";
    }
    QDialog dialog(this);
    dialog.setWindowTitle(id.isEmpty() ? "Add connection" : "Edit connection");
    auto *form = new QFormLayout(&dialog);
    auto *name = new QLineEdit(p.name);
    auto *socket = new QLineEdit(p.socketPath);
    socket->setPlaceholderText("Empty: host Codex default daemon");
    auto *executable = new QLineEdit(p.executable);
    auto *automatic = new QCheckBox("Connect when Archcodex starts");
    automatic->setChecked(p.autoConnect);
    form->addRow("Name", name);
    form->addRow("Unix socket on host", socket);
    form->addRow("Host Codex executable", executable);
    form->addRow(automatic);
    auto *explanation =
        new QLabel("Connects to an existing app-server. It does not start or update Codex.");
    explanation->setWordWrap(true);
    form->addRow(explanation);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        p.name = name->text().trimmed();
        p.socketPath = socket->text();
        p.executable = executable->text();
        p.autoConnect = automatic->isChecked();
        const auto issue = p.validate();
        if (!issue.isEmpty()) {
            error(issue);
            return;
        }
        dialog.accept();
    });
    dialog.resize(600, 200);
    if (dialog.exec() != QDialog::Accepted)
        return;
    // Editing a detected profile makes its executable/connection settings user-managed.
    p.discovered = false;
    if (profiles.contains(id) && profiles[id].socketPath != p.socketPath)
        p.environmentId.clear();
    if (!persist([&] { store->save(p); }))
        return;
    if (auto *old = backends.take(id)) {
        old->disconnectServer();
        old->deleteLater();
        terminal->closeBackend(id);
    }
    profiles[p.id] = p;
    model->backendName(p.id, p.name);
    results->backendName(p.id, p.name);
    model->backendEnvironment(p.id, p.environmentId);
    results->backendEnvironment(p.id, p.environmentId);
    refreshNames();
    attach(p, true);
    updateSummary();
}
void Window::connectionsDialog() {
    QDialog dialog(this);
    dialog.setWindowTitle("Connections — local Unix sockets");
    auto *layout = new QVBoxLayout(&dialog);
    auto *list = new QListWidget;
    layout->addWidget(list);
    auto populate = [&] {
        const auto selectedId =
            list->currentItem() ? list->currentItem()->data(Qt::UserRole).toString() : QString();
        list->clear();
        for (auto it = profiles.begin(); it != profiles.end(); ++it) {
            auto *item = new QListWidgetItem(
                it->name + " — " + connectionStates.value(it.key(), "Disconnected"), list);
            item->setData(Qt::UserRole, it.key());
            item->setToolTip(serverDetails(*it));
            if (it.key() == selectedId)
                list->setCurrentItem(item);
        }
    };
    connect(this, &Window::connectionSummaryChanged, &dialog, populate);
    populate();
    auto *row = new QHBoxLayout;
    layout->addLayout(row);
    auto selected = [&] {
        return list->currentItem() ? list->currentItem()->data(Qt::UserRole).toString() : QString();
    };
    auto button = [&](const QString &label, auto action) {
        auto *b = new QPushButton(label);
        row->addWidget(b);
        connect(b, &QPushButton::clicked, &dialog, action);
    };
    button("Add…", [&] {
        editConnection();
        populate();
    });
    button("Edit…", [&] {
        auto id = selected();
        if (!id.isEmpty()) {
            editConnection(id);
            populate();
        }
    });
    button("Connect", [&] {
        paused.remove(selected());
        auto *b = backends.value(selected());
        if (b)
            b->connectServer();
        populate();
    });
    button("Disconnect", [&] {
        paused.insert(selected());
        auto *b = backends.value(selected());
        if (b)
            b->disconnectServer();
        populate();
    });
    button("Remove", [&] {
        auto id = selected();
        if (id.isEmpty())
            return;
        if (QMessageBox::question(&dialog, "Remove connection?",
                                  "Remove this connection and its local project/history cache? "
                                  "Codex conversations and running work are kept.") !=
            QMessageBox::Yes)
            return;
        if (!persist([&] { store->removeBackend(id); }))
            return;
        ignoredSockets.insert(discoveredPaths.value(id, profiles[id].socketPath));
        if (auto *b = backends.take(id)) {
            b->disconnectServer();
            b->deleteLater();
        }
        terminal->closeBackend(id);
        profiles.remove(id);
        connectionStates.remove(id);
        connectionReady.remove(id);
        more.remove(id);
        model->removeBackend(id);
        results->removeBackend(id);
        for (auto it = sessions.begin(); it != sessions.end();)
            if (it->backendId == id)
                it = sessions.erase(it);
            else
                ++it;
        clearSearch();
        updateSummary();
        populate();
    });
    auto *close = new QDialogButtonBox(QDialogButtonBox::Close);
    layout->addWidget(close);
    connect(close, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog.resize(800, 300);
    dialog.exec();
}
void Window::discover() {
    QStringList paths;
    for (const auto &p : profiles)
        if (!p.socketPath.isEmpty())
            paths << p.socketPath;
    discoveryMessage = "Discovering existing local Codex sessions…";
    updateSummary();
    discovery->refresh(paths);
}
void Window::applyDiscovery(const QJsonArray &endpoints, const QStringList &warnings) {
    QSet<QString> present;
    for (const auto &value : endpoints) {
        const auto item = value.toObject();
        const auto path = item["path"].toString();
        if (!QDir::isAbsolutePath(path) || !validText(path))
            continue;
        QString id;
        const auto aliases = item["aliases"].toArray();
        for (const auto &p : profiles)
            if (p.socketPath == path || aliases.contains(p.socketPath) ||
                (p.socketPath.isEmpty() && item["default"].toBool())) {
                id = p.id;
                break;
            }
        if (id.isEmpty()) {
            if (ignoredSockets.contains(path))
                continue;
            BackendProfile p;
            p.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            for (const auto &field : {"user", "host", "ownerHome", "cwd", "pid", "default"})
                if (item.contains(field))
                    p.identity[field] = item[field];
            p.discovered = true;
            p.name = serverLabel(p);
            p.socketPath = path;
            p.executable = item["executable"].toString("codex");
            p.environmentId = item["hostEnvironment"].toBool() ? "local-host" : p.id;
            if (!p.validate().isEmpty() || !persist([&] { store->save(p); }))
                continue;
            id = p.id;
            profiles[id] = p;
            model->backendName(id, p.name);
            results->backendName(id, p.name);
            attach(p, false);
        }
        auto &p = profiles[id];
        for (const auto &field : {"user", "host", "ownerHome", "cwd", "pid", "default"})
            if (item.contains(field))
                p.identity[field] = item[field];
        const auto executable = item["executable"].toString("codex");
        if (p.discovered && p.executable != executable) {
            auto updated = p;
            updated.executable = executable;
            if (updated.validate().isEmpty() && persist([&] { store->save(updated); })) {
                p = updated;
                auto *old = backends.take(id);
                old->disconnect(this);
                old->disconnectServer();
                old->deleteLater();
                discoveredPresent.remove(id);
                attach(p, false);
            }
        }
        const QString environment = item["hostEnvironment"].toBool() ? "local-host" : id;
        if (p.environmentId != environment) {
            p.environmentId = environment;
            persist([&] { store->save(p); });
        }
        model->backendEnvironment(id, environment);
        results->backendEnvironment(id, environment);
        for (const auto &s : sessions)
            if (s.backendId == id)
                model->upsert(s);
        for (const auto &s : searchSessions)
            if (s.backendId == id)
                results->upsert(s);
        persist([&] { store->save(p); });
        discoveredPaths[id] = path;
        present.insert(id);
        if (!backends[id]->ready() && !paused.contains(id) && p.autoConnect &&
            !discoveredPresent.contains(id))
            backends[id]->connectServer();
    }
    refreshNames();
    if (warnings.isEmpty()) {
        for (const auto &id : discoveredPresent - present)
            if (profiles.contains(id) && profiles[id].discovered)
                backends[id]->disconnectServer();
        discoveredPresent = present;
    } else
        discoveredPresent.unite(present);
    discoveryMessage =
        endpoints.isEmpty()
            ? "No local Codex servers found. Start Codex normally, or add a custom connection."
            : QString("%1 local server(s) discovered. Folders group existing sessions.")
                  .arg(endpoints.size());
    if (!warnings.isEmpty())
        discoveryMessage += "\n" + warnings.join('\n');
    updateSummary();
}
void Window::activate(const QModelIndex &index) {
    const auto id = index.data(ThreadRole).toString();
    if (id.isEmpty())
        return;
    const auto backend = index.data(BackendRole).toString();
    const auto key = sessionKey(backend, id);
    pendingOpen.clear();
    if (terminal->focusSession(key)) {
        auto &s = sessions[key];
        s.opened = QDateTime::currentSecsSinceEpoch();
        model->upsert(s);
        if (searchSessions.contains(key)) {
            searchSessions[key] = s;
            results->upsert(s);
        }
        if (!s.id.startsWith("new-"))
            persist([&] { store->save(s); });
        return;
    }
    if (!profiles.contains(backend))
        return;
    if (!backends.value(backend) || !backends[backend]->ready()) {
        error("Connect this backend before opening its terminal.");
        return;
    }
    pendingOpen = key;
    backends[backend]->verifySession(id);
}
void Window::search() {
    const auto query = searchBox->text().trimmed();
    if (query.isEmpty()) {
        clearSearch();
        return;
    }
    if (query.size() > 4096) {
        error("Search text is too long.");
        return;
    }
    scopeBackend.clear();
    scopePath.clear();
    if (scope->currentIndex() == 1) {
        const auto index = tree->currentIndex();
        if (!index.isValid()) {
            error("Select a server, folder or session first.");
            return;
        }
        scopeBackend = index.data(BackendRole).toString();
        if (index.data(KindRole).toInt() != int(NodeKind::Server))
            scopePath = index.data(PathRole).toString();
    }
    if (!searchingView) {
        expanded[tabs->currentIndex()] = expandedKeys(tree);
        searchSelection = tree->currentIndex().data(KeyRole).toString();
    }
    ++searchToken;
    searchingView = true;
    searchSessions.clear();
    results->resetResults();
    searchErrors.clear();
    searching.clear();
    filter->setSourceModel(results);
    filter->setView(SessionFilter::View::All);
    tabs->setEnabled(false);
    loadMore->setEnabled(false);
    for (auto it = backends.begin(); it != backends.end(); ++it)
        if (scopeBackend.isEmpty() || scopeBackend == it.key())
            searching.insert(it.key());
    const auto ids = searching.values();
    for (const auto &id : ids)
        backends[id]->search(query, searchToken);
    updateSearchSummary();
}
void Window::clearSearch() {
    const bool wasSearching = searchingView;
    const QSignalBlocker block(tree);
    ++searchToken;
    for (auto *b : backends)
        b->search({}, searchToken);
    searchingView = false;
    searching.clear();
    searchSessions.clear();
    results->resetResults();
    filter->setSourceModel(model);
    filter->setView(static_cast<SessionFilter::View>(tabs->currentIndex()));
    if (wasSearching) {
        restoreTree(tree, expanded.value(tabs->currentIndex()), searchSelection);
        expandActivityViews();
    }
    tabs->setEnabled(true);
    loadMore->setEnabled(std::any_of(more.begin(), more.end(), [](bool x) { return x; }));
    updateSummary();
}
void Window::updateSearchSummary() {
    int visible = 0;
    for (int i = 0; i < results->rowCount(); ++i)
        visible += results->item(i)->data(CountRole).toInt();
    const auto hidden = searchSessions.size() - visible;
    notice->setText(
        QString("%1 results%2%3%4")
            .arg(visible)
            .arg(hidden ? QString(" (%1 unsaved/unavailable hidden)").arg(hidden) : QString())
            .arg(searching.isEmpty()
                     ? QString()
                     : QString(" — searching %1 connection(s)…").arg(searching.size()))
            .arg(searchErrors.isEmpty() ? QString() : "\n" + searchErrors.join('\n')));
}
bool Window::smokeTest(QString *error) {
    return terminal->smokeTest(error);
}
void Window::closeEvent(QCloseEvent *event) {
    QSettings settings(dataDirectory + "/window.ini", QSettings::IniFormat);
    settings.setValue("geometry", saveGeometry());
    settings.setValue("splitter", splitter->saveState());
    if (!searchingView) {
        settings.setValue("expanded", expandedKeys(tree));
    }
    for (auto *b : backends)
        b->disconnectServer();
    QMainWindow::closeEvent(event);
}
} // namespace Archcodex
