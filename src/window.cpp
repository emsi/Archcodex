// SPDX-License-Identifier: GPL-3.0-or-later
#include "window.h"
#include "asyncbackend.h"
#include "discovery.h"
#include "discoverydialog.h"
#include "launch.h"
#include "sessionmodel.h"
#include "settingsdialog.h"
#include "terminal.h"
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QHelpEvent>
#include <QInputDialog>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QScopedValueRollback>
#include <QSettings>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStatusBar>
#include <QStyle>
#include <QSysInfo>
#include <QTabBar>
#include <QToolButton>
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
    return index == int(SessionView::Running) || index == int(SessionView::Waiting) ||
           index == int(SessionView::Opened);
}
const QStringList viewDescriptions{
    "All sessions, including saved history. Hollow dots mean unknown state.",
    "Live sessions, including idle prompts and errors. Hollow dots mean last-known active.",
    "Sessions currently executing a turn.",
    "Live sessions ready for a prompt, an answer or an approval.",
    "Open terminal views, including retained output from exited clients."};
} // namespace
Window::Window(const QString &directory, bool connectOnStart) : dataDirectory(directory) {
    localUser = qEnvironmentVariable("USER", "Local user");
    localHost = QSysInfo::machineHostName();
    if (!QDir(directory).exists()) {
        if (!QDir().mkpath(directory))
            throw std::runtime_error("Cannot create the application data directory.");
        QFile::setPermissions(directory, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    }
    store = std::make_unique<AsyncStore>(directory + "/metadata.sqlite");
    connect(store.get(), &AsyncStore::failed, this, [this](const QString &message) {
        storageFailed = true;
        statusBar()->showMessage("Metadata cache: " + message);
    });
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
    tabs->addTab("Opened");
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
    scope->addItems({"All servers", "Selected group, server or folder"});
    scope->setAccessibleName("Search scope");
    left->addWidget(scope);
    connect(searchBox, &QLineEdit::returnPressed, this, &Window::search);
    connect(searchBox, &QLineEdit::textChanged, this, &Window::queryChanged);
    connect(scope, &QComboBox::currentIndexChanged, this, [this](int index) {
        auto &state = explorer[tabs->currentIndex()];
        state.scopeIndex = index;
        state.scopeCaptured = false;
        captureScope();
        if (tabs->currentIndex() != int(SessionView::All))
            queryChanged(searchBox->text());
        else
            updateSearchControls();
    });
    searchNotice = new QLabel;
    searchNotice->setObjectName("searchFeedback");
    searchNotice->hide();
    searchNotice->setTextFormat(Qt::PlainText);
    searchNotice->setWordWrap(true);
    left->addWidget(searchNotice);
    tree = new SessionTree;
    tree->setModel(filter);
    tree->setHeaderHidden(true);
    tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tree->setUniformRowHeights(true);
    tree->setAnimated(false);
    tree->setAccessibleName("Projects and sessions");
    tree->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(tree, &QWidget::customContextMenuRequested, this, &Window::sessionContextMenu);
    left->addWidget(tree, 1);
    connect(tree, &QTreeView::activated, this, &Window::activate);
    connect(tabs, &QTabBar::currentChanged, this, &Window::switchExplorer);
    connect(tree, &QTreeView::collapsed, this, [this](const QModelIndex &index) {
        explorer[tabs->currentIndex()].navigation.collapsed.insert(index.data(KeyRole).toString());
    });
    connect(tree, &QTreeView::expanded, this, [this](const QModelIndex &index) {
        explorer[tabs->currentIndex()].navigation.collapsed.remove(index.data(KeyRole).toString());
    });
    for (auto *source : {model, results}) {
        connect(source, &SessionModel::topologyAboutToChange, this, [this, source] {
            if (!changingExplorer && filter->sourceModel() == source) {
                topologyExpanded = expandedKeys(tree);
                topologySelected = tree->currentIndex().data(KeyRole).toString();
            }
        });
        connect(source, &SessionModel::topologyChanged, this, [this, source] {
            if (changingExplorer || filter->sourceModel() != source)
                return;
            const QSignalBlocker block(tree);
            restoreTree(tree, topologyExpanded, topologySelected);
            walkTree(filter, {}, [&](const QModelIndex &root) {
                const auto kind = static_cast<NodeKind>(root.data(KindRole).toInt());
                if (kind != NodeKind::Group && kind != NodeKind::Server)
                    return;
                const auto key = root.data(KeyRole).toString();
                if (!seenRoots.contains(key)) {
                    tree->expand(root);
                    seenRoots.insert(key);
                }
            });
            expandActivityViews();
        });
    }
    loadMore = new QPushButton("Load more history");
    loadMore->setEnabled(false);
    left->addWidget(loadMore);
    connect(model, &SessionModel::topologyChanged, this, [this] {
        if (!changingExplorer)
            updateSearchControls();
    });
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
    connect(terminal, &TerminalArea::restartRequested, this, &Window::restartTerminal);
    connect(terminal, &TerminalArea::sessionFocused, this, &Window::terminalFocused);
    connections = new QLabel;
    connections->setTextFormat(Qt::PlainText);
    statusBar()->addWidget(connections, 1);
    discoveryStatus = new QToolButton;
    discoveryStatus->setObjectName("discoveryStatus");
    discoveryStatus->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    discoveryStatus->setAutoRaise(true);
    statusBar()->addPermanentWidget(discoveryStatus);
    containerStatus = new QToolButton;
    containerStatus->setObjectName("containerDiscoveryStatus");
    containerStatus->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    containerStatus->setAutoRaise(true);
    containerStatus->setText("myCodex hosts…");
    statusBar()->addPermanentWidget(containerStatus);
    connect(containerStatus, &QToolButton::clicked, this, &Window::containerHostsDialog);
    connect(discoveryStatus, &QToolButton::clicked, this, &Window::showDiscoveryDetails);
    auto *projectsMenu = menuBar()->addMenu("&Session");
    projectsMenu->addAction("New session in selected project", this, [this] {
        const auto index = tree->currentIndex();
        auto id = index.data(BackendRole).toString();
        if (!index.isValid() || !profiles.contains(id) ||
            (index.data(KindRole).toInt() != int(NodeKind::Folder) &&
             index.data(KindRole).toInt() != int(NodeKind::Session))) {
            error("Select a project first.");
            return;
        }
        newSession(id, index.data(PathRole).toString());
    });
    projectsMenu->addAction("Next recent terminal", QKeySequence("Ctrl+Tab"), terminal,
                            [this] { terminal->switchRecent(false); });
    projectsMenu->addAction("Previous recent terminal", QKeySequence("Ctrl+Shift+Tab"), terminal,
                            [this] { terminal->switchRecent(true); });
    projectsMenu->addAction("Close terminal", terminal, &TerminalArea::closeCurrent);
    projectsMenu->addSeparator();
    projectsMenu->addAction("Quit", this, &QWidget::close);
    auto *connectionMenu = menuBar()->addMenu("&Connections");
    connectionMenu->addAction("Discover local sessions", this, &Window::discover);
    connectionMenu->addAction("Manage connections…", this, &Window::connectionsDialog);
    connectionMenu->addAction("Discovery details…", this, &Window::showDiscoveryDetails);
    connectionMenu->addAction("myCodex hosts…", this, &Window::containerHostsDialog);
    auto *view = menuBar()->addMenu("&View");
    auto *hideUnsaved = view->addAction("Hide unsaved sessions");
    hideUnsaved->setObjectName("hideUnsavedSessions");
    hideUnsaved->setCheckable(true);
    QSettings settings(directory + "/window.ini", QSettings::IniFormat);
    configuredLocalCodex = settings.value("application/localCodexExecutable").toString();
    terminal->setLocalCodexExecutable(configuredLocalCodex);
    hideUnsaved->setChecked(settings.value("hideUnsavedSessions", true).toBool());
    model->setHideUnsaved(hideUnsaved->isChecked());
    results->setHideUnsaved(hideUnsaved->isChecked());
    const bool workdirOnly = !settings.value("backends/myCodex/showAllSessions", false).toBool();
    model->setMyCodexWorkdirOnly(workdirOnly);
    results->setMyCodexWorkdirOnly(workdirOnly);
    hideUnsaved->setToolTip(
        "Hide entries without saved history. Open terminals remain accessible.");
    connect(hideUnsaved, &QAction::toggled, this, [this](bool hide) {
        QSettings settings(dataDirectory + "/window.ini", QSettings::IniFormat);
        settings.setValue("hideUnsavedSessions", hide);
        settings.sync();
        model->setHideUnsaved(hide);
        results->setHideUnsaved(hide);
        if (globalSearchActive)
            updateSearchSummary();
    });
    view->addAction("Focus project navigator", QKeySequence("Ctrl+Shift+P"), tree,
                    qOverload<>(&QWidget::setFocus));
    view->addAction("Focus search", QKeySequence("Ctrl+Shift+F"), searchBox,
                    qOverload<>(&QWidget::setFocus));
    auto *settingsMenu = menuBar()->addMenu("&Settings");
    auto *configure = settingsMenu->addAction("Configure Archcodex…", QKeySequence::Preferences,
                                              this, &Window::showSettings);
    configure->setObjectName("configureArchcodex");
    auto *help = menuBar()->addMenu("&Help");
    help->addAction("About Archcodex", this, [this] {
        QMessageBox::about(
            this, "Archcodex",
            QString("Archcodex %1\nNative Codex session workspace\n\nLocal Unix sockets · "
                    "event-driven status · Konsole terminal\nGPL-3.0-or-later")
                .arg(ARCHCODEX_VERSION));
    });
    discovery = new HostDiscovery(this);
    connect(discovery, &HostDiscovery::snapshot, this, &Window::applyDiscovery);
    connect(discovery, &HostDiscovery::failed, this, [this](const QString &message) {
        discoveryReport.failure = message;
        discoveryReport.issues = {};
        discoveryReport.inProgress = false;
        discoveryReport.checked = QDateTime::currentDateTime();
        updateSummary();
    });
    auto hosts = QJsonDocument::fromJson(settings.value("containerHosts").toByteArray()).array();
    if (hosts.isEmpty())
        hosts.append(QJsonObject{{"id", "local-mycodex"}, {"ssh", ""}, {"enabled", true}});
    for (const auto &value : hosts) {
        const auto h = value.toObject();
        const auto id = h["id"].toString(), host = h["ssh"].toString();
        if (id.isEmpty() || !validText(id, 128) || (!host.isEmpty() && !validSshHost(host)))
            continue;
        if (!h["enabled"].toBool(true))
            disabledContainerHosts.insert(id);
        addContainerHost(id, host, false);
    }
    connect(
        store.get(), &AsyncStore::loaded, this, [this, connectOnStart](const StoreSnapshot &cache) {
            for (const auto &p : cache.profiles) {
                if (profiles.contains(p.id))
                    continue;
                profiles[p.id] = p;
                model->backendName(p.id, p.name);
                results->backendName(p.id, p.name);
                model->backendEnvironment(p.id, p.environmentId);
                results->backendEnvironment(p.id, p.environmentId);
                if (p.isContainer()) {
                    model->setContainerWorkdir(p.id, p.identity["containerWorkdir"].toString());
                    results->setContainerWorkdir(p.id, p.identity["containerWorkdir"].toString());
                }
                attach(p, connectOnStart && p.autoConnect && !p.discovered && !p.isContainer());
            }
            cachePending = cache.sessions;
            restoreCache(connectOnStart);
        });
    restoreGeometry(settings.value("geometry").toByteArray());
    splitter->restoreState(settings.value("splitter").toByteArray());
    if (settings.contains("expanded")) {
        tree->collapseAll();
        restoreTree(tree, settings.value("expanded").toStringList(), {});
    }
    updateSummary();
}
void Window::restoreCache(bool connectOnStart) {
    QList<Session> batch;
    for (int i = 0; i < 64 && !cachePending.isEmpty(); ++i) {
        auto s = cachePending.takeLast();
        const auto key = sessionKey(s.backendId, s.id);
        if (!profiles.contains(s.backendId))
            continue;
        s.contextRequired = profiles[s.backendId].isContainer();
        if (s.contextRequired && s.executionEnvironment != profiles[s.backendId].environmentId)
            s.executionObserved = false;
        if (sessions.contains(key))
            continue; // New events take precedence over cache.
        sessions[key] = s;
        batch << s;
    }
    model->upsertMany(batch);
    if (!cachePending.isEmpty()) {
        QTimer::singleShot(0, this, [this, connectOnStart] { restoreCache(connectOnStart); });
        return;
    }
    refreshNames();
    QSettings settings(dataDirectory + "/window.ini", QSettings::IniFormat);
    if (settings.contains("expanded")) {
        tree->collapseAll();
        restoreTree(tree, settings.value("expanded").toStringList(), {});
    }
    if (connectOnStart)
        discover();
}
Window::~Window() {
    discovery->disconnect(this);
    delete discovery;
    for (auto *b : backends) {
        b->disconnect(this);
        delete b;
    }
    for (auto *d : containerDiscovery) {
        d->disconnect(this);
        delete d;
    }
    terminal->disconnect(this);
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
    for (auto it = pendingForks.begin(); it != pendingForks.end();)
        if (it->startsWith(p.id + QChar(0x1f)))
            it = pendingForks.erase(it);
        else
            ++it;
    presentProfile(p);
    auto *b =
        new AsyncBackend(p, QCoreApplication::applicationDirPath() + "/archcodex-bridge", this);
    backends[p.id] = b;
    connect(b, &Backend::loadedInventory, this, [this, id = p.id](const QSet<QString> &loaded) {
        // Cached rows outside the first history page also need reconciliation.
        QList<Session> updates, matches;
        for (auto &s : sessions)
            if (s.backendId == id && !s.id.startsWith("new-") && !s.known &&
                !loaded.contains(s.id)) {
                s.status = {{"type", "notLoaded"}};
                s.known = true;
                updates << s;
                persist([&] { store->save(s); });
                const auto key = sessionKey(id, s.id);
                if (searchSessions.contains(key)) {
                    searchSessions[key] = s;
                    matches << s;
                }
            }
        model->upsertMany(updates);
        results->upsertMany(matches);
    });
    connect(b, &Backend::connectionChanged, this,
            [this, id = p.id](const QString &text, bool ready) {
                connectionStates[id] = text;
                connectionReady[id] = ready;
                if (!ready) {
                    QList<Session> updates, matches;
                    if (pendingOpen.startsWith(id + QChar(0x1f)))
                        pendingOpen.clear();
                    for (auto it = sessions.begin(); it != sessions.end(); ++it)
                        if (it->backendId == id) {
                            it->known = false;
                            updates << *it;
                        }
                    for (auto it = searchSessions.begin(); it != searchSessions.end(); ++it)
                        if (it->backendId == id) {
                            it->known = false;
                            matches << *it;
                        }
                    model->upsertMany(updates);
                    results->upsertMany(matches);
                    if (globalSearchActive && searching.remove(id)) {
                        searchErrors
                            << profiles.value(id).name + ": Search interrupted by disconnection.";
                        updateSearchSummary();
                    }
                }
                updateSummary();
            });
    connect(b, &Backend::sessionChanged, this, [this](const Session &s) { applySessions({s}); });
    connect(b, &Backend::sessionsChanged, this, &Window::applySessions);
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
        if (globalSearchActive)
            updateSearchSummary();
        persist([&] { store->removeSession(id, thread); });
    });
    connect(b, &Backend::moreAvailable, this, [this, id = p.id](bool available) {
        more[id] = available;
        updateSearchControls();
    });
    connect(b, &Backend::searchResults, this,
            [this, id = p.id](quint64 token, const QList<Session> &matches, const QString &error,
                              bool complete) {
                if (token != searchToken || !globalSearchActive)
                    return;
                QList<Session> updates;
                for (auto s : matches) {
                    if (scopedSearch &&
                        (!scopeBackends.contains(s.backendId) ||
                         (!scopePath.isEmpty() && s.cwd != scopePath &&
                          !s.cwd.startsWith(scopePath == "/" ? "/" : scopePath + "/"))))
                        continue;
                    const auto key = sessionKey(s.backendId, s.id);
                    if (sessions.contains(key))
                        s = sessions[key];
                    else
                        s.known = false;
                    searchSessions[key] = s;
                    updates << s;
                }
                results->upsertMany(updates);
                if (!error.isEmpty())
                    searchErrors << profiles.value(id).name + ": " + error;
                if (complete)
                    searching.remove(id);
                updateSearchSummary();
                if (showingSearch())
                    expandActivityViews();
            });
    connect(b, &Backend::identityReceived, this, [this, id = p.id](const QJsonObject &identity) {
        if (!profiles.contains(id))
            return;
        auto &profile = profiles[id];
        for (auto it = identity.begin(); it != identity.end(); ++it)
            profile.identity[it.key()] = it.value();
        if (profile.isContainer() && identity["codexHome"].isString())
            profile.identity["serverCodexHome"] = identity["codexHome"];
        refreshNames();
        persist([&] { store->save(profile); });
    });
    connect(
        b, &Backend::sessionVerified, this,
        [this, id = p.id](const QString &thread, const QString &issue) {
            const auto key = sessionKey(id, thread);
            if (pendingOpen != key)
                return;
            const bool restart = pendingRestart;
            pendingOpen.clear();
            pendingRestart = false;
            if (!issue.isEmpty()) {
                error("This server could not open the session:\n" + issue);
                return;
            }
            auto s = sessions.value(key);
            if (profiles[id].isContainer() &&
                (!s.executionObserved || s.executionEnvironment != profiles[id].environmentId)) {
                error("This is shared history. Archcodex has not observed this conversation "
                      "loaded in this container. Open it in its original environment first; "
                      "it will then become available here through server events.");
                return;
            }
            QString launchError;
            const bool opened = restart ? terminal->restart(profiles[id], s, &launchError)
                                        : terminal->open(profiles[id], s, &launchError);
            if (!opened) {
                error(launchError);
                return;
            }
        });
    connect(b, &Backend::sessionForked, this,
            [this, id = p.id](const QString &source, const Session &session, const QString &issue) {
                if (!pendingForks.remove(sessionKey(id, source)))
                    return;
                if (!issue.isEmpty()) {
                    error("Could not fork the session:\n" + issue);
                    return;
                }
                applySessions({session});
                QString launchError;
                if (!terminal->open(profiles[id], sessions.value(sessionKey(id, session.id)),
                                    &launchError))
                    error("The fork was created, but its terminal could not open:\n" + launchError);
            });
    if (start)
        QTimer::singleShot(0, b, &Backend::connectServer);
    else
        connectionStates[p.id] = "Disconnected";
}
void Window::newSession(const QString &backend, const QString &path) {
    if (!profiles.contains(backend) || !backends.value(backend) || !backends[backend]->ready()) {
        error("Connect the selected project's backend first.");
        return;
    }
    QString issue;
    if (!terminal->newSession(profiles[backend], path, &issue))
        error(issue);
}
void Window::forkSession(const QString &backend, const QString &thread) {
    const auto key = sessionKey(backend, thread);
    if (pendingForks.contains(key))
        return;
    if (!profiles.contains(backend) || !backends.value(backend) || !backends[backend]->ready()) {
        error("Connect the session's backend before forking.");
        return;
    }
    if (thread.isEmpty() || thread.startsWith("new-")) {
        error("This terminal does not yet have a confirmed session ID to fork.");
        return;
    }
    pendingForks.insert(key);
    const auto s = sessions.value(key, searchSessions.value(key));
    backends[backend]->forkSession(thread,
                                   s.executionObserved ? s.executionEnvironment : QString());
}
void Window::sessionContextMenu(const QPoint &position) {
    const auto index = tree->indexAt(position);
    if (!index.isValid())
        return;
    const auto kind = static_cast<NodeKind>(index.data(KindRole).toInt());
    const auto key = index.data(KeyRole).toString();
    const auto backend = index.data(BackendRole).toString();
    const auto thread = index.data(ThreadRole).toString();
    const auto path = index.data(PathRole).toString();
    const bool connected = backends.value(backend) && backends[backend]->ready();
    auto *menu = new QMenu(tree);
    menu->setObjectName("sessionContextMenu");
    menu->setAttribute(Qt::WA_DeleteOnClose);
    // Capture identities, not transient indexes: events can reorder or remove rows
    // while a menu is open. Expand resolves its key against the current view.
    if (kind == NodeKind::Session) {
        const auto s = sessions.value(key, searchSessions.value(key));
        auto *fork = menu->addAction("Fork", this,
                                     [this, backend, thread] { forkSession(backend, thread); });
        fork->setEnabled(connected && !thread.startsWith("new-") && !thread.isEmpty() &&
                         !s.unavailable && !pendingForks.contains(key) &&
                         (!s.contextRequired || s.executionObserved));
    }
    if (kind == NodeKind::Session || kind == NodeKind::Folder) {
        auto *create =
            menu->addAction("New", this, [this, backend, path] { newSession(backend, path); });
        create->setEnabled(connected && QDir::isAbsolutePath(path) && validText(path));
    }
    if (kind == NodeKind::Session) {
        auto *close =
            menu->addAction("Close", terminal, [this, key] { terminal->closeSession(key); });
        close->setEnabled(sessions.value(key).terminalOpen);
        close->setToolTip("Close this terminal view; keep the saved conversation.");
    } else {
        menu->addAction("Expand All", this, [this, key] {
            walkTree(filter, {}, [&](const QModelIndex &item) {
                if (item.data(KeyRole).toString() != key)
                    return;
                tree->expand(item);
                walkTree(filter, item, [this](const QModelIndex &child) { tree->expand(child); });
            });
        });
    }
    menu->popup(tree->viewport()->mapToGlobal(position));
}
void Window::expandActivityViews() {
    const auto &state = explorer[tabs->currentIndex()];
    if (!state.filtering && !expandsByDefault(tabs->currentIndex()))
        return;
    const QSignalBlocker block(tree);
    const auto &closed = state.navigation.collapsed;
    walkTree(filter, {}, [&](const QModelIndex &index) {
        if (filter->rowCount(index))
            tree->setExpanded(index, !closed.contains(index.data(KeyRole).toString()));
    });
}
void Window::applySessions(const QList<Session> &items) {
    QList<Session> updates, matches;
    for (auto s : items) {
        if (!profiles.contains(s.backendId))
            continue;
        const auto key = sessionKey(s.backendId, s.id);
        const auto old = sessions.value(key);
        s.opened = old.opened;
        s.terminalOpen = old.terminalOpen;
        s.clientIssue = old.clientIssue;
        s.contextRequired = profiles[s.backendId].isContainer();
        if (s.contextRequired) {
            const auto environment = profiles[s.backendId].environmentId;
            s.executionObserved =
                (s.executionObserved && s.executionEnvironment == environment) ||
                (old.executionObserved && old.executionEnvironment == environment);
            s.executionEnvironment = environment;
        }
        sessions[key] = s;
        updates << s;
        store->save(s);
        if (searchSessions.contains(key)) {
            searchSessions[key] = s;
            matches << s;
        }
    }
    model->upsertMany(updates);
    results->upsertMany(matches);
    if (globalSearchActive && !matches.isEmpty())
        updateSearchSummary();
}
void Window::addContainerHost(const QString &id, const QString &host, bool start) {
    if (containerDiscovery.contains(id))
        return;
    containerHosts[id] = host;
    auto *d = new ContainerDiscovery(id, host, this);
    containerDiscovery[id] = d;
    containerFeedback[id] = disabledContainerHosts.contains(id) ? "Disabled" : "Not connected";
    connect(d, &ContainerDiscovery::snapshot, this,
            [this, id](const QJsonObject &data) { applyContainers(id, data); });
    connect(d, &ContainerDiscovery::failed, this, [this, id](const QString &error) {
        if (disabledContainerHosts.contains(id))
            return;
        containerFeedback[id] = error;
        containerProblems.insert(id);
        updateContainerSummary();
    });
    if (start) {
        containerFeedback[id] = "Discovering…";
        d->refresh();
    }
    updateContainerSummary();
}
void Window::saveContainerHosts() {
    QJsonArray data;
    for (auto it = containerHosts.begin(); it != containerHosts.end(); ++it)
        data.append(QJsonObject{{"id", it.key()},
                                {"ssh", it.value()},
                                {"enabled", !disabledContainerHosts.contains(it.key())}});
    QSettings settings(dataDirectory + "/window.ini", QSettings::IniFormat);
    settings.setValue("containerHosts", QJsonDocument(data).toJson(QJsonDocument::Compact));
}
void Window::updateContainerSummary() {
    QStringList details;
    for (auto it = containerHosts.begin(); it != containerHosts.end(); ++it)
        details << (it.value().isEmpty() ? "This computer" : it.value()) + ": " +
                       containerFeedback.value(it.key());
    containerStatus->setToolTip(details.join('\n'));
    containerStatus->setText(containerProblems.isEmpty() ? "myCodex hosts…"
                                                         : "myCodex connection issues…");
    containerStatus->setIcon(
        QIcon::fromTheme(containerProblems.isEmpty() ? "network-server" : "dialog-warning"));
    emit connectionSummaryChanged();
}
void Window::containerHostsDialog() {
    QDialog dialog(this);
    dialog.setWindowTitle("myCodex hosts");
    auto *layout = new QVBoxLayout(&dialog);
    auto *description = new QLabel(
        "Local containers are discovered automatically. Remote discovery uses "
        "host OpenSSH and requires a known host key and non-interactive authentication "
        "(for example, "
        "your SSH agent). Docker and Python 3 must be available on each host. "
        "Disabling or removing a host disconnects its terminals; their output is retained.");
    description->setWordWrap(true);
    layout->addWidget(description);
    auto *list = new QListWidget;
    list->setObjectName("containerHosts");
    layout->addWidget(list);
    auto populate = [&] {
        const auto current =
            list->currentItem() ? list->currentItem()->data(Qt::UserRole).toString() : QString();
        list->clear();
        for (auto it = containerHosts.begin(); it != containerHosts.end(); ++it) {
            const auto label = (it.value().isEmpty() ? "This computer" : it.value()) + " — " +
                               containerFeedback.value(it.key());
            auto *item = new QListWidgetItem(label, list);
            item->setData(Qt::UserRole, it.key());
            item->setToolTip(label);
            if (it.key() == current)
                list->setCurrentItem(item);
        }
    };
    populate();
    connect(this, &Window::connectionSummaryChanged, &dialog, populate);
    auto *row = new QHBoxLayout;
    layout->addLayout(row);
    auto button = [&](const QString &text, auto action) {
        auto *b = new QPushButton(text);
        row->addWidget(b);
        connect(b, &QPushButton::clicked, &dialog, action);
    };
    button("Add SSH host…", [&] {
        bool ok = false;
        const auto host =
            QInputDialog::getText(&dialog, "Add myCodex host", "SSH alias or user@host",
                                  QLineEdit::Normal, {}, &ok)
                .trimmed();
        if (!ok)
            return;
        if (!validSshHost(host)) {
            error("Enter an SSH alias or user@host without command options.");
            return;
        }
        if (containerHosts.values().contains(host)) {
            error("This host is already configured.");
            return;
        }
        addContainerHost(QUuid::createUuid().toString(QUuid::WithoutBraces), host, true);
        saveContainerHosts();
    });
    button("Connect / retry", [&] {
        if (!list->currentItem())
            return;
        const auto id = list->currentItem()->data(Qt::UserRole).toString();
        disabledContainerHosts.remove(id);
        containerFeedback[id] = "Discovering…";
        containerDiscovery[id]->refresh();
        saveContainerHosts();
        updateContainerSummary();
    });
    button("Disable", [&] {
        if (!list->currentItem())
            return;
        const auto id = list->currentItem()->data(Qt::UserRole).toString();
        disabledContainerHosts.insert(id);
        containerDiscovery[id]->stop();
        containerFeedback[id] = "Disabled";
        containerProblems.remove(id);
        for (const auto &p : profiles)
            if (p.discoveryHost == id)
                backends[p.id]->disconnectServer();
        saveContainerHosts();
        updateContainerSummary();
    });
    button("Remove", [&] {
        if (!list->currentItem())
            return;
        const auto id = list->currentItem()->data(Qt::UserRole).toString();
        if (containerHosts[id].isEmpty()) {
            error("The local host can be disabled, but not removed.");
            return;
        }
        delete containerDiscovery.take(id);
        containerHosts.remove(id);
        containerFeedback.remove(id);
        containerProblems.remove(id);
        disabledContainerHosts.remove(id);
        for (const auto &p : profiles)
            if (p.discoveryHost == id)
                backends[p.id]->disconnectServer();
        saveContainerHosts();
        updateContainerSummary();
    });
    auto *close = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(close, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(close);
    dialog.resize(850, 390);
    dialog.exec();
}
void Window::applyContainers(const QString &hostId, const QJsonObject &inventory) {
    if (!containerHosts.contains(hostId) || disabledContainerHosts.contains(hostId))
        return;
    const auto host = inventory["host"].toString();
    if (!host.isEmpty() && validText(host, 255)) {
        containerHostNames[hostId] = host;
        if (containerHosts[hostId].isEmpty()) {
            localHost = host;
            localIdentityDiscovered = true;
            const auto user = inventory["user"].toString();
            if (!user.isEmpty() && validText(user, 128))
                localUser = user;
        }
    }
    QSet<QString> present, empty;
    QStringList issues;
    for (const auto &issue : inventory["issues"].toArray())
        issues << issue.toObject()["summary"].toString();
    const auto containers = inventory["containers"].toArray();
    if (containers.size() > 512) {
        containerFeedback[hostId] = "Container inventory exceeds the 512-container limit.";
        containerProblems.insert(hostId);
        updateContainerSummary();
        return;
    }
    for (const auto &value : containers) {
        const auto c = value.toObject();
        const auto cid = c["containerId"].toString(), engine = c["engine"].toString();
        if (!QRegularExpression("^[a-f0-9]{64}$").match(cid).hasMatch() || !validText(engine, 256))
            continue;
        const auto key = hostId + QChar(0x1f) + engine + QChar(0x1f) + cid;
        const auto digest = [](const QString &s) {
            return QString::fromLatin1(
                QCryptographicHash::hash(s.toUtf8(), QCryptographicHash::Sha256).toHex());
        };
        const auto rootId = "container:" + hostId + ":" + digest(key);
        auto endpoints = c["endpoints"].toArray();
        for (const auto &i : c["issues"].toArray())
            issues << c["containerName"].toString() + ": " + i.toObject()["summary"].toString();
        bool hasServer = false;
        if (endpoints.size() > 4096) {
            issues << "Container endpoint inventory exceeds its limit.";
            endpoints = {};
        }
        for (const auto &entry : endpoints) {
            const auto ep = entry.toObject();
            BackendProfile p;
            p.id = "mc-" + digest(key + QChar(0x1f) + ep["path"].toString());
            if (ignoredSockets.contains(p.id))
                continue;
            p.kind = "mycodex";
            p.containerId = cid;
            p.containerEpoch = c["containerEpoch"].toString();
            p.dockerContext = c["dockerContext"].toString();
            p.sshHost = containerHosts[hostId];
            p.discoveryHost = hostId;
            p.runtimeUser = c["runtimeUser"].toString();
            p.runtimeHome = c["runtimeHome"].toString();
            p.socketPath = ep["path"].toString();
            p.relaySocket = ep["localSocket"].toString();
            p.executable = ep["executable"].toString("codex");
            p.environmentId = digest(key + c["environment"].toString());
            p.discovered = true;
            p.identity = {{"containerName", c["containerName"]},
                          {"containerEnvironment", c["environment"]},
                          {"dockerEngine", engine},
                          {"stateMode", c["stateMode"]},
                          {"stateVolume", c["stateVolume"]},
                          {"codexHome", c["codexHome"]},
                          {"containerWorkdir", c["cwd"]},
                          {"dockerHost", host},
                          {"pid", ep["pid"]},
                          {"cwd", ep["cwd"]}};
            p.name = serverLabel(p);
            if (!p.validate().isEmpty()) {
                issues << p.name + ": " + p.validate();
                continue;
            }
            present.insert(p.id);
            hasServer = true;
            const auto old = profiles.value(p.id);
            const bool changed =
                !backends.contains(p.id) || old.containerEpoch != p.containerEpoch ||
                old.environmentId != p.environmentId || old.executable != p.executable ||
                old.dockerContext != p.dockerContext || old.runtimeUser != p.runtimeUser ||
                old.relaySocket != p.relaySocket;
            if (!changed && old.identity.contains("serverCodexHome")) {
                p.identity["serverCodexHome"] = old.identity["serverCodexHome"];
                p.identity["codexHome"] = old.identity["serverCodexHome"];
            }
            if (profiles.contains(p.id))
                p.autoConnect = old.autoConnect;
            if (changed && backends.contains(p.id)) {
                auto *b = backends.take(p.id);
                b->disconnect(this);
                delete b;
                if (old.environmentId != p.environmentId)
                    for (auto &s : sessions)
                        if (s.backendId == p.id) {
                            s.executionObserved = false;
                            s.known = false;
                            model->upsert(s);
                            store->save(s);
                        }
            }
            profiles[p.id] = p;
            store->save(p);
            model->backendEnvironment(p.id, p.environmentId);
            results->backendEnvironment(p.id, p.environmentId);
            model->setContainerWorkdir(p.id, c["cwd"].toString());
            results->setContainerWorkdir(p.id, c["cwd"].toString());
            if (changed)
                attach(p, p.autoConnect && !paused.contains(p.id));
            else if (!backends[p.id]->ready() && p.autoConnect && !paused.contains(p.id) &&
                     !discoveredPresent.contains(p.id))
                backends[p.id]->connectServer();
            discoveredPresent.insert(p.id);
        }
        if (!hasServer) {
            empty.insert(rootId);
            BackendProfile container;
            container.kind = "mycodex";
            container.sshHost = containerHosts[hostId];
            container.discoveryHost = hostId;
            model->backendGroup(rootId, groupKey(container), groupLabel(container),
                                container.sshHost.isEmpty() ? 1 : 2);
            model->backendName(rootId, c["containerName"].toString());
            model->backendDetails(
                rootId, c["paused"].toBool() ? "Container paused"
                                             : "Running container — no accessible Codex server.\n" +
                                                   issues.join('\n'));
        }
    }
    for (const auto &id : emptyContainers)
        if (id.startsWith("container:" + hostId + ":") && !empty.contains(id))
            model->removeBackend(id);
    for (auto it = emptyContainers.begin(); it != emptyContainers.end();)
        if (it->startsWith("container:" + hostId + ":"))
            it = emptyContainers.erase(it);
        else
            ++it;
    emptyContainers.unite(empty);
    if (inventory["complete"].toBool())
        for (const auto &p : profiles)
            if (p.discoveryHost == hostId && !present.contains(p.id)) {
                backends[p.id]->disconnectServer();
                discoveredPresent.remove(p.id);
            }
    refreshNames();
    containerFeedback[hostId] =
        QString("%1 containers, %2 servers").arg(containers.size()).arg(present.size()) +
        (issues.isEmpty() ? QString() : "\n" + issues.join('\n'));
    if (issues.isEmpty())
        containerProblems.remove(hostId);
    else
        containerProblems.insert(hostId);
    updateContainerSummary();
}
void Window::refreshNames() {
    for (const auto &p : profiles) {
        if (!p.isContainer() && !localIdentityDiscovered) {
            if (!p.identity["user"].toString().isEmpty())
                localUser = p.identity["user"].toString();
            if (!p.identity["host"].toString().isEmpty())
                localHost = p.identity["host"].toString();
        } else if (p.isContainer() && !p.identity["dockerHost"].toString().isEmpty()) {
            if (!containerHostNames.contains(p.discoveryHost))
                containerHostNames[p.discoveryHost] = p.identity["dockerHost"].toString();
            if (p.sshHost.isEmpty() && !localIdentityDiscovered)
                localHost = p.identity["dockerHost"].toString();
        }
    }
    // Distinguish otherwise identical automatic labels with stable profile order,
    // never with a process ID. Manual names remain user-controlled.
    QStringList ids = profiles.keys();
    ids.sort();
    QHash<QString, int> totals, used;
    QHash<QString, int> treeTotals, treeUsed;
    for (const auto &id : ids) {
        ++totals[serverLabel(profiles[id])];
        ++treeTotals[groupKey(profiles[id]) + QChar(0x1f) + serverTreeLabel(profiles[id])];
    }
    for (const auto &id : ids) {
        auto &p = profiles[id];
        auto name = serverLabel(p);
        const int ordinal = ++used[name];
        if (p.discovered && totals[name] > 1)
            name += QString(" (%1)").arg(ordinal);
        const bool changed = p.name != name;
        p.name = name;
        auto treeName = serverTreeLabel(p);
        const auto treeKey = groupKey(p) + QChar(0x1f) + treeName;
        const int treeOrdinal = ++treeUsed[treeKey];
        if (p.discovered && treeTotals[treeKey] > 1)
            treeName += QString(" (%1)").arg(treeOrdinal);
        presentProfile(p, treeName);
        if (changed)
            persist([&] { store->save(p); });
    }
    updateSummary();
}
QString Window::groupKey(const BackendProfile &p) const {
    return !p.isContainer() ? QString("local-host")
           : p.sshHost.isEmpty()
               ? QString("mycodex-local")
               : "mycodex-ssh:" + (p.discoveryHost.isEmpty() ? p.sshHost : p.discoveryHost);
}
QString Window::groupLabel(const BackendProfile &p) const {
    if (!p.isContainer())
        return localUser + " @ " + localHost;
    return "myCodex @ " +
           (p.sshHost.isEmpty() ? localHost : containerHostNames.value(p.discoveryHost, p.sshHost));
}
void Window::presentProfile(const BackendProfile &p, const QString &label) {
    for (auto *source : {model, results}) {
        source->backendGroup(p.id, groupKey(p), groupLabel(p),
                             !p.isContainer()      ? 0
                             : p.sshHost.isEmpty() ? 1
                                                   : 2);
        source->backendName(p.id, label.isEmpty() ? serverTreeLabel(p) : label);
        source->backendDetails(p.id, serverDetails(p));
    }
}
void Window::terminalFocused(const QString &key) {
    pendingOpen.clear();
    pendingRestart = false;
    auto it = sessions.find(key);
    if (it == sessions.end())
        return;
    it->opened = QDateTime::currentSecsSinceEpoch();
    model->upsert(*it);
    if (searchSessions.contains(key)) {
        searchSessions[key] = *it;
        results->upsert(*it);
    }
    walkTree(filter, {}, [&](const QModelIndex &index) {
        if (index.data(KeyRole) == key) {
            tree->setCurrentIndex(index);
            for (auto parent = index.parent(); parent.isValid(); parent = parent.parent())
                tree->expand(parent);
            tree->scrollTo(index);
        }
    });
    if (!it->id.startsWith("new-"))
        persist([&] { store->save(*it); });
}
void Window::clientStateChanged(const Session &client) {
    const auto key = sessionKey(client.backendId, client.id);
    if (!client.terminalOpen && pendingOpen == key) {
        pendingOpen.clear();
        pendingRestart = false;
    }
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
void Window::restartTerminal(const Session &client) {
    pendingOpen.clear();
    pendingRestart = false;
    const auto id = client.backendId;
    if (!profiles.contains(id) || !backends.value(id) || !backends[id]->ready()) {
        error("Connect this backend before restarting its terminal.");
        return;
    }
    if (client.id.startsWith("new-")) {
        QString issue;
        if (!terminal->restart(profiles[id], client, &issue))
            error(issue);
        return;
    }
    pendingOpen = sessionKey(id, client.id);
    pendingRestart = true;
    backends[id]->verifySession(client.id);
}
void Window::updateSummary() {
    int count = 0;
    QStringList descriptions;
    for (auto it = profiles.begin(); it != profiles.end(); ++it) {
        count += connectionReady.value(it.key());
        descriptions << it->name + ": " + connectionStates.value(it.key(), "Disconnected");
    }
    connections->setText(
        profiles.isEmpty() ? "No connections"
        : count == profiles.size()
            ? "Connections ready"
            : QString("%1 of %2 connections ready").arg(count).arg(profiles.size()));
    connections->setToolTip(descriptions.join('\n'));
    updateSearchControls();
    const bool failed = !discoveryReport.failure.isEmpty();
    const bool limited = !discoveryReport.issues.isEmpty();
    const bool empty = discoveryReport.checked.isValid() && !discoveryReport.endpoints;
    discoveryStatus->setVisible(discoveryReport.inProgress || failed || limited || empty);
    discoveryStatus->setText(discoveryReport.inProgress ? "Discovering…"
                             : failed                   ? "Discovery failed"
                             : limited                  ? "Discovery limited"
                                                        : "No local servers");
    discoveryStatus->setIcon(style()->standardIcon(discoveryReport.inProgress
                                                       ? QStyle::SP_BrowserReload
                                                   : failed  ? QStyle::SP_MessageBoxCritical
                                                   : limited ? QStyle::SP_MessageBoxWarning
                                                             : QStyle::SP_MessageBoxInformation));
    discoveryStatus->setToolTip("View discovery details and available actions");
    discoveryStatus->setAccessibleName(discoveryStatus->text() + " — view details");
    if (discoveryDialog)
        discoveryDialog->setReport(discoveryReport);
    emit connectionSummaryChanged();
}
void Window::showSettings() {
    auto *hideUnsaved = findChild<QAction *>("hideUnsavedSessions");
    QSettings settings(dataDirectory + "/window.ini", QSettings::IniFormat);
    SettingsDialog dialog(hideUnsaved->isChecked(),
                          settings.value("backends/myCodex/showAllSessions", false).toBool(),
                          configuredLocalCodex, this);
    connect(&dialog, &SettingsDialog::settingsApplied, this,
            [this, hideUnsaved](bool hide, bool showAll, const QString &localExecutable) {
                hideUnsaved->setChecked(hide);
                QSettings settings(dataDirectory + "/window.ini", QSettings::IniFormat);
                settings.setValue("backends/myCodex/showAllSessions", showAll);
                settings.setValue("application/localCodexExecutable", localExecutable);
                settings.sync();
                configuredLocalCodex = localExecutable;
                terminal->setLocalCodexExecutable(
                    configuredLocalCodex.isEmpty() ? discoveredLocalCodex : configuredLocalCodex);
                model->setMyCodexWorkdirOnly(!showAll);
                results->setMyCodexWorkdirOnly(!showAll);
                if (globalSearchActive)
                    updateSearchSummary();
            });
    dialog.exec();
}
void Window::showDiscoveryDetails() {
    if (!discoveryDialog) {
        discoveryDialog = new DiscoveryDialog(this);
        connect(discoveryDialog, &DiscoveryDialog::retryRequested, this, &Window::discover);
        connect(discoveryDialog, &DiscoveryDialog::addConnectionRequested, this,
                [this] { editConnection(); });
    }
    discoveryDialog->setReport(discoveryReport);
    discoveryDialog->show();
    discoveryDialog->raise();
    discoveryDialog->activateWindow();
}
void Window::editConnection(const QString &id) {
    BackendProfile p = profiles.value(id);
    if (p.isContainer()) {
        error("Container endpoints are discovered automatically. Manage their host in Connections "
              "→ myCodex hosts.");
        return;
    }
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
    dialog.setWindowTitle("Connections — Codex servers");
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
        ignoredSockets.insert(
            profiles[id].isContainer() ? id : discoveredPaths.value(id, profiles[id].socketPath));
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
        if (!p.isContainer() && !p.socketPath.isEmpty())
            paths << p.socketPath;
    discoveryReport.inProgress = true;
    updateSummary();
    discovery->refresh(paths);
    for (auto it = containerDiscovery.begin(); it != containerDiscovery.end(); ++it)
        if (!disabledContainerHosts.contains(it.key())) {
            containerFeedback[it.key()] = "Discovering…";
            it.value()->refresh();
        }
    updateContainerSummary();
}
void Window::applyDiscovery(const QJsonArray &endpoints, const QJsonArray &issues) {
    discoveredLocalCodex = detectedLocalCodex(endpoints);
    terminal->setLocalCodexExecutable(configuredLocalCodex.isEmpty() ? discoveredLocalCodex
                                                                     : configuredLocalCodex);
    QSet<QString> present;
    for (const auto &value : endpoints) {
        const auto item = value.toObject();
        const auto path = item["path"].toString();
        if (!QDir::isAbsolutePath(path) || !validText(path))
            continue;
        QString id;
        const auto aliases = item["aliases"].toArray();
        for (const auto &p : profiles)
            if (!p.isContainer() && (p.socketPath == path || aliases.contains(p.socketPath) ||
                                     (p.socketPath.isEmpty() && item["default"].toBool()))) {
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
        if (!item["host"].toString().isEmpty()) {
            localHost = item["host"].toString();
            if (!item["user"].toString().isEmpty())
                localUser = item["user"].toString();
            localIdentityDiscovered = true;
        }
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
    if (issues.isEmpty()) {
        for (const auto &id : discoveredPresent - present)
            if (profiles.contains(id) && profiles[id].discovered && !profiles[id].isContainer())
                backends[id]->disconnectServer();
        for (const auto &id : discoveredPresent)
            if (profiles.value(id).isContainer())
                present.insert(id);
        discoveredPresent = present;
    } else
        discoveredPresent.unite(present);
    discoveryReport.issues = issues;
    discoveryReport.failure.clear();
    discoveryReport.inProgress = false;
    discoveryReport.checked = QDateTime::currentDateTime();
    discoveryReport.endpoints = endpoints.size();
    updateSummary();
}
void Window::activate(const QModelIndex &index) {
    const auto id = index.data(ThreadRole).toString();
    if (id.isEmpty())
        return;
    const auto backend = index.data(BackendRole).toString();
    const auto key = sessionKey(backend, id);
    pendingOpen.clear();
    pendingRestart = false;
    if (terminal->focusSession(key)) {
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
bool Window::showingSearch() const {
    return globalSearchActive && tabs->currentIndex() == int(SessionView::All);
}
void Window::rememberNavigation(int tab) {
    auto &navigation = explorer[tab].navigation;
    navigation.expanded = expandedKeys(tree);
    if (tree->currentIndex().isValid())
        navigation.selected = tree->currentIndex().data(KeyRole).toString();
    navigation.saved = true;
}
void Window::beginFiltering() {
    auto &state = explorer[tabs->currentIndex()];
    rememberNavigation(tabs->currentIndex());
    if (!state.filtering) {
        state.unfiltered = state.navigation;
        state.navigation.collapsed.clear();
        state.filtering = true;
    }
}
void Window::endFiltering() {
    auto &state = explorer[tabs->currentIndex()];
    if (state.filtering) {
        state.navigation = state.unfiltered;
        state.filtering = false;
    }
}
bool Window::captureScope() {
    auto &state = explorer[tabs->currentIndex()];
    state.selection.scoped = state.scopeIndex == 1;
    if (!state.selection.scoped) {
        state.selection.backends.clear();
        state.selection.path.clear();
        state.scopeLabel.clear();
        state.scopeCaptured = false;
        return true;
    }
    if (state.scopeCaptured)
        return true;
    const auto index = tree->currentIndex();
    const auto ids = index.data(BackendsRole).toStringList();
    state.selection.backends = QSet<QString>(ids.begin(), ids.end());
    const auto kind = static_cast<NodeKind>(index.data(KindRole).toInt());
    state.selection.path = kind == NodeKind::Folder || kind == NodeKind::Session
                               ? index.data(PathRole).toString()
                               : QString();
    state.scopeLabel =
        state.selection.path.isEmpty() ? index.data().toString() : state.selection.path;
    state.scopeCaptured = index.isValid();
    return state.scopeCaptured;
}
void Window::queryChanged(const QString &text) {
    auto &state = explorer[tabs->currentIndex()];
    state.query = text;
    if (tabs->currentIndex() == int(SessionView::All)) {
        if (text.trimmed().isEmpty())
            clearSearch();
        return;
    }
    // Scope is captured once, before filtering can hide the selected row.
    // Editing the query never silently widens or retargets it.
    if (text.trimmed().isEmpty()) {
        if (!state.filtering) {
            updateSearchControls();
            return;
        }
        endFiltering();
        state.selection.text.clear();
    } else {
        captureScope();
        beginFiltering();
        state.selection.text = text.trimmed();
    }
    showExplorer();
}
void Window::switchExplorer(int tab) {
    rememberNavigation(previousTab);
    auto &state = explorer[tab];
    if (!state.navigation.saved) {
        state.navigation.expanded = explorer[previousTab].navigation.expanded;
        state.navigation.selected = explorer[previousTab].navigation.selected;
    }
    const QSignalBlocker queryBlock(searchBox), scopeBlock(scope);
    searchBox->setText(state.query);
    scope->setCurrentIndex(state.scopeIndex);
    previousTab = tab;
    showExplorer();
}
void Window::showExplorer() {
    const QScopedValueRollback changing(changingExplorer, true);
    const QSignalBlocker block(tree);
    const auto &state = explorer[tabs->currentIndex()];
    filter->setSourceModel(showingSearch() ? results : model);
    filter->setView(static_cast<SessionView>(tabs->currentIndex()),
                    tabs->currentIndex() != int(SessionView::All) && state.filtering
                        ? state.selection
                        : SessionQuery{});
    tree->collapseAll();
    tree->setCurrentIndex({});
    restoreTree(tree, state.navigation.expanded, state.navigation.selected);
    expandActivityViews();
    updateSearchControls();
    if (showingSearch())
        updateSearchSummary();
}
void Window::updateSearchControls() {
    const bool all = tabs->currentIndex() == int(SessionView::All);
    searchBox->setPlaceholderText(all ? "Search conversation text — Enter"
                                      : "Filter titles, paths and session IDs");
    searchBox->setAccessibleName(all ? "Search conversations" : "Filter cached sessions");
    searchBox->setToolTip(
        all ? "Search titles and full conversation text on connected servers. Press Enter."
            : "Filter this tab immediately by cached title, working-directory path or session ID. "
              "Case-insensitive literal text; conversation bodies are not searched.");
    loadMore->setEnabled(all && !globalSearchActive &&
                         std::any_of(more.begin(), more.end(), [](bool x) { return x; }));
    const auto &state = explorer[tabs->currentIndex()];
    scope->setToolTip(state.scopeLabel.isEmpty()
                          ? "Choose all servers or select a row to set the search scope."
                          : "Selected scope: " + state.scopeLabel);
    searchNotice->setVisible(showingSearch() || (!all && state.filtering));
    if (!all && state.filtering) {
        int count = 0;
        for (int i = 0; i < filter->rowCount(); ++i)
            count += filter->index(i, 0).data(CountRole).toInt();
        searchNotice->setText(
            state.selection.scoped && !state.scopeCaptured
                ? "Select a host group, server, folder or session, then choose the selected scope "
                  "again."
                : QString("%1 matching sessions — cached titles, paths and IDs%2")
                      .arg(count)
                      .arg(state.selection.scoped ? "\nScope: " + state.scopeLabel : QString()));
    }
}
void Window::search() {
    if (tabs->currentIndex() != int(SessionView::All))
        return; // Local filters have already been applied; Enter never requests history.
    const auto query = searchBox->text().trimmed();
    if (query.isEmpty()) {
        clearSearch();
        return;
    }
    if (query.size() > 4096) {
        error("Search text is too long.");
        return;
    }
    // Each explicit submission may choose a new scope from the selected row.
    explorer[0].scopeCaptured = false;
    if (!captureScope()) {
        error("Select a host group, server, folder or session first.");
        return;
    }
    const auto &selection = explorer[0].selection;
    scopedSearch = selection.scoped;
    scopeBackends = selection.backends;
    scopePath = selection.path;
    beginFiltering();
    ++searchToken;
    globalSearchActive = true;
    searchSessions.clear();
    results->resetResults();
    searchErrors.clear();
    searching.clear();
    showExplorer();
    for (auto it = backends.begin(); it != backends.end(); ++it)
        if (!scopedSearch || scopeBackends.contains(it.key()))
            searching.insert(it.key());
    const auto ids = searching.values();
    for (const auto &id : ids)
        backends[id]->search(query, searchToken);
    updateSearchSummary();
}
void Window::clearSearch() {
    if (!globalSearchActive)
        return;
    if (tabs->currentIndex() != int(SessionView::All))
        rememberNavigation(tabs->currentIndex());
    ++searchToken;
    for (auto *b : backends)
        b->search({}, searchToken);
    globalSearchActive = false;
    searching.clear();
    searchSessions.clear();
    results->resetResults();
    // This can also be called when a connection is removed from another tab.
    auto &all = explorer[int(SessionView::All)];
    all.navigation = all.unfiltered;
    all.filtering = false;
    showExplorer();
}
void Window::updateSearchSummary() {
    if (!showingSearch())
        return;
    searchNotice->setVisible(true);
    int visible = 0;
    for (int i = 0; i < results->rowCount(); ++i)
        visible += results->item(i)->data(CountRole).toInt();
    const auto hidden = searchSessions.size() - visible;
    searchNotice->setText(
        QString("%1 results%2%3%4")
            .arg(visible)
            .arg(hidden ? QString(" (%1 hidden by visibility settings)").arg(hidden) : QString())
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
    const auto &state = explorer[tabs->currentIndex()];
    settings.setValue("expanded", state.filtering ? state.unfiltered.expanded : expandedKeys(tree));
    for (auto *b : backends)
        b->disconnectServer();
    QMainWindow::closeEvent(event);
}
} // namespace Archcodex
