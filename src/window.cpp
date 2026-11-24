// SPDX-License-Identifier: GPL-3.0-or-later
#include "window.h"
#include "asyncbackend.h"
#include "commands.h"
#include "connectionsdialog.h"
#include "discovery.h"
#include "discoverydialog.h"
#include "launch.h"
#include "notificationlog.h"
#include "notifications.h"
#include "opening.h"
#include "sessionmodel.h"
#include "settingsdialog.h"
#include "sshdialogs.h"
#include "terminal.h"
#include "updatedialog.h"
#include "updater.h"
#include <KCommandBar>
#include <KSqueezedTextLabel>
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
#include <QHideEvent>
#include <QInputDialog>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QScopedValueRollback>
#include <QScrollBar>
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
// Exclude presentation-only metadata, but reject stale edits/removals after an
// endpoint, execution environment, or user-managed configuration changes.
QJsonObject connectionDefinition(const BackendProfile &profile) {
    auto definition = profile.json();
    definition.remove("identity");
    if (profile.discovered)
        definition.remove("name");
    definition["execution"] = workspaceSourceIdentity(profile);
    definition["hostGeneration"] = profile.identity["hostGeneration"];
    return definition;
}
class TerminalTitle : public KSqueezedTextLabel {
  public:
    using KSqueezedTextLabel::KSqueezedTextLabel;

  protected:
    bool event(QEvent *event) override {
        if (event->type() == QEvent::ToolTip) {
            const auto *help = static_cast<QHelpEvent *>(event);
            // Session metadata is plain text, even when it resembles HTML.
            const auto text = accessibleName().toHtmlEscaped().replace('\n', "<br>");
            QToolTip::showText(help->globalPos(), "<p>" + text + "</p>", this);
            return true;
        }
        return KSqueezedTextLabel::event(event);
    }
};
// Keep help beside the navigator, anchored to the row rather than the pointer,
// and keep its text stable while metadata changes underneath it.
class SessionTree : public QTreeView {
    Q_OBJECT
    QPersistentModelIndex tooltipIndex;
    QPersistentModelIndex pressedSession;
    bool sessionClick = false;

  signals:
    void sessionActivated(const QModelIndex &index);
    void activationRepeated();

  protected:
    void mousePressEvent(QMouseEvent *event) override {
        sessionClick = false;
        const auto index = indexAt(event->position().toPoint());
        pressedSession = event->button() == Qt::LeftButton &&
                                 index.data(KindRole).toInt() == int(NodeKind::Session)
                             ? index
                             : QModelIndex{};
        QTreeView::mousePressEvent(event);
    }
    void mouseReleaseEvent(QMouseEvent *event) override {
        const QPersistentModelIndex target = pressedSession;
        pressedSession = QModelIndex{};
        const bool activate = event->button() == Qt::LeftButton && target.isValid() &&
                              target == indexAt(event->position().toPoint());
        QTreeView::mouseReleaseEvent(event);
        sessionClick = activate && target.isValid();
        if (sessionClick)
            emit sessionActivated(target);
    }
    void mouseDoubleClickEvent(QMouseEvent *event) override {
        // The first release already opened the session. Do not open again on
        // the second release, even if activation reordered the rows underneath.
        pressedSession = QModelIndex{};
        if (event->button() == Qt::LeftButton && sessionClick) {
            emit activationRepeated(); // Return focus without opening/recording another visit.
            event->accept();
            return;
        }
        QTreeView::mouseDoubleClickEvent(event);
    }
    void keyPressEvent(QKeyEvent *event) override {
        if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) &&
            currentIndex().isValid()) {
            if (currentIndex().data(KindRole).toInt() == int(NodeKind::Session))
                emit sessionActivated(currentIndex());
            else
                setExpanded(currentIndex(), !isExpanded(currentIndex()));
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Menu ||
            (event->key() == Qt::Key_F10 && event->modifiers() == Qt::ShiftModifier)) {
            const auto index = currentIndex();
            if (index.isValid()) {
                scrollTo(index);
                emit customContextMenuRequested(visualRect(index).center());
            } else
                emit customContextMenuRequested(QPoint(-1, -1));
            event->accept();
            return;
        }
        QTreeView::keyPressEvent(event);
    }
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
class CommandPalette : public KCommandBar {
  public:
    using KCommandBar::KCommandBar;

