// SPDX-License-Identifier: GPL-3.0-or-later
#include "window.h"
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
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QSplitter>
#include <QStatusBar>
#include <QTabBar>
#include <QTreeView>
#include <QUuid>
#include <QVBoxLayout>
#include <algorithm>

namespace Archcodex {
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
    auto *add = new QPushButton("+");
    add->setToolTip("Add project");
    add->setAccessibleName("Add project");
    add->setMaximumWidth(34);
    heading->addWidget(add);
    left->addLayout(heading);
    connect(add, &QPushButton::clicked, this, &Window::addProject);
    tabs = new QTabBar;
    tabs->addTab("All");
    tabs->addTab("Active");
    tabs->setExpanding(false);
    left->addWidget(tabs);
    searchBox = new QLineEdit;
    searchBox->setPlaceholderText("Search conversation text — Enter");
    searchBox->setAccessibleName("Search conversations");
    searchBox->setClearButtonEnabled(true);
    left->addWidget(searchBox);
    scope = new QComboBox;
    scope->addItems({"All projects", "Selected project"});
    scope->setAccessibleName("Search scope");
    left->addWidget(scope);
    connect(searchBox, &QLineEdit::returnPressed, this, &Window::search);
    connect(searchBox, &QLineEdit::textChanged, this, [this](const QString &text) {
        if (text.isEmpty())
            clearSearch();
    });
    notice = new QLabel("Add a connection to discover projects.");
    notice->setTextFormat(Qt::PlainText);
    notice->setWordWrap(true);
    left->addWidget(notice);
    tree = new QTreeView;
    tree->setModel(filter);
    tree->setHeaderHidden(true);
    tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tree->setUniformRowHeights(true);
    tree->setAnimated(false);
    tree->setAccessibleName("Projects and sessions");
    left->addWidget(tree, 1);
    connect(tree, &QTreeView::activated, this, &Window::activate);
    connect(tabs, &QTabBar::currentChanged, this, [this](int index) {
        QStringList saved;
        for (int i = 0; i < filter->rowCount(); ++i) {
            auto idx = filter->index(i, 0);
            if (tree->isExpanded(idx))
                saved << idx.data(KeyRole).toString();
        }
        expanded[previousTab] = saved;
        filter->setActiveOnly(index == 1);
        tree->collapseAll();
        for (int i = 0; i < filter->rowCount(); ++i) {
            auto idx = filter->index(i, 0);
            if (expanded.value(index).contains(idx.data(KeyRole).toString()))
                tree->expand(idx);
        }
        previousTab = index;
        if (!searchingView)
            notice->setText(index == 1
                                ? "Running or waiting for input. Hollow dots mean unknown state."
                                : "Projects and sessions, most recently used first.");
    });
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
    splitter->setSizes({310, 840});
    connect(close, &QPushButton::clicked, terminal, &TerminalArea::closeCurrent);
    connect(terminal, &TerminalArea::currentChanged, terminalTitle, &QLabel::setText);
    connections = new QLabel;
    connections->setTextFormat(Qt::PlainText);
    statusBar()->addWidget(connections, 1);
    auto *projectsMenu = menuBar()->addMenu("&Project");
    projectsMenu->addAction("Add project…", this, &Window::addProject);
    projectsMenu->addAction("New session in selected project", this, [this] {
        const auto index = tree->currentIndex();
        const auto id = index.data(BackendRole).toString();
        if (!index.isValid() || !profiles.contains(id)) {
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
    connectionMenu->addAction("Manage connections…", this, &Window::connectionsDialog);
    auto *view = menuBar()->addMenu("&View");
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
    }
    for (const auto &p : store->projects())
        model->project(p);
    for (const auto &s : store->sessions()) {
        sessions[sessionKey(s.backendId, s.id)] = s;
        model->upsert(s);
    }
    for (const auto &p : profiles)
        attach(p, connectOnStart && p.autoConnect);
    QSettings settings(directory + "/window.ini", QSettings::IniFormat);
    restoreGeometry(settings.value("geometry").toByteArray());
    splitter->restoreState(settings.value("splitter").toByteArray());
    const auto open = settings.value("expanded").toStringList();
    for (int i = 0; i < filter->rowCount(); ++i) {
        auto idx = filter->index(i, 0);
        if (open.contains(idx.data(KeyRole).toString()))
            tree->expand(idx);
    }
    updateSummary();
}
Window::~Window() {
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
    connect(b, &Backend::connectionChanged, this,
            [this, id = p.id](const QString &text, bool ready) {
                connectionStates[id] = text;
                connectionReady[id] = ready;
                if (!ready) {
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
        sessions[key] = s;
        model->upsert(s);
        persist([&] { store->save(s); });
        if (searchSessions.contains(key)) {
            searchSessions[key] = s;
            results->upsert(s);
        }
    });
    connect(b, &Backend::sessionRemoved, this, [this, id = p.id](const QString &thread) {
        sessions.remove(sessionKey(id, thread));
        model->remove(id, thread);
        searchSessions.remove(sessionKey(id, thread));
        results->remove(id, thread);
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
                        (s.backendId != scopeBackend || s.cwd != scopePath))
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
    if (start)
        QTimer::singleShot(0, b, &Backend::connectServer);
    else
        connectionStates[p.id] = "Disconnected";
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
    if (!searchingView && !profiles.isEmpty())
        notice->setText(descriptions.join('\n'));
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
        auto *b = backends.value(selected());
        if (b)
            b->connectServer();
        populate();
    });
    button("Disconnect", [&] {
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
void Window::addProject() {
    if (profiles.isEmpty()) {
        editConnection();
        if (profiles.isEmpty())
            return;
    }
    QDialog dialog(this);
    dialog.setWindowTitle("Add project");
    auto *form = new QFormLayout(&dialog);
    auto *backend = new QComboBox;
    for (auto it = profiles.begin(); it != profiles.end(); ++it)
        backend->addItem(it->name, it.key());
    auto *path = new QLineEdit;
    path->setPlaceholderText("Absolute path in the backend environment");
    auto *name = new QLineEdit;
    form->addRow("Connection", backend);
    form->addRow("Working directory / worktree", path);
    form->addRow("Display name (optional)", name);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    Project project;
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        if (!QDir::isAbsolutePath(path->text()) || !validText(path->text()) ||
            !validText(name->text(), 160)) {
            error("Enter a valid absolute backend path and display name.");
            return;
        }
        project = {backend->currentData().toString(), QDir::cleanPath(path->text()),
                   name->text().trimmed()};
        if (project.name.isEmpty())
            project.name = QFileInfo(project.path).fileName();
        if (project.name.isEmpty())
            project.name = project.path;
        dialog.accept();
    });
    dialog.resize(580, 160);
    if (dialog.exec() == QDialog::Accepted && persist([&] { store->save(project); }))
        model->project(project);
}
void Window::activate(const QModelIndex &index) {
    const auto id = index.data(ThreadRole).toString();
    if (id.isEmpty())
        return;
    const auto backend = index.data(BackendRole).toString();
    const auto key = sessionKey(backend, id);
    Session s = searchingView ? searchSessions.value(key) : sessions.value(key);
    if (!profiles.contains(backend))
        return;
    if (!backends.value(backend) || !backends[backend]->ready()) {
        error("Connect this backend before opening its terminal.");
        return;
    }
    QString issue;
    if (!terminal->open(profiles[backend], s, &issue)) {
        error(issue);
        return;
    }
    s.opened = QDateTime::currentSecsSinceEpoch();
    sessions[key] = s;
    model->upsert(s);
    persist([&] { store->save(s); });
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
            error("Select a project or one of its sessions first.");
            return;
        }
        scopeBackend = index.data(BackendRole).toString();
        scopePath = index.data(PathRole).toString();
    }
    ++searchToken;
    searchingView = true;
    searchSessions.clear();
    results->resetResults();
    searchErrors.clear();
    searching.clear();
    filter->setSourceModel(results);
    filter->setActiveOnly(false);
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
    ++searchToken;
    for (auto *b : backends)
        b->search({}, searchToken);
    searchingView = false;
    searching.clear();
    searchSessions.clear();
    results->resetResults();
    filter->setSourceModel(model);
    filter->setActiveOnly(tabs->currentIndex() == 1);
    tabs->setEnabled(true);
    loadMore->setEnabled(std::any_of(more.begin(), more.end(), [](bool x) { return x; }));
    updateSummary();
}
void Window::updateSearchSummary() {
    notice->setText(QString("%1 results%2%3")
                        .arg(searchSessions.size())
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
        QStringList expanded;
        for (int i = 0; i < filter->rowCount(); ++i) {
            auto idx = filter->index(i, 0);
            if (tree->isExpanded(idx))
                expanded << idx.data(KeyRole).toString();
        }
        settings.setValue("expanded", expanded);
    }
    for (auto *b : backends)
        b->disconnectServer();
    QMainWindow::closeEvent(event);
}
} // namespace Archcodex