  protected:
    void hideEvent(QHideEvent *event) override {
        KCommandBar::hideEvent(event);
        deleteLater(); // Keep proxy actions alive until their triggered handler returns.
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
void walkContainers(QAbstractItemModel *model, const QModelIndex &parent,
                    const std::function<void(const QModelIndex &)> &visit) {
    for (int i = 0; i < model->rowCount(parent); ++i) {
        const auto child = model->index(i, 0, parent);
        if (child.data(KindRole).toInt() == int(NodeKind::Session))
            continue;
        visit(child);
        walkContainers(model, child, visit);
    }
}
QStringList expandedKeys(QTreeView *tree, const QModelIndex &root = {}) {
    QStringList keys;
    const auto capture = [&](const QModelIndex &index) {
        if (tree->isExpanded(index))
            keys << index.data(KeyRole).toString();
    };
    if (root.isValid())
        capture(root);
    walkContainers(tree->model(), root, capture);
    return keys;
}
void restoreTree(QTreeView *tree, const QStringList &keys, const QString &selected) {
    auto *proxy = qobject_cast<QSortFilterProxyModel *>(tree->model());
    auto *source = qobject_cast<SessionModel *>(proxy->sourceModel());
    // Resolve stable keys directly instead of traversing every session and
    // performing a linear list lookup for each row.
    for (const auto &key : keys) {
        const auto index = proxy->mapFromSource(source->nodeIndex(key));
        if (index.isValid())
            tree->expand(index);
    }
    const auto index = proxy->mapFromSource(source->nodeIndex(selected));
    if (index.isValid()) {
        tree->setCurrentIndex(index);
        for (auto parent = index.parent(); parent.isValid(); parent = parent.parent())
            tree->expand(parent);
    }
}
bool expandsByDefault(int index) {
    return index == int(SessionView::Running) || index == int(SessionView::Unread) ||
           index == int(SessionView::Opened);
}
const QStringList viewDescriptions{
    "All sessions, including saved history. Hollow dots mean unknown state.",
    "Live sessions, including idle prompts and errors. Hollow dots mean last-known active.",
    "Sessions currently executing a turn.",
    "Unvisited updates and unresolved input or approval requests. Viewing marks updates read; "
    "unresolved requests stay until answered.",
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
    commands = new Commands(this);
    model = new SessionModel(this);
    results = new SessionModel(this);
    filter = new SessionFilter(this);
    filter->setSourceModel(model);
    splitter = new QSplitter(this);
    setCentralWidget(splitter);
    auto *sidebar = new QWidget;
    projectPane = sidebar;
    sidebar->setMinimumWidth(240);
    auto *left = new QVBoxLayout(sidebar);
    auto *heading = new QHBoxLayout;
    auto *label = new QLabel("Projects");
    label->setObjectName("projectsHeading");
    auto f = label->font();
    f.setBold(true);
    label->setFont(f);
    heading->addWidget(label);
    heading->addStretch();
    auto *discoverButton = new QPushButton("Refresh");
    discoverButton->setObjectName("refreshConnections");
    discoverButton->setToolTip(
        "Refresh connections. Forget missing automatic connections and their local "
        "project/history cache when no open, pending or remembered terminal uses them. "
        "Codex conversation files are kept.");
    heading->addWidget(discoverButton);
    left->addLayout(heading);
    connect(discoverButton, &QPushButton::clicked, this, [this] { discover(); });
    tabs = new QTabBar;
    for (QWidget *widget : {static_cast<QWidget *>(label), static_cast<QWidget *>(tabs)}) {
        widget->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(widget, &QWidget::customContextMenuRequested, this,
                [this, widget](const QPoint &position) { projectsContextMenu(widget, position); });
    }
    tabs->addTab("All");
    tabs->addTab("Active");
    tabs->addTab("Running");
    tabs->addTab("Unread");
    tabs->addTab("Opened");
    for (int i = 0; i < tabs->count(); ++i)
        tabs->setTabToolTip(i, viewDescriptions[i]);
    tabs->setExpanding(false);
    left->addWidget(tabs);
    searchBox = new QLineEdit;
    searchBox->setObjectName("sessionSearch");
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
    searchFeedback = new QWidget;
    searchFeedback->setObjectName("searchFeedbackArea");
    auto *feedbackLayout = new QVBoxLayout(searchFeedback);
    feedbackLayout->setContentsMargins(0, 0, 0, 0);
    searchNotice = new KSqueezedTextLabel;
    searchNotice->setObjectName("searchFeedback");
    searchNotice->hide();
    searchNotice->setTextFormat(Qt::PlainText);
    feedbackLayout->addWidget(searchNotice);
    auto *progressRow = new QHBoxLayout;
    searchProgress = new KSqueezedTextLabel;
    searchProgress->setObjectName("searchProgress");
    searchProgress->setTextFormat(Qt::PlainText);
    progressRow->addWidget(searchProgress, 1);
    searchDetailsButton = new QToolButton;
    searchDetailsButton->setObjectName("searchDetailsButton");
    searchDetailsButton->setText("Details…");
    searchDetailsButton->setAccessibleName("Search coverage details");
    searchDetailsButton->setToolTip("View full search counts, progress and connection errors.");
    connect(searchDetailsButton, &QToolButton::clicked, this, &Window::showSearchDetails);
    progressRow->addWidget(searchDetailsButton);
    feedbackLayout->addLayout(progressRow);
    searchFeedback->hide();
    left->addWidget(searchFeedback);
    auto *sessionTree = new SessionTree;
    tree = sessionTree;
    tree->setModel(filter);
    tree->setHeaderHidden(true);
    tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tree->setUniformRowHeights(true);
    tree->setAnimated(false);
    tree->setAccessibleName("Projects and sessions");
    tree->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(tree, &QWidget::customContextMenuRequested, this, &Window::sessionContextMenu);
    left->addWidget(tree, 1);
    // Qt's activated signal depends on desktop single/double-click preferences.
    // Use one explicit mouse/keyboard path, separate from selection updates.
    connect(sessionTree, &SessionTree::sessionActivated, this, &Window::activate);
    connect(tabs, &QTabBar::currentChanged, this, &Window::switchExplorer);
    connect(tree, &QTreeView::collapsed, this, [this](const QModelIndex &index) {
        auto &state = explorer[tabs->currentIndex()];
        const auto key = index.data(KeyRole).toString();
        state.navigation.collapsed.insert(key);
        state.autoCollapsedServers.remove(key);
        state.browsedServers.remove(key);
    });
    connect(tree, &QTreeView::expanded, this, [this](const QModelIndex &index) {
        auto &state = explorer[tabs->currentIndex()];
        const auto key = index.data(KeyRole).toString();
        state.navigation.collapsed.remove(key);
        state.autoCollapsedServers.remove(key);
        if (autoCollapseServers && autoCollapseServers->isChecked() &&
            index.data(KindRole).toInt() == int(NodeKind::Server))
            state.browsedServers.insert(key);
    });
    for (auto *source : {model, results}) {
        connect(source, &SessionModel::branchAboutToMove, this,
                [this, source](const QModelIndex &index) {
                    movingExpanded.clear();
                    movingSelected.clear();
                    if (changingExplorer || filter->sourceModel() != source)
                        return;
                    const auto branch = filter->mapFromSource(index);
                    if (!branch.isValid())
                        return;
                    movingExpanded = expandedKeys(tree, branch);
                    for (auto selected = tree->currentIndex(); selected.isValid();
                         selected = selected.parent())
                        if (selected == branch) {
                            movingSelected = tree->currentIndex().data(KeyRole).toString();
                            break;
                        }
                });
        connect(source, &SessionModel::branchMoved, this, [this, source] {
            if (!changingExplorer && filter->sourceModel() == source) {
                const QSignalBlocker block(tree);
                restoreTree(tree, movingExpanded, movingSelected);
                updateServerExpansion();
            }
        });
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
            walkContainers(filter, {}, [&](const QModelIndex &root) {
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
            if (source != results)
                syncCurrentSession(); // Search pages preserve browsing/scroll position.
            else
                updateServerExpansion();
        });
    }
    connect(filter, &QAbstractItemModel::rowsInserted, this,
            [this](const QModelIndex &parent, int first, int last) {
                if (changingExplorer)
                    return;
                const QSignalBlocker block(tree);
                const auto &state = explorer[tabs->currentIndex()];
                const auto expand = [&](const QModelIndex &index) {
                    const auto kind = static_cast<NodeKind>(index.data(KindRole).toInt());
                    const auto key = index.data(KeyRole).toString();
                    if (kind == NodeKind::Session)
                        return;
                    const bool root = kind == NodeKind::Group || kind == NodeKind::Server;
                    if ((root && !seenRoots.contains(key)) ||
                        ((state.filtering || expandsByDefault(tabs->currentIndex())) &&
                         !state.navigation.collapsed.contains(key)))
                        tree->expand(index);
                    if (root)
                        seenRoots.insert(key);
                };
                bool branchInserted = false;
                for (int row = first; row <= last; ++row) {
                    const auto index = filter->index(row, 0, parent);
                    if (index.data(KindRole).toInt() == int(NodeKind::Session))
                        continue;
                    branchInserted = true;
                    expand(index);
                    // A previously filtered-out root can expose an already
                    // populated branch in a single insertion notification.
                    walkContainers(filter, index, expand);
                }
                if (branchInserted)
                    updateServerExpansion();
            });
    loadMore = new QPushButton("Load more history");
    loadMore->setEnabled(false);
    left->addWidget(loadMore);
    connect(model, &SessionModel::topologyChanged, this, [this] {
        if (!changingExplorer)
            updateSearchControls();
    });
    connect(model, &QAbstractItemModel::dataChanged, this,
            [this](const QModelIndex &first, const QModelIndex &, const QList<int> &roles) {
                // Incremental membership changes update top-level aggregates
                // without regrouping. Cached-filter counts must follow them too.
                if (!changingExplorer && !first.parent().isValid() &&
                    (roles.isEmpty() || roles.contains(CountRole))) {
                    updateSearchControls();
                    // A displayed conversation may have just re-entered a local
                    // filter. Restore its missing selection without disturbing browsing.
                    if (filter->sourceModel() == model && !tree->currentIndex().isValid())
                        syncCurrentSession();
                }
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
    auto *terminalTitle = new TerminalTitle("No terminal selected");
    terminalTitle->setObjectName("terminalTitle");
    terminalTitle->setAccessibleName("No terminal selected");
    terminalTitle->setTextFormat(Qt::PlainText);
    terminalTitle->setTextInteractionFlags(Qt::TextSelectableByMouse);
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
    connect(terminal, &TerminalArea::currentChanged, terminalTitle,
            [terminalTitle](const QString &title) {
                // Metadata may contain line separators. Keep one font-sized row;
                // KDE supplies elision, a full-text tooltip and Copy Full Text.
                auto line = title;
                for (auto &c : line)
                    if (c.isSpace())
                        c = QLatin1Char(' ');
                terminalTitle->setText(line);
                terminalTitle->setAccessibleName(title);
            });
    connect(terminal, &TerminalArea::clientStateChanged, this, &Window::clientStateChanged);
    connect(terminal, &TerminalArea::restartRequested, this, &Window::restartTerminal);
    connect(terminal, &TerminalArea::sessionFocused, this, &Window::terminalFocused);
    connect(sessionTree, &SessionTree::activationRepeated, terminal, &TerminalArea::focusCurrent);
    connect(terminal, &TerminalArea::currentSessionChanged, this, [this](const QString &key) {
        model->setReviewedSession({});
        if (key.isEmpty()) {
            tree->selectionModel()->clear();
            for (auto &state : explorer) {
                state.navigation.selected.clear();
                state.unfiltered.selected.clear();
            }
            updateServerExpansion();
        } else
            syncCurrentSession();
    });
    connections = new QLabel;
    connections->setTextFormat(Qt::PlainText);
    statusBar()->addWidget(connections, 1);
    discoveryStatus = new QToolButton;
    discoveryStatus->setObjectName("discoveryStatus");
    discoveryStatus->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    discoveryStatus->setAutoRaise(true);
    statusBar()->addPermanentWidget(discoveryStatus);
    hostStatus = new QToolButton;
    hostStatus->setObjectName("hostDiscoveryStatus");
    hostStatus->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    hostStatus->setAutoRaise(true);
    hostStatus->setText("Hosts…");
    statusBar()->addPermanentWidget(hostStatus);
    connect(hostStatus, &QToolButton::clicked, this, &Window::hostsDialog);
    connect(discoveryStatus, &QToolButton::clicked, this, &Window::showDiscoveryDetails);
    auto *projectsMenu = menuBar()->addMenu("&Session");
    projectsMenu->addAction(commands->add("new_session", "New session in current project",
                                          "Sessions", {}, [this] { contextualCommand("new"); }));
    projectsMenu->addAction(commands->add("fork_session", "Fork current session", "Sessions", {},
                                          [this] { contextualCommand("fork"); }));
    projectsMenu->addAction(commands->add("restart_session", "Restart exited client", "Sessions",
                                          {}, [this] { contextualCommand("restart"); }));
    for (const auto projects : {false, true})
        for (const auto backwards : {false, true}) {
            const auto id =
                QString("cycle_%1_%2")
                    .arg(projects ? "projects" : "terminals", backwards ? "previous" : "next");
            const auto key = QKeySequence(Qt::ControlModifier |
                                          (backwards ? Qt::ShiftModifier : Qt::NoModifier) |
                                          (projects ? Qt::Key_QuoteLeft : Qt::Key_Tab));
            auto *action = commands->add(
                id,
                QString("%1 recent %2")
                    .arg(backwards ? "Previous" : "Next", projects ? "project" : "terminal"),
                "Navigation", {key},
                [this, projects, backwards] { terminal->switchRecent(backwards, projects); });
            action->setProperty("cycleKind", projects ? 2 : 1);
            action->setProperty("cycleBackwards", backwards);
            projectsMenu->addAction(action);
        }
    projectsMenu->addAction(commands->add("close_terminal", "Close current terminal", "Sessions",
                                          {QKeySequence("Ctrl+Shift+W")},
                                          [this] { terminal->closeCurrent(); }));
    projectsMenu->addSeparator();
    projectsMenu->addAction(
        commands->add("quit", "Quit", "Application", {}, [this] { this->close(); }));
    auto *connectionMenu = menuBar()->addMenu("&Connections");
    auto *refreshAction =
        commands->add("discover", "Refresh connections", "Connections", {}, [this] { discover(); });
    refreshAction->setToolTip(discoverButton->toolTip());
    refreshAction->setStatusTip(discoverButton->toolTip());
    connectionMenu->addAction(refreshAction);
    connectionMenu->addAction(commands->add("connections", "Manage connections…", "Connections", {},
                                            [this] { connectionsDialog(); }));
    connectionMenu->addAction(commands->add("discovery_details", "Discovery details…",
                                            "Connections", {}, [this] { showDiscoveryDetails(); }));
    connectionMenu->addAction(
        commands->add("mycodex_hosts", "Hosts…", "Connections", {}, [this] { hostsDialog(); }));
    auto *view = menuBar()->addMenu("&View");
    auto *hideUnsaved = view->addAction("Hide unsaved sessions");
    hideUnsaved->setObjectName("hideUnsavedSessions");
    hideUnsaved->setCheckable(true);
    QSettings settings(directory + "/window.ini", QSettings::IniFormat);
    configuredLocalCodex = settings.value("application/localCodexExecutable").toString();
    notifications = new Notifications(this);
    notifications->setEnabled(settings.value("notifications/enabled", true).toBool());
    connect(notifications, &Notifications::openRequested, this,
            [this](const QString &backend, const QString &thread, const QString &token) {
                if (closing)
                    return;
                Notifications::presentWindow(this, token);
                activateSession(backend, thread, true);
            });
    terminal->setLocalCodexExecutable(configuredLocalCodex);
    hideUnsaved->setChecked(settings.value("hideUnsavedSessions", true).toBool());
    model->setHideUnsaved(hideUnsaved->isChecked());
    results->setHideUnsaved(hideUnsaved->isChecked());
    const bool workdirOnly = !settings.value("backends/myCodex/showAllSessions", false).toBool();
    notifications->setMyCodexWorkdirOnly(workdirOnly);
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
        updateUnreadCount();
        if (globalSearchActive)
            updateSearchSummary();
    });
    autoCollapseServers = view->addAction("Auto-collapse other servers");
    autoCollapseServers->setObjectName("autoCollapseOtherServers");
    autoCollapseServers->setCheckable(true);
    autoCollapseServers->setChecked(
        settings.value("projects/autoCollapseOtherServers", false).toBool());
    autoCollapseServers->setToolTip(
        "Keep the displayed session's server expanded. Other servers collapse on terminal "
        "switch; host groups stay open and you can temporarily expand another server to browse.");
    commands->adopt("auto_collapse_servers", autoCollapseServers, "Projects");
    connect(autoCollapseServers, &QAction::toggled, this, [this](bool enabled) {
        QSettings settings(dataDirectory + "/window.ini", QSettings::IniFormat);
        settings.setValue("projects/autoCollapseOtherServers", enabled);
        for (auto &state : explorer)
            state.browsedServers.clear();
        if (enabled)
            syncCurrentSession();
        else {
            updateServerExpansion();
            // Inactive tabs already captured their logical expansion on leaving them.
            for (auto &state : explorer)
                state.autoCollapsedServers.clear();
        }
    });
    view->addAction(commands->add("focus_projects", "Focus Projects", "Navigation",
                                  {QKeySequence("Ctrl+Alt+P")},
                                  [this] { tree->setFocus(Qt::ShortcutFocusReason); }));
    view->addAction(commands->add("focus_search", "Focus project search / filter", "Navigation",
                                  {QKeySequence("Ctrl+Alt+F")},
                                  [this] { searchBox->setFocus(Qt::ShortcutFocusReason); }));
    view->addAction(commands->add("focus_terminal", "Focus terminal", "Navigation",
                                  {QKeySequence("Alt+0")}, [this] { terminal->focusCurrent(); }));
    view->addAction(commands->add("focus_next_pane", "Switch focus between Projects and terminal",
                                  "Navigation", {QKeySequence("F6"), QKeySequence("Shift+F6")},
                                  [this] {
                                      if (terminal->isAncestorOf(QApplication::focusWidget()) ||
                                          terminal->currentKey().isEmpty())
                                          tree->setFocus(Qt::ShortcutFocusReason);
                                      else
                                          terminal->focusCurrent();
                                  }));
    view->addAction(commands->add("command_palette", "Command palette…", "Navigation",
                                  {QKeySequence("Ctrl+Shift+P")},
                                  [this] { showCommandPalette(); }));
    view->addAction(
        commands->add("focus_menu", "Focus menu bar", "Navigation", {QKeySequence("F10")}, [this] {
            menuBar()->setFocus(Qt::ShortcutFocusReason);
            menuBar()->setActiveAction(menuBar()->actions().value(0));
        }));
    view->addAction(commands->add("expand_branch", "Expand selected branch", "Projects", {},
                                  [this] { contextualCommand("expand"); }));
    view->addSeparator();
    // Keep the stored action ID so customized Alt+4 bindings survive the rename.
    const QStringList viewIds{"all", "active", "running", "waiting", "opened"};
    for (int i = 0; i < tabs->count(); ++i)
        view->addAction(commands->add("view_" + viewIds[i], "Show " + tabs->tabText(i), "Projects",
                                      {QKeySequence(QString("Alt+%1").arg(i + 1))}, [this, i] {
                                          tabs->setCurrentIndex(i);
                                          tree->setFocus(Qt::ShortcutFocusReason);
                                      }));
    view->addAction(commands->add("next_unread", "Next unread session", "Projects", {}, [this] {
        QList<Session> pending;
        for (const auto &key : std::as_const(attentionKeys)) {
            const auto session = sessions.value(key, searchSessions.value(key));
            if (model->passesVisibilitySettings(session))
                pending << session;
        }
        std::sort(pending.begin(), pending.end(), [](const auto &a, const auto &b) {
            // Visits must not reorder this cycle while an unresolved request
            // remains eligible. Activity order is stable across those visits.
            const auto x = a.recency, y = b.recency;
            return x != y ? x > y : sessionKey(a.backendId, a.id) > sessionKey(b.backendId, b.id);
        });
        if (pending.isEmpty())
            return;
        int next = 0;
        for (int i = 0; i < pending.size(); ++i)
            if (sessionKey(pending[i].backendId, pending[i].id) == terminal->currentKey())
                next = (i + 1) % pending.size();
        activateSession(pending[next].backendId, pending[next].id);
    }));
    commands->collection()->action("next_unread")->setEnabled(false);
    auto *terminalMenu = menuBar()->addMenu("&Terminal");
    terminalMenu->addAction(commands->add("terminal_find", "Find in terminal scrollback",
                                          "Terminal", {QKeySequence("Ctrl+Shift+F")}, [this] {
                                              terminal->focusCurrent();
                                              terminal->triggerAction("edit_find");
                                          }));
    for (const auto &entry : QList<QPair<QString, QString>>{{"copy", "Copy"}, {"paste", "Paste"}}) {
        const bool copy = entry.first == "copy";
        terminalMenu->addAction(commands->add(
            "terminal_" + entry.first, entry.second, "Terminal",
            copy ? QList<QKeySequence>{QKeySequence("Ctrl+C"), QKeySequence("Ctrl+Shift+C"),
                                       QKeySequence("Ctrl+Insert")}
                 : QList<QKeySequence>{QKeySequence("Ctrl+Shift+V"), QKeySequence("Shift+Insert")},
            [this, name = "edit_" + entry.first] { terminal->triggerAction(name); }, terminal));
    }
    terminalMenu->addAction(commands->add("terminal_print", "Print terminal screen…", "Terminal",
                                          {}, [this] { terminal->triggerAction("file_print"); }));
    auto *settingsMenu = menuBar()->addMenu("S&ettings");
    settingsMenu->addAction(commands->add("configureArchcodex", "Configure Archcodex…",
                                          "Application", {QKeySequence("Ctrl+Shift+,")},
                                          [this] { showSettings(); }));
    terminal->setCommands(commands);
    auto *help = menuBar()->addMenu("&Help");
    updater = new Updater(ARCHCODEX_VERSION, dataDirectory, this);
    updateStatus = new QToolButton(this);
    updateStatus->setObjectName("updateStatus");
    updateStatus->setAutoRaise(true);
    statusBar()->addPermanentWidget(updateStatus);
    connect(updateStatus, &QToolButton::clicked, this, [this] { showUpdates(false); });
    connect(updater, &Updater::changed, this, &Window::updateUpdateStatus);
    connect(updater, &Updater::restartReady, this, [this] { this->close(); });
    updateUpdateStatus();
    help->addAction(commands->add("check_updates", "Check for Updates…", "Application", {},
                                  [this] { showUpdates(); }));
    auto *about = help->addAction("About Archcodex", this, [this] {
        QMessageBox::about(
            this, "Archcodex",
            QString("Archcodex %1\nNative Codex session workspace\n\nLocal Unix sockets · "
                    "event-driven status · Konsole terminal\nGPL-3.0-or-later")
                .arg(ARCHCODEX_VERSION));
    });
    commands->adopt("about", about, "Application");
    commands->adopt("hideUnsavedSessions", hideUnsaved, "Application");
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget *, QWidget *focused) {
        if (focused && (focused == projectPane || projectPane->isAncestorOf(focused)))
            projectsContext = true;
        else if (focused && (focused == terminal || terminal->isAncestorOf(focused)))
            projectsContext = false;
        updateCommandAvailability();
    });
    connect(tree->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this] { updateCommandAvailability(); });
    connect(terminal, &TerminalArea::currentChanged, this, [this] {
        updateCommandAvailability();
        updateViewedSession();
    });
    connect(terminal, &TerminalArea::clientStateChanged, this,
            [this] { updateCommandAvailability(); });
    connect(this, &Window::connectionSummaryChanged, this, &Window::updateCommandAvailability);
    updateCommandAvailability();
    const auto shortcutIssue = commands->load(directory + "/shortcutsrc");
    if (!shortcutIssue.isEmpty())
        statusBar()->showMessage("Using default keyboard shortcuts: " + shortcutIssue);
    discovery = new HostDiscovery(this);
    connect(discovery, &HostDiscovery::snapshot, this, &Window::applyDiscovery);
    connect(discovery, &HostDiscovery::failed, this, [this](const QString &message) {
        refreshScopeFinished("local", {message});
        discoveryReport.failure = message;
        discoveryReport.issues = {};
        discoveryReport.inProgress = false;
        discoveryReport.checked = QDateTime::currentDateTime();
        updateSummary();
    });
    const bool legacyHosts = !settings.contains("hostsV2") && !settings.contains("hosts");
    const auto records = QJsonDocument::fromJson(settings
                                                     .value(settings.contains("hostsV2") ? "hostsV2"
                                                            : legacyHosts ? "containerHosts"
                                                                          : "hosts")
                                                     .toByteArray())
                             .array();
    for (const auto &host : readHosts(records, legacyHosts))
        addHost(host, false);
    opening = new OpeningCoordinator(this);
    workspaceStatus = new QToolButton(this);
    workspaceStatus->setObjectName("workspaceStatus");
    workspaceStatus->setAutoRaise(true);
    workspaceStatus->setToolTip("Remembered terminal views and restoration details");
    statusBar()->addPermanentWidget(workspaceStatus);
    connect(workspaceStatus, &QToolButton::clicked, this, &Window::showWorkspaceDetails);
    connect(opening, &OpeningCoordinator::stateChanged, terminal, &TerminalArea::openingState);
    connect(opening, &OpeningCoordinator::progressChanged, this, &Window::updateWorkspaceStatus);
    connect(opening, &OpeningCoordinator::launchRequested, this,
            [this](const QString &view, const BackendProfile &profile, Session session) {
                if (closing)
                    return;
                const auto key = sessionKey(profile.id, session.id);
                session.opened = sessions.value(key).opened;
                QString issue;
                if (!terminal->startPrepared(view, profile, session, &issue))
                    terminal->openingState(view, issue, true);
            });
    connect(terminal, &TerminalArea::viewClosed, opening, &OpeningCoordinator::cancel);
    connect(terminal, &TerminalArea::connectionsRequested, this, &Window::connectionsDialog);
    connect(terminal, &TerminalArea::workspaceChanged, this, &Window::queueWorkspaceSave);
    connect(store.get(), &AsyncStore::workspaceFailed, this, [this](const QString &error) {
        workspaceError = error;
        workspaceWritable = false;
        updateWorkspaceStatus();
    });
    connect(store.get(), &AsyncStore::workspaceSaved, this,
            [this](const WorkspaceSnapshot &snapshot) {
                savedWorkspaceBackends.clear();
                for (const auto &view : snapshot.views)
                    savedWorkspaceBackends.insert(view.backendId);
            });
    connect(store.get(), &AsyncStore::backendRetired, this,
            [this](const QString &id, quint64 token, const QString &error) {
                if (retirements.value(id) != token)
                    return;
                if (!error.isEmpty() || !profiles.value(id).discovered || protectsConnection(id)) {
                    cancelRetirement(id);
                    if (!error.isEmpty())
                        discoveryReport.refreshIssues.append(QJsonObject{
                            {"code", "retirement-" + id},
                            {"summary", "Could not forget " + profiles.value(id).name},
                            {"explanation", "The connection and its cache remain available."},
                            {"action", "Check metadata storage access and refresh again."},
                            {"details", QJsonArray{error}}});
                } else {
                    retirements.remove(id);
                    forgetConnection(id);
                    ++refreshRemoved;
                }
                updateRefreshSummary();
            });
    refreshDeadline.setSingleShot(true);
    connect(&refreshDeadline, &QTimer::timeout, this, [this] {
        const auto pending = refreshScopes;
        for (const auto &id : pending)
            refreshScopeFinished(id, {"The fresh inventory did not finish within 60 seconds; "
                                      "connections were retained."});
    });
    updateWorkspaceStatus();
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
                attach(p, connectOnStart && p.autoConnect && !p.discovered && p.isLocal());
            }
            for (const auto &state : cache.readStates) {
                const auto key = sessionKey(state.backendId, state.id);
                if (!readStates.contains(key) && !readThrough.contains(key))
                    readStates[key] = state;
            }
            workspaceError = cache.workspaceError;
            workspaceWritable = workspaceError.isEmpty();
            workspaceRevision = cache.workspace.revision;
            for (const auto &view : cache.workspace.views)
                savedWorkspaceBackends.insert(view.backendId);
            const bool restore = QSettings(dataDirectory + "/window.ini", QSettings::IniFormat)
                                     .value("application/restoreOpenedTerminals", true)
                                     .toBool();
            if (workspaceWritable && restore) {
                QSet<QString> remembered;
                for (const auto &view : cache.workspace.views)
                    remembered.insert(view.key());
                for (auto session : cache.sessions) {
                    applyReadState(session);
                    const auto key = sessionKey(session.backendId, session.id);
                    if (remembered.contains(key) && !sessions.contains(key))
                        sessions[key] = session;
                }
                terminal->restoreWorkspace(cache.workspace, profiles);
                // Selected first, then saved MRU. Placeholders require no processes.
                auto order = cache.workspace.recent;
                if (!cache.workspace.selected.isEmpty()) {
                    order.removeAll(cache.workspace.selected);
                    order.prepend(cache.workspace.selected);
                }
                QHash<QString, WorkspaceView> views;
                for (const auto &view : cache.workspace.views)
                    views[view.id] = view;
                for (const auto &id : order)
                    if (views[id].disposition == ViewDisposition::Automatic)
                        opening->request(views[id], true);
            }
            workspaceLoaded = true;
            updateWorkspaceStatus();
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
    if (connectOnStart && QFile::exists("/.flatpak-info") &&
        settings.value("updates/checkOnStartup", true).toBool())
        QTimer::singleShot(0, updater, &Updater::check);
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
        applyReadState(s);
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
    syncCurrentSession();
    // Launch is interactive even when a refresh was queued during cache restoration.
    // Later automatic recovery must not inherit permission to reopen prompts.
    if (refreshQueued) {
        refreshQueued = false;
        discover(true, connectOnStart);
    } else if (connectOnStart)
        discover(false, true);
}
Window::~Window() {
    freezeWorkspace();
    qApp->disconnect(this);
    discovery->disconnect(this);
    delete discovery;
    for (auto *b : backends) {
        b->disconnect(this);
        delete b;
    }
    for (auto *d : hostDiscovery) {
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
    notifications->withdrawBackend(p.id);
    qCDebug(notificationLog) << "bind backend" << p.id << "container" << p.containerId << "epoch"
                             << p.containerEpoch << "endpoint" << p.socketPath << "relay"
                             << p.relaySocket;
    for (auto it = pendingForks.begin(); it != pendingForks.end();)
        if (it->startsWith(p.id + QChar(0x1f)))
            it = pendingForks.erase(it);
        else
            ++it;
    presentProfile(p);
    auto *b =
        new AsyncBackend(p, QCoreApplication::applicationDirPath() + "/archcodex-bridge", this);
    backends[p.id] = b;
    connect(b, &QObject::destroyed, this, [this] {
        for (auto it = pendingStarts.begin(); it != pendingStarts.end();)
            if (it.value().isNull())
                it = pendingStarts.erase(it);
            else
                ++it;
    });
    connect(b, &Backend::loadedInventory, this,
            [this, b = QPointer<Backend>(b), id = p.id](const QSet<QString> &loaded) {
                if (backends.value(id) != b)
                    return;
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
                for (const auto &session : updates)
                    terminal->updateSession(session, profiles.value(session.backendId).name);
                model->upsertMany(updates);
                results->upsertMany(matches);
            });
    connect(b, &Backend::connectionChanged, this,
            [this, b = QPointer<Backend>(b), id = p.id](const ConnectionState &state) {
                const bool ready = state.ready();
                if (backends.value(id) != b)
                    return;
                connectionStates[id] = state;
                connectionReady[id] = ready;
                if (!ready) {
                    notifications->withdrawBackend(id);
                    QList<Session> updates, matches;
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
                    for (const auto &session : updates)
                        terminal->updateSession(session, profiles.value(session.backendId).name);
                    model->upsertMany(updates);
                    results->upsertMany(matches);
                    if (globalSearchActive && searching.remove(id)) {
                        recordSearchError(id, "Search interrupted by disconnection.");
                        updateSearchSummary();
                    }
                }
                updateSummary();
            });
    connect(b, &Backend::sessionChanged, this,
            [this, b = QPointer<Backend>(b), id = p.id](const Session &s) {
                if (backends.value(id) == b)
                    applySessions({s});
            });
    connect(b, &Backend::sessionsChanged, this,
            [this, b = QPointer<Backend>(b), id = p.id](const QList<Session> &items) {
                if (backends.value(id) == b)
                    applySessions(items);
            });
    connect(b, &Backend::attentionRequested, this,
            [this, b = QPointer<Backend>(b), id = p.id](const SessionAttention &event) {
                if (!closing && backends.value(id) == b && b->ready() && profiles.contains(id)) {
                    updateViewedSession();
                    receivedAttention(event);
                    notifications->show(event, profiles[id]);
                }
            });
    connect(b, &Backend::sessionRemoved, this,
            [this, b = QPointer<Backend>(b), id = p.id](const QString &thread) {
                if (backends.value(id) != b)
                    return;
                notifications->withdrawSession(id, thread);
                const auto key = sessionKey(id, thread);
                readStates.remove(key);
                attentionKeys.remove(key);
                updateUnreadCount();
                readThrough[key] = SessionAttention::latestSequence();
                if (sessions.value(key).terminalOpen) {
                    auto &s = sessions[key];
                    s.unread = false;
                    s.unavailable = true;
                    s.known = false;
                    terminal->updateSession(s, profiles.value(id).name);
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
    connect(b, &Backend::moreAvailable, this,
            [this, b = QPointer<Backend>(b), id = p.id](bool available) {
                if (backends.value(id) != b)
                    return;
                more[id] = available;
                updateSearchControls();
            });
    connect(
        b, &Backend::searchResults, this,
        [this, b = QPointer<Backend>(b), id = p.id](quint64 token, const QList<Session> &matches,
                                                    const QString &error, bool complete) {
            if (backends.value(id) != b || token != searchToken || !globalSearchActive)
                return;
            QList<Session> updates;
            for (auto s : matches) {
                if (scopedSearch && (!scopeBackends.contains(s.backendId) ||
                                     (!scopePath.isEmpty() && s.cwd != scopePath &&
                                      !s.cwd.startsWith(scopePath == "/" ? "/" : scopePath + "/"))))
                    continue;
                const auto key = sessionKey(s.backendId, s.id);
                if (sessions.contains(key))
                    s = sessions[key];
                else
                    s.known = false;
                applyReadState(s);
                searchSessions[key] = s;
                updates << s;
            }
            results->upsertMany(updates);
            // Select a displayed conversation when it first arrives, without
            // repeatedly revealing it over the user's subsequent navigation.
            if (showingSearch() && !tree->currentIndex().isValid()) {
                const auto index =
                    filter->mapFromSource(results->sessionIndex(terminal->currentKey()));
                if (index.isValid())
                    tree->setCurrentIndex(index);
            }
            if (!error.isEmpty())
                recordSearchError(id, error);
            if (complete)
                searching.remove(id);
            updateSearchSummary();
        });
    connect(b, &Backend::identityReceived, this,
            [this, b = QPointer<Backend>(b), id = p.id](const QJsonObject &identity) {
                if (backends.value(id) != b || !profiles.contains(id))
                    return;
                auto &profile = profiles[id];
                for (auto it = identity.begin(); it != identity.end(); ++it)
                    profile.identity[it.key()] = it.value();
                if (profile.isContainer() && identity["codexHome"].isString())
                    profile.identity["serverCodexHome"] = identity["codexHome"];
                opening->updateProfile(profile);
                refreshNames();
                persist([&] { store->save(profile); });
            });
    connect(b, &Backend::sessionForked, this,
            [this, b = QPointer<Backend>(b),
             id = p.id](const QString &source, const Session &session, const QString &issue) {
                if (backends.value(id) != b)
                    return;
                if (!pendingForks.remove(sessionKey(id, source)))
                    return;
                if (!issue.isEmpty()) {
                    error("Could not fork the session:\n" + issue);
                    return;
                }
                applySessions({session});
                openSession(id, sessions.value(sessionKey(id, session.id)));
            });
    connect(b, &Backend::sessionStarted, this,
            [this, b = QPointer<Backend>(b), id = p.id](quint64 token, const Session &session,
                                                        const QString &issue) {
                if (pendingStarts.take(token) != b || backends.value(id) != b || closing)
                    return; // A replaced/removed source cannot open a terminal from a late reply.
                if (!issue.isEmpty()) {
                    error("Could not open the new session:\n" + issue);
                    return;
                }
                applySessions({session});
                openSession(id, sessions.value(sessionKey(id, session.id)));
            });
    opening->setSource(p, b);
    if (start)
        QTimer::singleShot(0, b, &Backend::connectServer);
    else
        connectionStates[p.id] = {};
}
void Window::newSession(const QString &backend, const QString &path) {
    cancelRetirement(backend);
    if (!profiles.contains(backend) || !backends.value(backend) || !backends[backend]->ready()) {
        error("Connect the selected project's backend first.");
        return;
    }
    if (!validText(path) || !QDir::isAbsolutePath(path)) {
        error("Select a project with an absolute working directory.");
        return;
    }
    if (terminal->workspace().views.size() + pendingStarts.size() >= 512) {
        statusBar()->showMessage("Close a terminal before opening more than 512 views.");
        return;
    }
    const auto token = ++nextStartToken;
    pendingStarts[token] = backends[backend];
    statusBar()->showMessage("Creating new session…", 5000);
    backends[backend]->startSession(path, token);
}
void Window::forkSession(const QString &backend, const QString &thread) {
    cancelRetirement(backend);
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
    if (!index.isValid()) {
        projectsContextMenu(tree->viewport(), position.x() < 0 ? QPoint{} : position);
        return;
    }
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
        const bool unread = sessions.value(key, searchSessions.value(key)).unread;
        auto *mark = menu->addAction(unread ? "Mark as read" : "Mark as unread", this,
                                     [this, key, unread] { setUnread(key, !unread); });
        mark->setObjectName("toggleUnread");
        mark->setEnabled(!thread.isEmpty() && !thread.startsWith("new-"));
        mark->setToolTip("Read state does not dismiss an unresolved input or approval request.");
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
    menu->addSeparator();
    menu->addAction(autoCollapseServers);
    menu->popup(tree->viewport()->mapToGlobal(position));
}
void Window::projectsContextMenu(QWidget *widget, const QPoint &position) {
    auto *menu = new QMenu(widget);
    menu->setObjectName("projectsContextMenu");
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->addAction(autoCollapseServers);
    menu->popup(widget->mapToGlobal(position));
}
QStringList Window::navigationExpansion(int tab) const {
    auto keys = expandedKeys(tree);
    const auto *source = qobject_cast<SessionModel *>(filter->sourceModel());
    for (const auto &key : explorer.value(tab).autoCollapsedServers)
        if (source && source->nodeIndex(key).isValid() && !keys.contains(key))
            keys << key;
    return keys;
}
void Window::updateServerExpansion() {
    if (!autoCollapseServers)
        return;
    auto &state = explorer[tabs->currentIndex()];
    const bool enabled = autoCollapseServers->isChecked();
    if (!enabled && state.autoCollapsedServers.isEmpty())
        return;
    const auto current = terminal ? terminal->currentKey() : QString();
    if (expansionSession != current) {
        expansionSession = current;
        for (auto &view : explorer)
            view.browsedServers.clear();
    }
    const auto *source = qobject_cast<SessionModel *>(filter->sourceModel());
    const auto index =
        source ? filter->mapFromSource(source->sessionIndex(current)) : QModelIndex{};
    const QSignalBlocker block(tree);
    if (!enabled || !index.isValid()) {
        // A tab/query can exclude the displayed session. Keep its results browsable,
        // and never alter filters or pretend another session is the current one.
        restoreTree(tree, state.autoCollapsedServers.values(), {});
        state.autoCollapsedServers.clear();
        return;
    }
    const auto backend = index.data(BackendRole).toString();
    QSet<QString> present;
    const auto visit = [&](auto &&self, const QModelIndex &parent) -> void {
        for (int row = 0; row < filter->rowCount(parent); ++row) {
            const auto node = filter->index(row, 0, parent);
            const auto kind = static_cast<NodeKind>(node.data(KindRole).toInt());
            if (kind == NodeKind::Group) {
                self(self, node);
            } else if (kind == NodeKind::Server) {
                const auto key = node.data(KeyRole).toString();
                present.insert(key);
                if (node.data(BackendRole).toString() != backend &&
                    !state.browsedServers.contains(key)) {
                    if (tree->isExpanded(node)) {
                        state.autoCollapsedServers.insert(key);
                        tree->collapse(node);
                    }
                } else if (state.autoCollapsedServers.remove(key))
                    tree->expand(node);
                // Only host/group and server rows: never traverse folders or sessions.
            }
        }
    };
    visit(visit, {});
    state.autoCollapsedServers.intersect(present);
}
void Window::expandActivityViews() {
    const auto &state = explorer[tabs->currentIndex()];
    if (!state.filtering && !expandsByDefault(tabs->currentIndex()))
        return;
    const QSignalBlocker block(tree);
    const auto &closed = state.navigation.collapsed;
    walkContainers(filter, {}, [&](const QModelIndex &index) {
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
        s.clientPending = old.clientPending;
        s.contextRequired = profiles[s.backendId].isContainer();
        if (s.contextRequired) {
            const auto environment = profiles[s.backendId].environmentId;
            s.executionObserved =
                (s.executionObserved && s.executionEnvironment == environment) ||
                (old.executionObserved && old.executionEnvironment == environment);
            s.executionEnvironment = environment;
        }
        applyReadState(s);
        sessions[key] = s;
        terminal->updateSession(s, profiles[s.backendId].name);
        updates << s;
        store->save(s);
        if (searchSessions.contains(key)) {
            searchSessions[key] = s;
            matches << s;
        }
    }
    model->upsertMany(updates);
    results->upsertMany(matches);
    updateCommandAvailability();
    if (globalSearchActive && !matches.isEmpty())
        updateSearchSummary();
}
void Window::addHost(const HostProfile &profile, bool start) {
    if (hosts.contains(profile.id))
        return;
    auto host = profile;
    if (host.isLocal())
        host.enabled = host.servers = true;
    hosts[host.id] = host;
    auto *d = new SharedHostDiscovery(host.id, host.ssh, this);
    hostDiscovery[host.id] = d;
    hostFeedback[host.id] = host.enabled ? "Not connected" : "Disabled";
    connect(d, &SharedHostDiscovery::snapshot, this,
            [this, id = host.id](const QJsonObject &data) { applyHostInventory(id, data); });
    connect(d, &SharedHostDiscovery::failed, this, [this, id = host.id](const QString &error) {
        if (!hosts.contains(id) || !hosts[id].enabled)
            return;
        updateAuthentication(id, {});
        refreshScopeFinished(id, {error});
        hostFeedback[id] = error;
        hostProblems.insert(id);
        updateHostSummary();
    });
    if (start)
        refreshHost(host.id, {}, true);
    updateHostSummary();
}
void Window::updateHost(const HostProfile &host) {
    if (!hosts.contains(host.id) || hosts[host.id].isLocal() || !host.validate().isEmpty())
        return;
    if (hosts[host.id].connectionKey() == host.connectionKey() && hosts[host.id].ssh == host.ssh)
        return;
    updateAuthentication(host.id, {});
    delete hostDiscovery.take(host.id);
    for (auto &profile : profiles) {
        if (profile.discoveryHost != host.id)
            continue;
        if (auto *backend = backends.value(profile.id))
            backend->disconnectServer();
        profile.relaySocket.clear();
        discoveredPresent.remove(profile.id);
        opening->updateProfile(profile);
    }
    hosts.remove(host.id);
    hostNames.remove(host.id);
    hostUsers.remove(host.id);
    auto replacement = host;
    replacement.generation = QUuid::createUuid().toString(QUuid::Id128);
    addHost(replacement, host.enabled);
    refreshNames();
    saveHosts();
}
void Window::updateAuthentication(const QString &id, const QJsonArray &prompts) {
    QJsonObject selected;
    for (const auto &value : prompts) {
        const auto p = value.toObject();
        if (!p["id"].isString() || !p["attempt"].isString() || !p["text"].isString() ||
            p["text"].toString().size() > 8192)
            continue;
        if (selected.isEmpty() || p["kind"] != "none")
            selected = p;
        if (p["kind"] != "none")
            break;
    }
    if (selected.isEmpty()) {
        authenticationQueue.removeAll(id);
        if (auto dialog = authenticationDialogs.take(id))
            dialog->withdraw();
        showNextAuthentication();
        return;
    }
    if (cancelledAuthentication.value(id) == selected["attempt"].toString())
        return; // A queued full snapshot must not reopen a just-cancelled dialog.
    cancelledAuthentication.remove(id);
    auto dialog = authenticationDialogs.value(id);
    if (!dialog) {
        dialog = new SshPromptDialog(this);
        authenticationDialogs[id] = dialog;
        authenticationQueue.append(id);
        connect(dialog, &SshPromptDialog::answered, this, [this, id](const QJsonObject &reply) {
            if (reply["cancel"].toBool()) {
                cancelledAuthentication[id] = reply["attempt"].toString();
                authenticationQueue.removeAll(id);
                if (auto cancelled = authenticationDialogs.take(id))
                    cancelled->deleteLater();
                QTimer::singleShot(0, this, &Window::showNextAuthentication);
            }
            if (auto *discovery = hostDiscovery.value(id))
                discovery->answerAuthentication(reply);
        });
    }
    dialog->setPrompt(hosts.value(id).label(), selected);
    showNextAuthentication();
}
void Window::showNextAuthentication() {
    while (!authenticationQueue.isEmpty()) {
        const auto dialog = authenticationDialogs.value(authenticationQueue.first());
        if (!dialog) {
            authenticationQueue.removeFirst();
            continue;
        }
        if (!dialog->isVisible())
            dialog->show();
        return;
    }
}
void Window::saveHosts() {
    QJsonArray data, compatible;
    for (const auto &host : hosts) {
        data.append(host.json());
        auto legacy = host.json();
        // Older clients must not silently connect with an unsupported user/port.
        if (!host.sshOptions().isEmpty() || !host.generation.isEmpty())
            legacy["enabled"] = false;
        compatible.append(legacy);
    }
    QSettings settings(dataDirectory + "/window.ini", QSettings::IniFormat);
    settings.setValue("hostsV2", QJsonDocument(data).toJson(QJsonDocument::Compact));
    settings.setValue("hosts", QJsonDocument(compatible).toJson(QJsonDocument::Compact));
}
void Window::refreshHost(const QString &id, const QString &revision, bool interactive) {
    if (!hosts.contains(id) || !hosts[id].enabled)
        return;
    const auto &host = hosts[id];
    if ((!host.servers || host.isLocal()) && !host.containers) {
        hostDiscovery[id]->stop();
        hostProblems.remove(id);
        hostFeedback[id] = host.isLocal() ? "Local Codex discovery active" : "Discovery disabled";
        refreshScopeFinished(id);
    } else {
        hostFeedback[id] = "Discovering…";
        hostDiscovery[id]->refresh(revision, host.servers, host.containers, host.sshOptions(),
                                   interactive);
    }
    updateHostSummary();
}
void Window::setHostEnabled(const QString &id, bool enabled) {
    if (!hosts.contains(id) || (hosts[id].isLocal() && !enabled))
        return;
    hosts[id].enabled = enabled;
    if (enabled)
        refreshHost(id, {}, true);
    else {
        updateAuthentication(id, {});
        hostDiscovery[id]->stop();
        refreshScopeFinished(id, {"Host disabled during refresh."});
        hostFeedback[id] = "Disabled";
        hostProblems.remove(id);
        for (const auto &p : profiles)
            if (hostId(p) == id)
                backends[p.id]->disconnectServer();
    }
    saveHosts();
    updateHostSummary();
}
void Window::removeHost(const QString &id) {
    if (!hosts.contains(id) || hosts[id].isLocal())
        return;
    updateAuthentication(id, {});
    cancelledAuthentication.remove(id);
    delete hostDiscovery.take(id);
    refreshScopeFinished(id, {"Host removed during refresh."});
    hosts.remove(id);
    hostFeedback.remove(id);
    hostProblems.remove(id);
    for (const auto &p : profiles)
        if (p.discoveryHost == id) {
            terminal->closeBackend(p.id);
            backends[p.id]->disconnectServer();
        }
    saveHosts();
    updateHostSummary();
}
void Window::updateHostSummary() {
    QStringList details;
    for (const auto &host : hosts)
        details << host.label() + ": " + hostFeedback.value(host.id);
    hostStatus->setToolTip(details.join('\n'));
    hostStatus->setText(hostProblems.isEmpty() ? "Hosts…" : "Host connection issues…");
    hostStatus->setIcon(
        QIcon::fromTheme(hostProblems.isEmpty() ? "network-server" : "dialog-warning"));
    emit connectionSummaryChanged();
}
void Window::hostsDialog() {
    if (hostsWindow) {
        hostsWindow->show();
        hostsWindow->raise();
        hostsWindow->activateWindow();
        return;
    }
    auto *dialog = new QDialog(this);
    hostsWindow = dialog;
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle("Hosts");
    auto *layout = new QVBoxLayout(dialog);
    auto *description = new QLabel(
        "Discover Codex servers and myCodex containers on each host. SSH hosts use your "
        "OpenSSH configuration. Select Connect to enter passwords or passphrases; they are never "
        "saved. Python 3 is required; "
        "Docker is needed only for myCodex. This Computer is always available. "
        "Disabling a remote host retains terminal output; removing it closes its views.");
    description->setWordWrap(true);
    layout->addWidget(description);
    auto *list = new QListWidget;
    list->setObjectName("hosts");
    layout->addWidget(list);
    auto *servers = new QCheckBox("Discover Codex servers");
    auto *containers = new QCheckBox("Discover myCodex containers");
    servers->setObjectName("discoverHostServers");
    containers->setObjectName("discoverHostContainers");
    layout->addWidget(servers);
    layout->addWidget(containers);
    auto currentId = [list] {
        return list->currentItem() ? list->currentItem()->data(Qt::UserRole).toString() : QString();
    };
    auto *row = new QHBoxLayout;
    layout->addLayout(row);
    auto button = [row, dialog](const QString &text, auto action) {
        auto *b = new QPushButton(text);
        row->addWidget(b);
        connect(b, &QPushButton::clicked, dialog, action);
        return b;
    };
    button("Add SSH host…", [this, dialog] {
        if (hosts.size() >= 64) {
            error("At most 64 hosts can be configured.");
            return;
        }
        const auto added =
            editSshHost(dialog, {QUuid::createUuid().toString(QUuid::WithoutBraces), {}}, hosts);
        if (!added)
            return;
        addHost(*added, true);
        saveHosts();
    });
    auto *edit = button("Edit…", [this, dialog, currentId] {
        const auto id = currentId();
        if (!hosts.contains(id) || hosts[id].isLocal())
            return;
        const auto edited = editSshHost(dialog, hosts[id], hosts);
        if (edited)
            updateHost(*edited);
    });
    edit->setObjectName("editHost");
    auto *retry = button("Connect / retry", [this, currentId] {
        const auto id = currentId();
        setHostEnabled(id, true);
        if (hosts.contains(id) && hosts[id].isLocal())
            discovery->refresh();
    });
    auto *disable = button("Disable", [this, currentId] { setHostEnabled(currentId(), false); });
    disable->setObjectName("disableHost");
    auto *remove = button("Remove", [this, currentId] { removeHost(currentId()); });
    remove->setObjectName("removeHost");
    auto selected = [this, currentId, servers, containers, edit, disable, remove, retry] {
        const auto id = currentId();
        const bool valid = hosts.contains(id), local = valid && hosts[id].isLocal();
        QSignalBlocker a(servers), b(containers);
        servers->setChecked(valid && hosts[id].servers);
        containers->setChecked(valid && hosts[id].containers);
        servers->setEnabled(valid && !local);
        containers->setEnabled(valid);
        disable->setEnabled(valid && !local && hosts[id].enabled);
        remove->setEnabled(valid && !local);
        edit->setEnabled(valid && !local);
        retry->setEnabled(valid);
    };
    auto populate = [this, list, currentId, selected] {
        const auto current = currentId();
        QSignalBlocker blocker(list);
        list->clear();
        QStringList ids;
        for (const auto &host : hosts)
            if (host.isLocal())
                ids.prepend(host.id);
            else
                ids.append(host.id);
        for (const auto &id : ids) {
            const auto &host = hosts[id];
            const auto label =
                host.label() + " — " +
                (host.isLocal() ? QString("%1 host servers · ").arg(discoveryReport.endpoints)
                                : QString()) +
                hostFeedback.value(id);
            auto *item = new QListWidgetItem(label, list);
            item->setData(Qt::UserRole, id);
            item->setToolTip(label);
            if (id == current)
                list->setCurrentItem(item);
        }
        if (!list->currentItem())
            list->setCurrentRow(0);
        selected();
    };
    connect(list, &QListWidget::currentRowChanged, dialog, selected);
    auto options = [this, currentId, servers, containers] {
        const auto id = currentId();
        if (!hosts.contains(id))
            return;
        auto &host = hosts[id];
        host.servers = host.isLocal() || servers->isChecked();
        host.containers = containers->isChecked();
        for (const auto &p : profiles)
            if (hostId(p) == id && (p.isContainer() ? !host.containers : !host.servers))
                backends[p.id]->disconnectServer();
        refreshHost(id);
        saveHosts();
    };
    connect(servers, &QCheckBox::toggled, dialog, options);
    connect(containers, &QCheckBox::toggled, dialog, options);
    connect(this, &Window::connectionSummaryChanged, dialog, populate);
    populate();
    auto *close = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(close, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    layout->addWidget(close);
    dialog->resize(850, 440);
    dialog->show();
}
void Window::applyHostInventory(const QString &id, const QJsonObject &inventory) {
    if (!hosts.contains(id) || !hosts[id].enabled)
        return;
    updateAuthentication(id, inventory["prompts"].toArray());
    const auto state = inventory["state"].toString();
    if (!state.isEmpty() && state != "connected") {
        const auto message = inventory["message"].toString("Host connection unavailable");
        hostFeedback[id] = message;
        if (state == "connecting" || state == "authenticating")
            hostProblems.remove(id);
        else {
            hostProblems.insert(id);
            refreshScopeFinished(id, {message});
        }
        updateHostSummary();
        return;
    }
    for (const auto &field : {"host", "user"}) {
        const auto value = inventory[field].toString();
        if (!value.isEmpty() && validText(value, 255))
            (QString::fromLatin1(field) == "host" ? hostNames : hostUsers)[id] = value;
    }
    hostProblems.remove(id);
    if (hosts[id].containers)
        applyContainers(id, inventory);
    else
        hostFeedback[id] = "myCodex discovery disabled";
    QStringList issues;
    for (const auto &value : inventory["issues"].toArray())
        issues << value.toObject()["summary"].toString();
    if (!hosts[id].isLocal() && hosts[id].servers) {
        issues += applyRemoteServers(id, inventory["hostServers"].toObject());
        hostFeedback[id].prepend(
            QString("%1 host servers · ")
                .arg(inventory["hostServers"].toObject()["endpoints"].toArray().size()));
        if (!issues.isEmpty()) {
            hostProblems.insert(id);
            hostFeedback[id] += "\n" + issues.join('\n');
        }
    }
    if (!issues.isEmpty())
        hostProblems.insert(id);
    refreshNames();
    if (inventory["revision"].toString() == refreshRevision && inventory["settled"].toBool()) {
        if (hostProblems.contains(id))
            issues << hostFeedback[id];
        refreshScopeFinished(id, issues);
    }
    updateHostSummary();
}
QStringList Window::applyRemoteServers(const QString &host, const QJsonObject &inventory) {
    if (!hosts.contains(host) || !hosts[host].enabled || !hosts[host].servers ||
        hosts[host].isLocal())
        return {};
    QStringList issues;
    for (const auto &value : inventory["issues"].toArray())
        issues << value.toObject()["summary"].toString();
    bool valid = inventory["endpoints"].isArray() && inventory["issues"].isArray();
    const auto endpoints = inventory["endpoints"].toArray();
    if (endpoints.size() > 4096)
        return {"Host server inventory exceeds its limit."};
    QSet<QString> present, paths;
    const auto digest = [](const QString &s) {
        return QString::fromLatin1(
            QCryptographicHash::hash(s.toUtf8(), QCryptographicHash::Sha256).toHex());
    };
    for (const auto &value : endpoints) {
        const auto ep = value.toObject();
        const auto path = ep["path"].toString();
        if (paths.contains(path)) {
            valid = false;
            issues << "Duplicate host endpoint in inventory.";
            continue;
        }
        paths.insert(path);
        BackendProfile p;
        const auto options = hosts[host].sshOptions();
        const auto scope = hosts[host].discoveryScope();
        p.id = "ssh-" + digest(scope + QChar(0x1f) + path);
        if (ignoredSockets.contains(p.id))
            continue;
        p.kind = "ssh-unix";
        p.sshHost = hosts[host].ssh;
        p.discoveryHost = host;
        p.socketPath = path;
        p.relaySocket = ep["localSocket"].toString();
        p.executable = ep["executable"].toString("codex");
        p.discovered = true;
        for (const auto &field : {"user", "host", "ownerHome", "cwd", "pid", "default"})
            if (ep.contains(field))
                p.identity[field] = ep[field];
        if (!options.isEmpty())
            p.identity["sshOptions"] = options;
        if (!hosts[host].generation.isEmpty())
            p.identity["hostGeneration"] = hosts[host].generation;
        p.environmentId = digest(scope + QChar(0x1f) + ep["user"].toString() + QChar(0x1f) +
                                 ep["ownerHome"].toString() + QChar(0x1f) + ep["host"].toString() +
                                 (ep["hostEnvironment"].toBool() ? QString() : p.id));
        p.name = serverLabel(p);
        if (!p.validate().isEmpty() || !QDir::isAbsolutePath(p.relaySocket) ||
            !validText(p.relaySocket)) {
            valid = false;
            issues << "Invalid host endpoint in inventory.";
            continue;
        }
        const auto old = profiles.value(p.id);
        const bool changed = !backends.contains(p.id) || old.relaySocket != p.relaySocket ||
                             old.environmentId != p.environmentId;
        if (profiles.contains(p.id)) {
            p.autoConnect = old.autoConnect;
            if (old.environmentId == p.environmentId) {
                for (const auto &field : {"codexHome", "userAgent"})
                    if (old.identity.contains(field))
                        p.identity[field] = old.identity[field];
            }
        }
        cancelRetirement(p.id);
        store->allowBackend(p.id);
        if (!refreshRevision.isEmpty()) {
            if (!profiles.contains(p.id))
                refreshAdded.insert(p.id);
            else if (old.json() != p.json() && !refreshAdded.contains(p.id))
                refreshUpdated.insert(p.id);
        }
        if (changed && backends.contains(p.id)) {
            auto *b = backends.take(p.id);
            b->disconnect(this);
            delete b;
        }
        profiles[p.id] = p;
        store->save(p);
        opening->updateProfile(p);
        model->backendEnvironment(p.id, p.environmentId);
        results->backendEnvironment(p.id, p.environmentId);
        if (changed)
            attach(p, p.autoConnect && !paused.contains(p.id));
        else if (!backends[p.id]->ready() && p.autoConnect && !paused.contains(p.id) &&
                 !discoveredPresent.contains(p.id))
            backends[p.id]->connectServer();
        present.insert(p.id);
        discoveredPresent.insert(p.id);
    }
    if (valid && inventory["complete"].toBool() && issues.isEmpty()) {
        const bool refreshing = !refreshRevision.isEmpty() && refreshScopes.contains(host) &&
                                inventory["revision"].toString() == refreshRevision;
        for (const auto &p : std::as_const(profiles))
            if (p.kind == "ssh-unix" && p.discoveryHost == host && !present.contains(p.id)) {
                discoveredPresent.remove(p.id);
                if (refreshing)
                    retireConnection(p.id);
                else if (auto *b = backends.value(p.id))
                    b->disconnectServer();
            }
    }
    refreshNames();
    if (!valid)
        issues << "Host server discovery has not supplied a valid inventory.";
    return issues;
}
void Window::applyContainers(const QString &hostId, const QJsonObject &inventory) {
    if (!hosts.contains(hostId) || !hosts[hostId].enabled || !hosts[hostId].containers)
        return;
    const auto host = inventory["host"].toString();
    if (!host.isEmpty() && validText(host, 255)) {
        hostNames[hostId] = host;
        if (hosts[hostId].isLocal()) {
            localHost = host;
            localIdentityDiscovered = true;
            const auto user = inventory["user"].toString();
            if (!user.isEmpty() && validText(user, 128))
                localUser = user;
        }
    }
    QSet<QString> present, empty;
    QHash<QString, QJsonObject> checkedContainers;
    bool validInventory = inventory["containers"].isArray();
    QStringList issues;
    for (const auto &issue : inventory["issues"].toArray())
        issues << issue.toObject()["summary"].toString();
    const auto containers = inventory["containers"].toArray();
    if (containers.size() > 512) {
        hostFeedback[hostId] = "Container inventory exceeds the 512-container limit.";
        hostProblems.insert(hostId);
        refreshScopeFinished(hostId, {hostFeedback[hostId]});
        updateHostSummary();
        return;
    }
    for (const auto &value : containers) {
        const auto c = value.toObject();
        const auto cid = c["containerId"].toString(), engine = c["engine"].toString();
        if (!QRegularExpression("^[a-f0-9]{64}$").match(cid).hasMatch() || engine.isEmpty() ||
            !validText(engine, 256)) {
            validInventory = false;
            issues << "Invalid container identity in inventory.";
            continue;
        }
        const auto options = hosts[hostId].sshOptions();
        const auto scope = hosts[hostId].discoveryScope();
        const auto key = scope + QChar(0x1f) + engine + QChar(0x1f) + cid;
        const auto digest = [](const QString &s) {
            return QString::fromLatin1(
                QCryptographicHash::hash(s.toUtf8(), QCryptographicHash::Sha256).toHex());
        };
        const auto rootId = "container:" + hostId + ":" + digest(key);
        auto endpoints = c["endpoints"].toArray();
        for (const auto &i : c["issues"].toArray())
            issues << c["containerName"].toString() + ": " + i.toObject()["summary"].toString();
        bool hasServer = false;
        bool validEndpoints = c["endpoints"].isArray();
        QSet<QString> endpointPaths;
        if (endpoints.size() > 4096) {
            issues << "Container endpoint inventory exceeds its limit.";
            endpoints = {};
            validEndpoints = false;
        }
        for (const auto &entry : endpoints) {
            const auto ep = entry.toObject();
            if (endpointPaths.contains(ep["path"].toString())) {
                validEndpoints = false;
                issues << "Duplicate endpoint in container inventory.";
            }
            endpointPaths.insert(ep["path"].toString());
            BackendProfile p;
            p.id = "mc-" + digest(key + QChar(0x1f) + ep["path"].toString());
            if (ignoredSockets.contains(p.id))
                continue;
            p.kind = "mycodex";
            p.containerId = cid;
            p.containerEpoch = c["containerEpoch"].toString();
            p.dockerContext = c["dockerContext"].toString();
            p.sshHost = hosts[hostId].ssh;
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
                          {"hostUser", inventory["user"]},
                          {"pid", ep["pid"]},
                          {"cwd", ep["cwd"]}};
            if (!options.isEmpty())
                p.identity["sshOptions"] = options;
            if (!hosts[hostId].generation.isEmpty())
                p.identity["hostGeneration"] = hosts[hostId].generation;
            p.name = serverLabel(p);
            if (!p.validate().isEmpty()) {
                issues << p.name + ": " + p.validate();
                validEndpoints = false;
                continue;
            }
            cancelRetirement(p.id);
            store->allowBackend(p.id);
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
            if (!refreshRevision.isEmpty()) {
                if (!profiles.contains(p.id))
                    refreshAdded.insert(p.id);
                else if (old.json() != p.json() && !refreshAdded.contains(p.id))
                    refreshUpdated.insert(p.id);
            }
            profiles[p.id] = p;
            opening->updateProfile(p);
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
        auto checked = c;
        checked["endpointComplete"] = validEndpoints && c["endpointComplete"].toBool() &&
                                      c["issues"].isArray() && c["issues"].toArray().isEmpty() &&
                                      !c["paused"].toBool();
        const auto scopeKey = engine + QChar(0x1f) + cid;
        if (checkedContainers.contains(scopeKey)) {
            validInventory = false;
            issues << "Duplicate container identity in inventory.";
        }
        checkedContainers[scopeKey] = checked;
        if (!hasServer) {
            empty.insert(rootId);
            BackendProfile container;
            container.kind = "mycodex";
            container.sshHost = hosts[hostId].ssh;
            container.discoveryHost = hostId;
            model->backendGroup(rootId, groupKey(container), groupLabel(container));
            model->backendName(rootId, "myCodex · " + c["containerName"].toString());
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
    const bool refreshing = !refreshRevision.isEmpty() && refreshScopes.contains(hostId) &&
                            inventory["revision"].toString() == refreshRevision;
    const auto engines = inventory["engines"].toObject();
    if (validInventory) {
        for (const auto &p : std::as_const(profiles)) {
            if (!p.isContainer() || p.discoveryHost != hostId || present.contains(p.id))
                continue;
            const auto engine = p.identity["dockerEngine"].toString();
            const auto scan = engines[engine].toObject();
            if (scan["revision"] != inventory["revision"] || !scan["containerIds"].isArray())
                continue;
            const auto container = checkedContainers.value(engine + QChar(0x1f) + p.containerId);
            const bool gone = !scan["containerIds"].toArray().contains(p.containerId);
            const bool checked = container["endpointComplete"].toBool() &&
                                 container["endpointRevision"] == inventory["revision"];
            if (gone || checked) {
                discoveredPresent.remove(p.id);
                if (refreshing)
                    retireConnection(p.id);
                else if (auto *backend = backends.value(p.id))
                    backend->disconnectServer();
            }
        }
    }
    refreshNames();
    hostFeedback[hostId] =
        QString("%1 containers, %2 servers").arg(containers.size()).arg(present.size()) +
        (issues.isEmpty() ? QString() : "\n" + issues.join('\n'));
    if (issues.isEmpty())
        hostProblems.remove(hostId);
    else
        hostProblems.insert(hostId);
    updateHostSummary();
}
void Window::refreshNames() {
    for (const auto &p : profiles) {
        if (p.isLocal() && !localIdentityDiscovered) {
            if (!p.identity["user"].toString().isEmpty())
                localUser = p.identity["user"].toString();
            if (!p.identity["host"].toString().isEmpty())
                localHost = p.identity["host"].toString();
        } else if (p.kind == "ssh-unix") {
            if (!hostNames.contains(p.discoveryHost))
                hostNames[p.discoveryHost] = p.identity["host"].toString();
            if (!hostUsers.contains(p.discoveryHost))
                hostUsers[p.discoveryHost] = p.identity["user"].toString();
        } else if (p.isContainer() && !p.identity["dockerHost"].toString().isEmpty()) {
            if (!hostNames.contains(p.discoveryHost))
                hostNames[p.discoveryHost] = p.identity["dockerHost"].toString();
            if (!hostUsers.contains(p.discoveryHost) && p.identity["hostUser"].isString())
                hostUsers[p.discoveryHost] = p.identity["hostUser"].toString();
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
QString Window::hostId(const BackendProfile &p) const {
    if (!p.discoveryHost.isEmpty())
        return p.discoveryHost;
    if (p.isLocal() || p.sshHost.isEmpty())
        for (const auto &host : hosts)
            if (host.isLocal())
                return host.id;
    return {};
}
QString Window::groupKey(const BackendProfile &p) const {
    const auto generation = p.identity["hostGeneration"].toString();
    return p.sshHost.isEmpty() ? QString("local-host")
                               : "ssh-host:" + hostId(p) +
                                     (generation.isEmpty() ? QString() : QChar(0x1f) + generation);
}
QString Window::groupLabel(const BackendProfile &p) const {
    if (p.sshHost.isEmpty())
        return "This Computer · " + localUser + "@" + localHost;
    const auto id = hostId(p);
    if (hosts.contains(id) && p.identity["hostGeneration"].toString() != hosts[id].generation) {
        HostProfile previous{id, p.sshHost};
        const auto options = p.identity["sshOptions"].toObject();
        previous.user = options["user"].toString();
        previous.port = options["port"].toInt();
        return previous.label() + " · Previous connection";
    }
    const auto user = hostUsers.value(id);
    const auto name = hostNames.value(id, p.sshHost);
    const auto label = hosts.contains(id) ? hosts[id].label() : p.sshHost;
    return label + (name == p.sshHost && user.isEmpty()
                        ? QString()
                        : " · " + (user.isEmpty() ? QString() : user + "@") + name);
}
void Window::presentProfile(const BackendProfile &p, const QString &label) {
    const auto sourceIdentity = sessionSourceIdentity(p);
    for (auto it = readStates.begin(); it != readStates.end(); ++it) {
        if (it->backendId != p.id || !it->unread || it->sourceIdentity == sourceIdentity)
            continue;
        it->unread = false;
        store->save(*it);
        const auto key = it.key();
        if (sessions.contains(key)) {
            auto &session = sessions[key];
            applyReadState(session);
            model->upsert(session);
        }
        if (searchSessions.contains(key)) {
            auto &session = searchSessions[key];
            applyReadState(session);
            results->upsert(session);
        }
    }
    updateUnreadCount();
    for (auto *source : {model, results}) {
        source->backendGroup(p.id, groupKey(p), groupLabel(p));
        source->backendName(p.id, label.isEmpty() ? serverTreeLabel(p) : label);
        source->backendDetails(p.id, serverDetails(p));
    }
}
void Window::terminalFocused(const QString &key) {
    updateViewedSession(true);
    auto it = sessions.find(key);
    if (it == sessions.end()) {
        syncCurrentSession();
        return;
    }
    it->opened = QDateTime::currentSecsSinceEpoch();
    model->upsert(*it);
    if (searchSessions.contains(key)) {
        searchSessions[key] = *it;
        results->upsert(*it);
    }
    // A visit can move an entire host above the viewport. Reveal the selected
    // row after both models have sorted; background updates do not take this path.
    syncCurrentSession();
    if (!it->id.startsWith("new-"))
        persist([&] { store->save(*it); });
}
void Window::syncCurrentSession() {
    // With no displayed terminal, Projects remains available for browsing and
    // choosing a search scope. Closing the displayed view clears its selection.
    const auto key = terminal ? terminal->currentKey() : QString();
    updateServerExpansion();
    if (key.isEmpty())
        return;
    const auto *source = qobject_cast<SessionModel *>(filter->sourceModel());
    const auto index = source ? filter->mapFromSource(source->sessionIndex(key)) : QModelIndex{};
    if (!index.isValid()) {
        tree->selectionModel()->clear();
        return; // Keep the user's tab/query/scope; the terminal header identifies it.
    }
    tree->setCurrentIndex(index);
    for (auto parent = index.parent(); parent.isValid(); parent = parent.parent())
        tree->expand(parent);
    tree->scrollTo(index);
}
void Window::clientStateChanged(const Session &client) {
    const auto key = sessionKey(client.backendId, client.id);
    auto s = sessions.value(key, client);
    s.terminalOpen = client.terminalOpen;
    s.clientIssue = client.clientIssue;
    s.clientPending = client.clientPending;
    applyReadState(s);
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
    cancelRetirement(client.backendId);
    const auto view = profiles.contains(client.backendId)
                          ? terminal->prepare(profiles[client.backendId], client, false)
                          : terminal->view(sessionKey(client.backendId, client.id));
    opening->request(view, false);
}
void Window::openSession(const QString &backend, const Session &session) {
    if (!profiles.contains(backend))
        return;
    cancelRetirement(backend);
    const auto view = terminal->prepare(profiles[backend], session, true);
    if (view.id.isEmpty()) {
        statusBar()->showMessage("Close a terminal before opening more than 512 views.");
        return;
    }
    opening->request(view, false);
}
void Window::updateSummary() {
    int count = 0;
    QStringList descriptions;
    for (auto it = profiles.begin(); it != profiles.end(); ++it) {
        count += connectionReady.value(it.key());
        descriptions << it->name + ": " + connectionStates.value(it.key()).description();
    }
    connections->setText(
        profiles.isEmpty() ? "No connections"
        : count == profiles.size()
            ? "Connections ready"
            : QString("%1 of %2 connections ready").arg(count).arg(profiles.size()));
    connections->setToolTip(descriptions.join('\n'));
    updateSearchControls();
    const bool failed = !discoveryReport.failure.isEmpty();
    const bool limited =
        !discoveryReport.issues.isEmpty() || !discoveryReport.refreshIssues.isEmpty();
    const bool empty = discoveryReport.checked.isValid() && !discoveryReport.endpoints;
    discoveryStatus->setVisible(discoveryReport.inProgress || failed || limited || empty ||
                                !discoveryReport.refreshSummary.isEmpty());
    discoveryStatus->setText(!discoveryReport.refreshSummary.isEmpty()
                                 ? (!refreshScopes.isEmpty() || !retirements.isEmpty()
                                        ? QString("Refreshing…")
                                        : QString("Refreshed · %1 removed · %2 unchecked")
                                              .arg(refreshRemoved)
                                              .arg(discoveryReport.refreshIssues.size()))
                             : discoveryReport.inProgress ? "Discovering…"
                             : failed                     ? "Discovery failed"
                             : limited                    ? "Discovery limited"
                                                          : "No local servers");
    discoveryStatus->setIcon(style()->standardIcon(discoveryReport.inProgress
                                                       ? QStyle::SP_BrowserReload
                                                   : failed  ? QStyle::SP_MessageBoxCritical
                                                   : limited ? QStyle::SP_MessageBoxWarning
                                                             : QStyle::SP_MessageBoxInformation));
    discoveryStatus->setToolTip(discoveryReport.refreshSummary.isEmpty()
                                    ? "View discovery details and available actions"
                                    : discoveryReport.refreshSummary + "\nView refresh details");
    discoveryStatus->setAccessibleName(discoveryStatus->text() + " — view details");
    if (discoveryDialog)
        discoveryDialog->setReport(discoveryReport);
    emit connectionSummaryChanged();
}
Window::CommandTarget Window::commandTarget() const {
    if (paletteTarget)
        return *paletteTarget;
    if (projectsContext) {
        const auto index = tree->currentIndex();
        return {index.data(BackendRole).toString(), index.data(ThreadRole).toString(),
                index.data(PathRole).toString(), index.data(KeyRole).toString(),
                static_cast<NodeKind>(index.data(KindRole).toInt())};
    }
    const auto session = terminal->currentSession();
    return {session.backendId, session.id, session.cwd, terminal->currentKey(), NodeKind::Session};
}
void Window::updateCommandAvailability() {
    const auto target = commandTarget();
    const auto session = sessions.value(target.key, searchSessions.value(target.key));
    const bool ready = backends.value(target.backend) && backends[target.backend]->ready();
    const auto enable = [this](const QString &id, bool enabled) {
        if (auto *action = commands->collection()->action(id))
            action->setEnabled(enabled);
    };
    enable("new_session", ready && QDir::isAbsolutePath(target.path));
    enable("fork_session", ready && !target.thread.isEmpty() && !target.thread.startsWith("new-") &&
                               !session.unavailable && !pendingForks.contains(target.key) &&
                               (!session.contextRequired || session.executionObserved));
    enable("restart_session", ready && session.terminalOpen && !session.clientIssue.isEmpty());
    enable("expand_branch",
           projectsContext && target.kind != NodeKind::Session && !target.key.isEmpty());
    enable("close_terminal", !terminal->currentKey().isEmpty());
    enable("terminal_find", !terminal->currentKey().isEmpty());
    enable("terminal_print", !terminal->currentKey().isEmpty());
}
void Window::contextualCommand(const QString &name) {
    const auto target = commandTarget();
    if (name == "new")
        newSession(target.backend, target.path);
    else if (name == "fork")
        forkSession(target.backend, target.thread);
    else if (name == "restart")
        restartTerminal(sessions.value(target.key));
    else if (name == "expand")
        walkTree(filter, {}, [&](const QModelIndex &index) {
            if (index.data(KeyRole).toString() == target.key) {
                tree->expand(index);
                walkTree(filter, index, [this](const QModelIndex &child) { tree->expand(child); });
            }
        });
}
void Window::showCommandPalette() {
    const auto target = commandTarget();
    updateCommandAvailability();
    auto *bar = new CommandPalette(this);
    bar->setObjectName("commandPalette");
    QMap<QString, QList<QAction *>> groups;
    for (auto *action : commands->collection()->actions()) {
        auto *proxy = new QAction(action->icon(), action->text(), bar);
        proxy->setEnabled(action->isEnabled());
        proxy->setCheckable(action->isCheckable());
        proxy->setChecked(action->isChecked());
        proxy->setShortcuts(action->shortcuts());
        connect(proxy, &QAction::triggered, this, [this, action, target] {
            {
                const QScopedValueRollback captured(paletteTarget,
                                                    std::optional<CommandTarget>(target));
                updateCommandAvailability();
                action->trigger();
            }
            updateCommandAvailability();
        });
        groups[action->property("category").toString()] << proxy;
    }
    QList<KCommandBar::ActionGroup> entries;
    for (auto it = groups.cbegin(); it != groups.cend(); ++it)
        entries << KCommandBar::ActionGroup{it.key(), it.value()};
    bar->setActions(entries);
    bar->show();
}
void Window::showSettings() {
    auto *hideUnsaved = findChild<QAction *>("hideUnsavedSessions");
    QSettings settings(dataDirectory + "/window.ini", QSettings::IniFormat);
    SettingsDialog dialog(hideUnsaved->isChecked(),
                          settings.value("backends/myCodex/showAllSessions", false).toBool(),
                          configuredLocalCodex, this, commands,
                          settings.value("application/restoreOpenedTerminals", true).toBool(),
                          settings.value("notifications/enabled", true).toBool(),
                          settings.value("updates/checkOnStartup", true).toBool());
    connect(&dialog, &SettingsDialog::settingsApplied, this,
            [this, hideUnsaved](bool hide, bool showAll, const QString &localExecutable,
                                bool restore, bool notify, bool checkUpdates) {
                hideUnsaved->setChecked(hide);
                QSettings settings(dataDirectory + "/window.ini", QSettings::IniFormat);
                settings.setValue("backends/myCodex/showAllSessions", showAll);
                settings.setValue("application/localCodexExecutable", localExecutable);
                settings.setValue("application/restoreOpenedTerminals", restore);
                settings.setValue("notifications/enabled", notify);
                settings.setValue("updates/checkOnStartup", checkUpdates);
                notifications->setEnabled(notify);
                notifications->setMyCodexWorkdirOnly(!showAll);
                settings.sync();
                configuredLocalCodex = localExecutable;
                terminal->setLocalCodexExecutable(
                    configuredLocalCodex.isEmpty() ? discoveredLocalCodex : configuredLocalCodex);
                model->setMyCodexWorkdirOnly(!showAll);
                results->setMyCodexWorkdirOnly(!showAll);
                updateUnreadCount();
                if (globalSearchActive)
                    updateSearchSummary();
            });
    dialog.exec();
}
void Window::showUpdates(bool check) {
    if (!updateDialog) {
        updateDialog = new UpdateDialog(updater, ARCHCODEX_VERSION, this);
        connect(updateDialog, &UpdateDialog::skipVersion, this, [this](const QString &version) {
            QSettings(dataDirectory + "/window.ini", QSettings::IniFormat)
                .setValue("updates/skippedVersion", version);
            updateUpdateStatus();
        });
    }
    updateDialog->show();
    updateDialog->raise();
    updateDialog->activateWindow();
    if (check && !updater->busy())
        updater->check();
}
void Window::updateUpdateStatus() {
    using State = Updater::State;
    const auto state = updater->state();
    const auto version = updater->release()["version"].toString();
    const auto skipped = QSettings(dataDirectory + "/window.ini", QSettings::IniFormat)
                             .value("updates/skippedVersion")
                             .toString();
    updateStatus->setVisible((state == State::Available && version != skipped) ||
                             state == State::Installed || state == State::Downloading ||
                             state == State::Installing || state == State::Error);
    updateStatus->setText(state == State::Installed   ? "Update installed — restart…"
                          : state == State::Available ? "Update available: " + version + "…"
                          : state == State::Error     ? "Update check / install failed…"
                                                      : "Updating…");
    updateStatus->setToolTip(state == State::Error ? updater->error() : "Archcodex updates");
}
void Window::showDiscoveryDetails() {
    if (!discoveryDialog) {
        discoveryDialog = new DiscoveryDialog(this);
        connect(discoveryDialog, &DiscoveryDialog::retryRequested, this, [this] { discover(); });
        connect(discoveryDialog, &DiscoveryDialog::addConnectionRequested, this,
                [this] { editConnection(); });
    }
    discoveryDialog->setReport(discoveryReport);
    discoveryDialog->show();
    discoveryDialog->raise();
    discoveryDialog->activateWindow();
}
void Window::editConnection(const QString &id) {
    if (auto *existing = connectionEditors.value(id).data()) {
        existing->show();
        existing->raise();
        existing->activateWindow();
        return;
    }
    if (!id.isEmpty() && (!profiles.contains(id) || !profiles[id].isLocal()))
        return;
    const auto original = profiles.value(id);
    const QPointer<Backend> originalBackend = backends.value(id);
    auto *dialog =
        new QDialog(connectionsWindow ? static_cast<QWidget *>(connectionsWindow.data()) : this);
    connectionEditors[id] = dialog;
    dialog->setObjectName("connectionEditor");
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowModality(Qt::NonModal);
    dialog->setWindowTitle(id.isEmpty() ? "Add local connection" : "Edit connection");
    auto *form = new QFormLayout(dialog);
    auto *name = new QLineEdit(id.isEmpty() ? "Local Codex" : original.name);
    name->setObjectName("connectionName");
    auto *socket = new QLineEdit(original.socketPath);
    socket->setObjectName("connectionSocket");
    socket->setPlaceholderText("Empty: host Codex default daemon");
    auto *executable = new QLineEdit(original.executable);
    executable->setObjectName("connectionExecutable");
    auto *automatic = new QCheckBox("Connect when Archcodex starts");
    automatic->setChecked(original.autoConnect);
    form->addRow("Name", name);
    form->addRow("Unix socket on host", socket);
    form->addRow("Host Codex executable", executable);
    form->addRow(automatic);
    auto *explanation =
        new QLabel("Connects to an existing app-server. It does not start or update Codex.");
    explanation->setWordWrap(true);
    form->addRow(explanation);
    auto *issue = new QLabel;
    issue->setObjectName("connectionEditorError");
    issue->setTextFormat(Qt::PlainText);
    issue->setWordWrap(true);
    form->addRow(issue);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    connect(
        buttons, &QDialogButtonBox::accepted, dialog,
        [this, dialog, id, original, originalBackend, name, socket, executable, automatic, issue] {
            if (!id.isEmpty() &&
                (!profiles.contains(id) || backends.value(id) != originalBackend ||
                 connectionDefinition(profiles[id]) != connectionDefinition(original))) {
                issue->setText("This connection changed or was removed. Close this editor and "
                               "select it again.");
                return;
            }
            auto p = id.isEmpty() ? BackendProfile{} : profiles[id];
            if (id.isEmpty())
                p.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            p.name = name->text().trimmed();
            p.socketPath = socket->text();
            p.executable = executable->text();
            p.autoConnect = automatic->isChecked();
            const auto validation = p.validate();
            if (!validation.isEmpty()) {
                issue->setText(validation);
                return;
            }
            // Editing a discovered endpoint deliberately makes it user-managed.
            p.discovered = false;
            cancelRetirement(p.id);
            store->allowBackend(p.id);
            if (!id.isEmpty() && profiles[id].socketPath != p.socketPath)
                p.environmentId.clear();
            if (!persist([&] { store->save(p); }))
                return;
            if (auto *old = backends.take(id)) {
                old->disconnectServer();
                old->deleteLater();
                terminal->closeBackend(id);
            }
            profiles[p.id] = p;
            model->backendEnvironment(p.id, p.environmentId);
            results->backendEnvironment(p.id, p.environmentId);
            refreshNames();
            attach(p, true);
            updateSummary();
            dialog->accept();
        });
    dialog->resize(600, 240);
    dialog->show();
}
void Window::removeConnection(const QString &id) {
    if (id.isEmpty() || !profiles.contains(id))
        return;
    if (auto *existing = connectionRemovals.value(id).data()) {
        existing->raise();
        existing->activateWindow();
        return;
    }
    const auto original = profiles[id];
    const QPointer<Backend> originalBackend = backends.value(id);
    auto *dialog = new QMessageBox(
        QMessageBox::Question, "Remove connection?",
        "Remove this connection and its local project/history cache? Codex "
        "conversations and running work are kept.",
        QMessageBox::Yes | QMessageBox::No,
        connectionsWindow ? static_cast<QWidget *>(connectionsWindow.data()) : this);
    connectionRemovals[id] = dialog;
    dialog->setObjectName("connectionRemoval");
    dialog->setInformativeText(original.name);
    dialog->setDefaultButton(QMessageBox::No);
    dialog->setTextFormat(Qt::PlainText);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowModality(Qt::NonModal);
    connect(dialog, &QDialog::finished, this, [this, id, original, originalBackend](int answer) {
        if (answer != QMessageBox::Yes)
            return;
        if (!profiles.contains(id) || backends.value(id) != originalBackend ||
            connectionDefinition(profiles[id]) != connectionDefinition(original)) {
            auto *notice = new QMessageBox(
                QMessageBox::Information, "Connection changed",
                "This connection changed or was removed. Select it again before removing it.",
                QMessageBox::Close, this);
            notice->setAttribute(Qt::WA_DeleteOnClose);
            notice->setWindowModality(Qt::NonModal);
            notice->show();
            return;
        }
        cancelRetirement(id);
        if (!persist([&] { store->removeBackend(id); }))
            return;
        ignoredSockets.insert(
            original.usesRelay() ? id : discoveredPaths.value(id, original.socketPath));
        notifications->withdrawBackend(id);
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
    });
    dialog->show();
}
void Window::updateConnections() {
    if (!connectionsWindow)
        return;
    QList<ConnectionRow> records;
    auto ids = profiles.keys();
    ids.sort();
    QHash<QString, int> counts, ordinals;
    for (const auto &id : ids) {
        const auto &p = profiles[id];
        const auto state = connectionStates.value(id);
        const auto location =
            p.sshHost.isEmpty() ? QString("This Computer")
            : hosts.contains(hostId(p)) &&
                    p.identity["hostGeneration"].toString() == hosts[hostId(p)].generation
                ? hosts[hostId(p)].label()
                : groupLabel(p);
        auto row = connectionRow(p, state, location);
        row.connectable = backends.contains(id) && !state.ready() &&
                          state.phase != ConnectionState::Phase::Connecting &&
                          (!p.usesRelay() || !p.relaySocket.isEmpty());
        row.disconnectable = backends.contains(id) && !paused.contains(id);
        ++counts[row.cells.mid(0, 4).join(QChar(0x1f))];
        records << row;
    }
    for (auto &row : records) {
        const auto key = row.cells.mid(0, 4).join(QChar(0x1f));
        const int ordinal = ++ordinals[key];
        if (profiles[row.id].discovered && counts[key] > 1)
            row.cells[1] += QString(" (%1)").arg(ordinal);
    }
    connectionsWindow->setConnections(records);
}
void Window::connectionsDialog() {
    if (!connectionsWindow) {
        auto *dialog = new ConnectionsDialog(dataDirectory + "/window.ini", this);
        connectionsWindow = dialog;
        connect(this, &Window::connectionSummaryChanged, dialog, [this] { updateConnections(); });
        connect(dialog, &ConnectionsDialog::addRequested, this, [this] { editConnection(); });
        connect(dialog, &ConnectionsDialog::editRequested, this, &Window::editConnection);
        connect(dialog, &ConnectionsDialog::removeRequested, this, &Window::removeConnection);
        connect(dialog, &ConnectionsDialog::connectRequested, this, [this](const QString &id) {
            if (!profiles.contains(id) || !backends.value(id))
                return;
            cancelRetirement(id);
            paused.remove(id);
            backends[id]->connectServer();
            updateConnections();
        });
        connect(dialog, &ConnectionsDialog::disconnectRequested, this, [this](const QString &id) {
            if (!profiles.contains(id) || !backends.value(id))
                return;
            paused.insert(id);
            backends[id]->disconnectServer();
            updateConnections();
        });
    }
    updateConnections();
    connectionsWindow->show();
    connectionsWindow->raise();
    connectionsWindow->activateWindow();
}
void Window::discover(bool reconcile, bool interactive) {
    if (!workspaceLoaded || !cachePending.isEmpty() || !refreshScopes.isEmpty() ||
        !retirements.isEmpty()) {
        refreshQueued |= reconcile;
        return;
    }
    refreshRevision = reconcile ? QUuid::createUuid().toString(QUuid::Id128) : QString();
    if (reconcile) {
        refreshAdded.clear();
        refreshUpdated.clear();
        refreshRemoved = 0;
        discoveryReport.refreshIssues = {};
        refreshScopes.insert("local");
        for (auto it = hostDiscovery.cbegin(); it != hostDiscovery.cend(); ++it)
            if (hosts.value(it.key()).enabled)
                refreshScopes.insert(it.key());
        refreshDeadline.start(60000);
    }
    QStringList paths;
    for (const auto &p : profiles)
        if (p.isLocal() && !p.socketPath.isEmpty())
            paths << p.socketPath;
    discoveryReport.inProgress = true;
    updateRefreshSummary();
    discovery->refresh(paths, refreshRevision);
    for (auto it = hostDiscovery.begin(); it != hostDiscovery.end(); ++it)
        if (hosts.value(it.key()).enabled) {
            hostFeedback[it.key()] = "Refreshing…";
            refreshHost(it.key(), refreshRevision, interactive);
        }
    updateHostSummary();
}
bool Window::protectsConnection(const QString &id) const {
    for (const auto &key : attentionKeys)
        if (key.startsWith(id + QChar(0x1f)))
            return true;
    for (const auto &state : readStates)
        if (state.backendId == id && state.unread)
            return true;
    if (!workspaceLoaded || !workspaceWritable || !cachePending.isEmpty() ||
        savedWorkspaceBackends.contains(id) || opening->referencesBackend(id) ||
        (!hostId(profiles.value(id)).isEmpty() &&
         (!hosts.contains(hostId(profiles.value(id))) ||
          !hosts.value(hostId(profiles.value(id))).enabled)) ||
        paused.contains(id) || !profiles.value(id).autoConnect)
        return true;
    for (const auto &view : terminal->workspace().views)
        if (view.backendId == id)
            return true;
    for (const auto &backend : pendingStarts)
        if (backend && backend == backends.value(id))
            return true;
    for (const auto &key : pendingForks)
        if (key.startsWith(id + QChar(0x1f)))
            return true;
    return false;
}
void Window::retireConnection(const QString &id) {
    if (!profiles.value(id).discovered || protectsConnection(id) || retirements.contains(id))
        return;
    const auto token = ++nextRetirement;
    retirements[id] = token;
    store->retireBackend(id, token);
}
void Window::cancelRetirement(const QString &id) {
    if (!retirements.remove(id))
        return;
    // The delete may already have committed, with its acknowledgement still queued.
    // Ordered writes restore the entire cached source before accepting new metadata.
    store->allowBackend(id);
    store->save(profiles[id]);
    for (const auto &session : std::as_const(sessions))
        if (session.backendId == id)
            store->save(session);
    for (const auto &state : std::as_const(readStates))
        if (state.backendId == id)
            store->save(state);
    QTimer::singleShot(0, this, &Window::updateRefreshSummary);
}
void Window::forgetConnection(const QString &id) {
    notifications->withdrawBackend(id);
    opening->removeSource(id);
    if (auto *backend = backends.take(id)) {
        backend->disconnect(this);
        backend->disconnectServer();
        backend->deleteLater();
    }
    profiles.remove(id);
    readStates.removeIf([&](const auto &entry) { return entry.value().backendId == id; });
    readThrough.removeIf(
        [&](const auto &entry) { return entry.key().startsWith(id + QChar(0x1f)); });
    attentionKeys.removeIf([&](const auto &key) { return key.startsWith(id + QChar(0x1f)); });
    updateUnreadCount();
    connectionStates.remove(id);
    connectionReady.remove(id);
    more.remove(id);
    paused.remove(id);
    discoveredPaths.remove(id);
    discoveredPresent.remove(id);
    searching.remove(id);
    searchErrors.remove(id);
    scopeBackends.remove(id);
    for (auto &state : explorer)
        state.selection.backends.remove(id);
    for (auto *items : {&sessions, &searchSessions})
        for (auto it = items->begin(); it != items->end();)
            if (it->backendId == id)
                it = items->erase(it);
            else
                ++it;
    model->removeBackend(id);
    results->removeBackend(id);
    refreshNames();
    updateSearchSummary();
}
void Window::refreshScopeFinished(const QString &id, const QStringList &issues) {
    if (!refreshScopes.remove(id))
        return;
    if (!issues.isEmpty())
        discoveryReport.refreshIssues.append(
            QJsonObject{{"code", "refresh-" + id},
                        {"summary", (id == "local"               ? QString("Local servers")
                                     : hosts.value(id).isLocal() ? QString("This Computer")
                                                                 : hosts.value(id).ssh) +
                                        ": refresh incomplete"},
                        {"explanation", "Connections in unverified scopes were retained."},
                        {"action", "Check discovery access and refresh again."},
                        {"details", QJsonArray::fromStringList(issues)}});
    updateRefreshSummary();
}
void Window::updateRefreshSummary() {
    if (refreshRevision.isEmpty()) {
        updateSummary();
        return;
    }
    const bool active = !refreshScopes.isEmpty() || !retirements.isEmpty();
    discoveryReport.refreshSummary =
        QString("%1 · %2 new · %3 updated · %4 removed · %5 incomplete")
            .arg(active ? "Refreshing" : "Refresh complete")
            .arg(refreshAdded.size())
            .arg(refreshUpdated.size())
            .arg(refreshRemoved)
            .arg(discoveryReport.refreshIssues.size());
    if (!active) {
        refreshDeadline.stop();
        refreshRevision.clear();
        if (refreshQueued) {
            refreshQueued = false;
            QTimer::singleShot(0, this, [this] { discover(); });
        }
    }
    updateSummary();
}
void Window::applyDiscovery(const QJsonArray &endpoints, const QJsonArray &reportedIssues,
                            const QString &revision) {
    auto issues = reportedIssues;
    if (endpoints.size() > 4096) {
        issues.append(QJsonObject{{"summary", "Local endpoint inventory exceeds its limit"}});
        applyDiscovery({}, issues, revision);
        return;
    }
    bool validEndpoints = endpoints.size() <= 4096;
    QSet<QString> paths;
    for (const auto &value : endpoints) {
        const auto path = value.toObject()["path"].toString();
        validEndpoints &= QDir::isAbsolutePath(path) && validText(path) && !paths.contains(path);
        paths.insert(path);
    }
    if (!validEndpoints)
        issues.append(
            QJsonObject{{"summary", "Invalid or excessive local endpoint inventory"},
                        {"action", "Refresh again; unverified connections were retained."}});
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
            if (p.isLocal() && (p.socketPath == path || aliases.contains(p.socketPath) ||
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
            if (!p.validate().isEmpty()) {
                issues.append(QJsonObject{{"summary", "Invalid local endpoint: " + p.validate()}});
                continue;
            }
            if (!persist([&] { store->save(p); }))
                continue;
            id = p.id;
            profiles[id] = p;
            if (!refreshRevision.isEmpty())
                refreshAdded.insert(id);
            model->backendName(id, p.name);
            results->backendName(id, p.name);
            attach(p, false);
        }
        cancelRetirement(id);
        auto &p = profiles[id];
        const auto previous = p.json();
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
        opening->updateProfile(p);
        model->backendEnvironment(id, environment);
        results->backendEnvironment(id, environment);
        for (const auto &s : sessions)
            if (s.backendId == id)
                model->upsert(s);
        for (const auto &s : searchSessions)
            if (s.backendId == id)
                results->upsert(s);
        persist([&] { store->save(p); });
        if (!refreshRevision.isEmpty() && previous != p.json() && !refreshAdded.contains(id))
            refreshUpdated.insert(id);
        discoveredPaths[id] = path;
        present.insert(id);
        if (!backends[id]->ready() && !paused.contains(id) && p.autoConnect &&
            !discoveredPresent.contains(id))
            backends[id]->connectServer();
    }
    refreshNames();
    if (issues.isEmpty()) {
        for (const auto &id : discoveredPresent - present)
            if (profiles.contains(id) && profiles[id].discovered && profiles[id].isLocal())
                backends[id]->disconnectServer();
        for (const auto &id : discoveredPresent)
            if (!profiles.value(id).isLocal())
                present.insert(id);
        discoveredPresent = present;
    } else
        discoveredPresent.unite(present);
    if (!refreshRevision.isEmpty() && revision == refreshRevision &&
        refreshScopes.contains("local")) {
        if (issues.isEmpty())
            for (const auto &p : std::as_const(profiles))
                if (p.discovered && p.isLocal() && !present.contains(p.id))
                    retireConnection(p.id);
        QStringList failures;
        for (const auto &value : issues)
            failures << value.toObject()["summary"].toString();
        refreshScopeFinished("local", failures);
    }
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
    activateSession(backend, id);
}
void Window::activateSession(const QString &backend, const QString &id, bool requireConnected) {
    const auto key = sessionKey(backend, id);
    if (terminal->focusSession(key)) {
        if (requireConnected)
            qCDebug(notificationLog) << "click reuses terminal" << backend << id;
        // A visit prioritizes a pending restore, but never restarts an exited client.
        const auto view = terminal->view(key);
        if (!terminal->hasClient(key) && view.disposition == ViewDisposition::Automatic)
            opening->request(view, false);
        return;
    }
    if (requireConnected && (!backends.contains(backend) || !backends[backend]->ready())) {
        statusBar()->showMessage(
            "This session is unavailable. Reconnect its server or refresh discovery.", 10000);
        return;
    }
    // Attention events can precede metadata. The IDs identify the target; cached
    // metadata only decorates its pending view. OpeningCoordinator verifies it live.
    auto session = sessions.value(key, searchSessions.value(key));
    if (requireConnected)
        qCDebug(notificationLog) << "click verifies new terminal" << backend << id;
    session.backendId = backend;
    session.id = id;
    openSession(backend, session);
}
bool Window::showingSearch() const {
    return globalSearchActive && tabs->currentIndex() == int(SessionView::All);
}
void Window::rememberNavigation(int tab) {
    auto &navigation = explorer[tab].navigation;
    // The tab widget already reports the destination during a switch.
    navigation.expanded = navigationExpansion(tab);
    navigation.selected = tree->currentIndex().data(KeyRole).toString();
    navigation.saved = true;
}
void Window::beginFiltering() {
    auto &state = explorer[tabs->currentIndex()];
    rememberNavigation(tabs->currentIndex());
    if (!state.filtering) {
        state.unfiltered = state.navigation;
        state.navigation.collapsed.clear();
        state.autoCollapsedServers.clear();
        state.filtering = true;
    }
}
void Window::endFiltering() {
    auto &state = explorer[tabs->currentIndex()];
    if (state.filtering) {
        state.navigation = state.unfiltered;
        state.autoCollapsedServers.clear();
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
    syncCurrentSession();
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
    searchFeedback->setVisible(showingSearch() || (!all && state.filtering));
    searchNotice->setVisible(showingSearch() || (!all && state.filtering));
    searchProgress->setVisible(showingSearch());
    searchDetailsButton->setVisible(showingSearch());
    if (!showingSearch())
        closeSearchDetails();
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
                      .arg(state.selection.scoped ? " · Scope: " + state.scopeLabel : QString()));
        searchNotice->setAccessibleName(searchNotice->fullText());
        searchNotice->setToolTip("<p>" + searchNotice->fullText().toHtmlEscaped() + "</p>");
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
    if (!explorer[0].filtering)
        beginFiltering();
    else {
        // Filtered expansion follows the explicit collapse set. Retain the
        // current selection without walking the previous result tree again.
        explorer[0].navigation.selected = tree->currentIndex().data(KeyRole).toString();
        explorer[0].navigation.saved = true;
    }
    closeSearchDetails();
    submittedQuery = query;
    ++searchToken;
    globalSearchActive = true;
    searchSessions.clear();
    {
        const QScopedValueRollback changing(changingExplorer, true);
        results->resetResults(); // This is a replacement, not a branch to restore.
    }
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
    closeSearchDetails();
    searchErrors.clear();
    searching.clear();
    searchSessions.clear();
    {
        const QScopedValueRollback changing(changingExplorer, true);
        results->resetResults();
    }
    // This can also be called when a connection is removed from another tab.
    auto &all = explorer[int(SessionView::All)];
    all.navigation = all.unfiltered;
    all.filtering = false;
    showExplorer();
}
void Window::updateSearchSummary() {
    if (!showingSearch())
        return;
    searchFeedback->setVisible(true);
    int visible = 0;
    for (int i = 0; i < results->rowCount(); ++i)
        visible += results->item(i)->data(CountRole).toInt();
    const auto hidden = searchSessions.size() - visible;
    searchNotice->setText(QString("%1 results%2")
                              .arg(visible)
                              .arg(hidden ? QString(" · %1 hidden").arg(hidden) : QString()));
    searchProgress->setText(
        (searching.isEmpty() ? QString("Search complete")
                             : QString("Searching %1 connection(s)…").arg(searching.size())) +
        (searchErrors.isEmpty() ? QString()
                                : QString(" · %1 incomplete").arg(searchErrors.size())));
    for (auto *label : {searchNotice, searchProgress}) {
        label->setAccessibleName(label->fullText());
        label->setToolTip("<p>" + label->fullText().toHtmlEscaped() + "</p>");
    }
    updateSearchDetails();
}
void Window::recordSearchError(const QString &backend, const QString &message) {
    auto &messages = searchErrors[backend];
    if (!messages.contains(message))
        messages << message;
}
void Window::closeSearchDetails() {
    if (searchDetails) {
        searchDetails->close();
        searchDetails.clear();
    }
}
void Window::showSearchDetails() {
    if (!showingSearch())
        return;
    if (!searchDetails) {
        searchDetails = new QDialog(this);
        searchDetails->setObjectName("searchDetails");
        searchDetails->setWindowTitle("Search details");
        searchDetails->setAttribute(Qt::WA_DeleteOnClose);
        searchDetails->resize(640, 400);
        auto *layout = new QVBoxLayout(searchDetails);
        auto *text = new QPlainTextEdit;
        text->setReadOnly(true);
        text->setAccessibleName("Search report");
        layout->addWidget(text, 1);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
        connect(buttons, &QDialogButtonBox::rejected, searchDetails, &QDialog::close);
        layout->addWidget(buttons);
    }
    updateSearchDetails();
    searchDetails->show(); // Non-modal; only an explicit user action opens it.
    searchDetails->raise();
    searchDetails->activateWindow();
}
void Window::updateSearchDetails() {
    if (!searchDetails)
        return;
    QString report = "Query: " + submittedQuery +
                     "\nScope: " + (scopedSearch ? explorer[0].scopeLabel : "All servers") +
                     "\n\n" + searchNotice->fullText() + "\n" + searchProgress->fullText();
    if (searchSessions.size())
        report += "\nHidden matches are excluded by the current visibility settings.";
    for (auto it = searchErrors.cbegin(); it != searchErrors.cend(); ++it)
        report += "\n\n" + profiles.value(it.key()).name + " [" + it.key() + "]\n" +
                  it.value().join('\n');
    auto *text = searchDetails->findChild<QPlainTextEdit *>();
    if (text->toPlainText() == report)
        return;
    const auto anchor = text->textCursor().anchor();
    const auto position = text->textCursor().position();
    const auto scroll = text->verticalScrollBar()->value();
    text->setPlainText(report);
    auto restored = text->textCursor();
    const auto last = text->document()->characterCount() - 1;
    restored.setPosition(qMin(anchor, last));
    restored.setPosition(qMin(position, last), QTextCursor::KeepAnchor);
    text->setTextCursor(restored);
    text->verticalScrollBar()->setValue(scroll);
}
bool Window::smokeTest(QString *error) {
    return terminal->smokeTest(error);
}
void Window::queueWorkspaceSave() {
    updateWorkspaceStatus();
    if (closing || !workspaceLoaded || !workspaceWritable || workspaceSaving)
        return;
    workspaceSaving = true;
    QTimer::singleShot(0, this, [this] {
        workspaceSaving = false;
        if (!closing)
            saveWorkspace();
    });
}
void Window::saveWorkspace() {
    if (!workspaceLoaded || !workspaceWritable)
        return;
    auto snapshot = terminal->workspace();
    snapshot.revision = ++workspaceRevision;
    store->save(snapshot);
}
void Window::freezeWorkspace() {
    if (closing)
        return;
    // Capture intent before transport teardown can report client exits.
    saveWorkspace();
    closing = true;
    opening->stop();
    terminal->freezeWorkspace();
}
void Window::updateWorkspaceStatus() {
    const auto pending = opening->pendingCount();
    workspaceStatus->setText(!workspaceError.isEmpty() ? "Workspace needs attention…"
                             : pending ? QString("%1 pending terminal(s)…").arg(pending)
                                       : "Terminal workspace…");
}
void Window::showWorkspaceDetails() {
    auto *dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle("Terminal workspace");
    auto *layout = new QVBoxLayout(dialog);
    auto *info = new QLabel(
        "Restoration uses the conversation opened by Archcodex. Switching with /resume, /fork, "
        "/new or agent navigation inside the TUI can leave that saved target unchanged. "
        "New-session launchers with no confirmed ID remain disconnected after restart. "
        "Open the desired saved conversation from Projects to remember it reliably.");
    info->setWordWrap(true);
    layout->addWidget(info);
    auto *list = new QListWidget;
    layout->addWidget(list);
    const auto refresh = [this, list] {
        list->clear();
        if (!workspaceError.isEmpty())
            list->addItem(
                "Workspace persistence is unavailable; its last saved data is preserved: " +
                workspaceError);
        for (const auto &view : terminal->workspace().views) {
            const auto client = sessions.value(view.key());
            auto *item = new QListWidgetItem(
                view.title + " — " +
                    (terminal->hasClient(view.key()) ? QString("Client open") : client.clientIssue),
                list);
            item->setToolTip(view.cwd + "\n" + view.threadId);
            item->setData(Qt::UserRole, view.key());
        }
    };
    connect(terminal, &TerminalArea::clientStateChanged, dialog, refresh);
    connect(terminal, &TerminalArea::workspaceChanged, dialog, refresh);
    connect(list, &QListWidget::itemActivated, dialog, [this](QListWidgetItem *item) {
        terminal->focusSession(item->data(Qt::UserRole).toString());
    });
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    auto *connections = buttons->addButton("Connections…", QDialogButtonBox::ActionRole);
    connect(connections, &QPushButton::clicked, this, &Window::connectionsDialog);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
    layout->addWidget(buttons);
    refresh();
    dialog->resize(680, 420);
    dialog->show();
}
void Window::applyReadState(Session &session) {
    const auto key = sessionKey(session.backendId, session.id);
    auto it = readStates.find(key);
    if (it != readStates.end() && it->unread &&
        it->sourceIdentity != sessionSourceIdentity(profiles.value(session.backendId))) {
        it->unread = false; // Never transfer an update to a replacement execution context.
        store->save(*it);
    }
    session.unread = it != readStates.end() && it->unread;
    const bool wasPending = attentionKeys.contains(key);
    if (session.needsAttention())
        attentionKeys.insert(key);
    else
        attentionKeys.remove(key);
    if (wasPending != session.needsAttention())
        updateUnreadCount();
}
void Window::setUnread(const QString &key, bool unread) {
    auto session = sessions.value(key, searchSessions.value(key));
    if (session.id.isEmpty() || session.id.startsWith("new-") ||
        !profiles.contains(session.backendId))
        return;
    if (!unread) {
        readThrough[key] = SessionAttention::latestSequence();
        if (key == viewedSession && tabs->currentIndex() == int(SessionView::Unread) &&
            model->sessionIndex(key).isValid())
            model->setReviewedSession(key);
    }
    const SessionReadState state{session.backendId, session.id,
                                 sessionSourceIdentity(profiles[session.backendId]), unread};
    const bool changed = !readStates.contains(key) || readStates[key].unread != unread ||
                         readStates[key].sourceIdentity != state.sourceIdentity;
    readStates[key] = state;
    if (!changed && session.unread == unread)
        return;
    session.unread = unread;
    applyReadState(session);
    sessions[key] = session;
    model->upsert(session);
    if (searchSessions.contains(key)) {
        searchSessions[key] = session;
        results->upsert(session);
    }
    if (unread)
        cancelRetirement(session.backendId);
    // Preserve metadata for a manually marked search-only result too.
    store->save(session);
    store->save(state);
}
void Window::receivedAttention(SessionAttention event) {
    const auto &session = event.session;
    const auto key = sessionKey(session.backendId, session.id);
    if (!profiles.contains(session.backendId) || !session.isSaved() ||
        !notifications->inScope(session, profiles[session.backendId]))
        return;
    event.observe(); // Test/alternate backends can deliver unstamped occurrences.
    if (event.sequence <= readThrough.value(key))
        return; // The user visited after detection but before delayed delivery.
    if (!sessions.contains(key))
        applySessions({session}); // Metadata can follow the occurrence in a later GUI batch.
    if (viewedSession == key) {
        setUnread(key, false);
        return;
    }
    setUnread(key, true);
}
void Window::updateUnreadCount() {
    if (unreadCountScheduled)
        return;
    unreadCountScheduled = true;
    QTimer::singleShot(0, this, [this] {
        unreadCountScheduled = false;
        int count = 0;
        // Only pending sessions, not a traversal of the entire history/tree.
        for (const auto &key : std::as_const(attentionKeys)) {
            const auto session = sessions.value(key, searchSessions.value(key));
            if (model->passesVisibilitySettings(session))
                ++count;
        }
        tabs->setTabText(int(SessionView::Unread),
                         count ? QString("Unread (%1)").arg(count) : QString("Unread"));
        commands->collection()->action("next_unread")->setEnabled(count > 0);
        tabs->setTabToolTip(int(SessionView::Unread),
                            viewDescriptions[int(SessionView::Unread)] +
                                " Counts include sessions hidden by this tab's search or scope.");
    });
}
void Window::updateViewedSession(bool visit) {
    if (!notifications || !terminal)
        return;
    auto session =
        isVisible() && !isMinimized() && isActiveWindow() ? terminal->currentSession() : Session{};
    notifications->setViewedSession(session.backendId, session.id);
    auto key = session.id.isEmpty() ? QString() : sessionKey(session.backendId, session.id);
    if (!key.isEmpty() &&
        (!terminal->hasClient(key) || session.clientPending || !session.clientIssue.isEmpty() ||
         terminal->view(key).sourceIdentity !=
             workspaceSourceIdentity(profiles.value(session.backendId)))) {
        session = {};
        key.clear();
    }
    const bool entered = key != viewedSession;
    viewedSession = key;
    if (!key.isEmpty() && (visit || entered)) {
        // Retain the just-read row until another terminal is selected. It no
        // longer contributes to the unread/unresolved count.
        if (tabs->currentIndex() == int(SessionView::Unread) && model->sessionIndex(key).isValid())
            model->setReviewedSession(key);
        setUnread(key, false);
    }
}
bool Window::event(QEvent *event) {
    const bool result = QMainWindow::event(event);
    if (event->type() == QEvent::ActivationChange || event->type() == QEvent::WindowStateChange ||
        event->type() == QEvent::Show || event->type() == QEvent::Hide)
        updateViewedSession();
    return result;
}
void Window::closeEvent(QCloseEvent *event) {
    if (updater && updater->finishing()) {
        statusBar()->showMessage("Wait for the update operation to finish before quitting.");
        event->ignore();
        return;
    }
    if (updater)
        updater->cancel();
    freezeWorkspace();
    QSettings settings(dataDirectory + "/window.ini", QSettings::IniFormat);
    settings.setValue("geometry", saveGeometry());
    settings.setValue("splitter", splitter->saveState());
    const auto &state = explorer[tabs->currentIndex()];
    settings.setValue("expanded", state.filtering ? state.unfiltered.expanded
                                                  : navigationExpansion(tabs->currentIndex()));
    for (auto *b : backends)
        b->disconnectServer();
    QMainWindow::closeEvent(event);
}
} // namespace Archcodex

#include "window.moc"
