// SPDX-License-Identifier: GPL-3.0-or-later
#include "terminal.h"
#include "commands.h"
#include <KActionCollection>
#include <KPluginFactory>
#include <KPluginMetaData>
#include <KXMLGUIClient>
#include <QAction>
#include <QApplication>
#include <QDir>
#include <QFrame>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QMetaObject>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QUuid>
#include <QVBoxLayout>
#include <kde_terminal_interface.h>

namespace Archcodex {
TerminalArea::TerminalArea(QWidget *parent) : QWidget(parent), stack(new QStackedWidget(this)) {
    qApp->installEventFilter(this);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    exitBar = new QWidget(this);
    auto *exitLayout = new QHBoxLayout(exitBar);
    exitText = new QLabel;
    exitText->setTextFormat(Qt::PlainText);
    exitText->setWordWrap(true);
    exitText->setTextInteractionFlags(Qt::TextSelectableByMouse);
    exitLayout->addWidget(exitText, 1);
    auto *retry = new QPushButton("Restart client");
    retry->setObjectName("reconnectTerminal");
    exitLayout->addWidget(retry);
    exitBar->hide();
    layout->addWidget(exitBar);
    layout->addWidget(stack);
    auto *label = new QLabel(
        "Select an existing session\n\nLocal Codex sessions are discovered automatically.\n"
        "Projects group their working directories; no project setup is needed.",
        this);
    label->setAlignment(Qt::AlignCenter);
    label->setTextFormat(Qt::PlainText);
    empty = label;
    stack->addWidget(empty);
    connect(stack, &QStackedWidget::currentChanged, this, &TerminalArea::updateCurrent);
    connect(retry, &QPushButton::clicked, this, [this] {
        const auto key = currentKey();
        if (key.isEmpty() || !exits.contains(key))
            return;
        // The controller owns current profiles and session evidence. Never retry
        // a captured command after rediscovery has changed the execution context.
        emit restartRequested(clients.value(key));
    });
}
TerminalArea::~TerminalArea() {
    qApp->removeEventFilter(this);
    delete switcher;
    stack->disconnect(this);
    for (auto *p : findChildren<KParts::ReadOnlyPart *>())
        p->disconnect(this);
}
void TerminalArea::setLocalCodexExecutable(const QString &path) {
    localCodexExecutable = path;
}
KParts::ReadOnlyPart *TerminalArea::create(QString *error) {
    const auto result = KPluginFactory::instantiatePlugin<KParts::ReadOnlyPart>(
        KPluginMetaData("kf6/parts/konsolepart"), this);
    if (!result) {
        *error =
            "The packaged Konsole terminal component could not be loaded: " + result.errorString;
        return nullptr;
    }
    auto *part = result.plugin;
    if (!qobject_cast<TerminalInterface *>(part)) {
        *error = "Konsole does not expose the expected terminal interface.";
        delete part;
        return nullptr;
    }
    // Our pinned KPart exposes its existing hold-open facility and the PTY exit
    // event. Never inspect foreground processes or infer exit from terminal text.
    if (part->metaObject()->indexOfSignal("terminalProcessExited(int,bool)") < 0 ||
        !QMetaObject::invokeMethod(part, "setAutoClose", Q_ARG(bool, false))) {
        *error = "The Konsole component lacks Archcodex's terminal lifecycle support. Reinstall "
                 "the matching package.";
        delete part;
        return nullptr;
    }
    connect(part, SIGNAL(terminalProcessExited(int, bool)), this, SLOT(processExited(int, bool)));
    QMetaObject::invokeMethod(part, "setMonitorSilenceEnabled", Q_ARG(bool, false));
    QMetaObject::invokeMethod(part, "setMonitorActivityEnabled", Q_ARG(bool, false));
    const auto disableTracking = [](auto &&self, KXMLGUIClient *client) -> void {
        for (auto *action : client->actionCollection()->actions()) {
            if (action->objectName().startsWith("monitor-")) {
                action->setChecked(false);
                action->setEnabled(false);
                action->setVisible(false);
            }
        }
        for (auto *child : client->childClients())
            self(self, child);
    };
    disableTracking(disableTracking, part);
    connect(part, SIGNAL(overrideShortcut(QKeyEvent *, bool &)), this,
            SLOT(overrideShortcut(QKeyEvent *, bool &)), Qt::DirectConnection);
    updateShortcuts();
    stack->addWidget(part->widget());
    connect(part, &QObject::destroyed, this, [this] {
        for (auto it = parts.begin(); it != parts.end();)
            if (it.value().isNull()) {
                forgetClient(it.key());
                titles.remove(it.key());
                exits.remove(it.key());
                it = parts.erase(it);
            } else
                ++it;
        updateCurrent();
    });
    return part;
}
void TerminalArea::setCommands(Commands *value) {
    commands = value;
    connect(commands, &Commands::bindingsChanged, this, &TerminalArea::updateShortcuts);
    updateShortcuts();
}
void TerminalArea::updateShortcuts() {
    if (!commands)
        return;
    const auto visit = [this](auto &&self, KXMLGUIClient *client) -> void {
        for (auto *action : client->actionCollection()->actions()) {
            if (!action->property("archcodexOriginalShortcuts").isValid())
                action->setProperty("archcodexOriginalShortcuts",
                                    QVariant::fromValue(action->shortcuts()));
            auto keys = action->property("archcodexOriginalShortcuts").value<QList<QKeySequence>>();
            keys.removeIf([this](const QKeySequence &key) { return commands->owns(key); });
            action->setShortcuts(keys);
        }
        for (auto *child : client->childClients())
            self(self, child);
    };
    for (auto *part : findChildren<KParts::ReadOnlyPart *>())
        visit(visit, part);
}
void TerminalArea::overrideShortcut(QKeyEvent *event, bool &override) {
    if (commands && commands->matches(event))
        override = false;
}
void TerminalArea::focusCurrent() {
    if (auto *part = parts.value(currentKey()).data())
        part->widget()->setFocus(Qt::ShortcutFocusReason);
}
Session TerminalArea::currentSession() const {
    return clients.value(currentKey());
}
bool TerminalArea::triggerAction(const QString &name) {
    auto *part = parts.value(currentKey()).data();
    if (!part)
        return false;
    const auto find = [&](auto &&self, KXMLGUIClient *client) -> QAction * {
        if (auto *action = client->actionCollection()->action(name))
            return action;
        for (auto *child : client->childClients())
            if (auto *action = self(self, child))
                return action;
        return nullptr;
    };
    if (auto *action = find(find, part); action && action->isEnabled()) {
        action->trigger();
        return true;
    }
    return false;
}
bool TerminalArea::open(const BackendProfile &p, const Session &s, QString *error) {
    if (focusSession(sessionKey(p.id, s.id)))
        return true;
    return startSession(p, s, error);
}
bool TerminalArea::restart(const BackendProfile &p, const Session &s, QString *error) {
    const auto key = sessionKey(p.id, s.id);
    if (!parts.value(key) || !exits.contains(key)) {
        *error = "The terminal is no longer available for restart.";
        return false;
    }
    return startSession(p, s, error);
}
bool TerminalArea::startSession(const BackendProfile &p, const Session &s, QString *error) {
    if (p.isContainer() && p.relaySocket.isEmpty()) {
        *error = "This container has no live desktop transport. Reconnect its host first.";
        return false;
    }
    const bool fresh = s.id.startsWith("new-");
    if (p.isContainer() && !fresh &&
        (!s.executionObserved || s.executionEnvironment != p.environmentId)) {
        *error = "This conversation is visible in shared history, but Archcodex has not observed "
                 "it loaded in this container. Open it in its original environment first.";
        return false;
    }
    if (!p.validate().isEmpty() || s.backendId != p.id || s.id.isEmpty() || !validText(s.id, 128) ||
        !validText(s.cwd) || (!s.cwd.isEmpty() && !QDir::isAbsolutePath(s.cwd))) {
        *error = "Invalid session or connection configuration.";
        return false;
    }
    const auto key = sessionKey(p.id, s.id);
    const auto title =
        (fresh ? QString("New session") : s.title) + "  ·  " + p.name + "  ·  " + s.cwd;
    return launch(key, clientArguments(p, fresh ? QString() : s.id, s.cwd, localCodexExecutable),
                  title, s, error);
}
bool TerminalArea::newSession(const BackendProfile &p, const QString &path, QString *error) {
    if (!p.validate().isEmpty() || !validText(path) || !QDir::isAbsolutePath(path)) {
        *error = "Select a project with an absolute working directory.";
        return false;
    }
    Session s;
    s.backendId = p.id;
    s.id = "new-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    s.title = "New session terminal";
    s.cwd = path;
    s.persistenceKnown = true;
    return startSession(p, s, error);
}
bool TerminalArea::launch(const QString &key, const QStringList &arguments, const QString &title,
                          const Session &session, QString *error) {
    auto *part = create(error);
    if (!part)
        return false;
    auto *old = parts.value(key).data();
    parts[key] = part;
    titles[key] = title;
    exits.remove(key);
    auto s = session;
    s.terminalOpen = true;
    s.clientIssue.clear();
    clients[key] = s;
    emit clientStateChanged(s);
    // Konsole already handles the Flatpak host boundary. Arguments stay separate.
    qobject_cast<TerminalInterface *>(part)->startProgram("/bin/sh", arguments);
    stack->setCurrentWidget(part->widget());
    delete old;
    finishCycle(false);
    remember(key);
    part->widget()->setFocus();
    updateCurrent();
    emit sessionFocused(key);
    return true;
}
bool TerminalArea::focusSession(const QString &key) {
    if (!parts.value(key))
        return false;
    finishCycle(false);
    remember(key);
    return showSession(key);
}
bool TerminalArea::showSession(const QString &key) {
    auto *part = parts.value(key).data();
    if (!part)
        return false;
    stack->setCurrentWidget(part->widget());
    part->widget()->setFocus();
    updateCurrent();
    emit sessionFocused(key);
    return true;
}
void TerminalArea::remember(const QString &key) {
    if (key.isEmpty() || !parts.value(key))
        return;
    recent.removeAll(key);
    recent.prepend(key);
}
QStringList TerminalArea::candidates(const QStringList &order, bool projects) const {
    QStringList result;
    QSet<QString> seen;
    for (const auto &key : order) {
        if (!parts.value(key))
            continue;
        const auto &session = clients.value(key);
        const auto project = projectKey(session.backendId, QDir::cleanPath(session.cwd));
        if (projects && seen.contains(project))
            continue;
        seen.insert(project);
        result << key;
    }
    return result;
}
void TerminalArea::finishCycle(bool commit) {
    if (cycleOrder.isEmpty() && cycle.isEmpty())
        return;
    const auto selected = cycle.value(cycleIndex);
    const auto previousFocus = cycleFocus;
    cycleOrder.clear();
    cycleProjectOrder.clear();
    cycle.clear();
    cycleOrigin.clear();
    cycleModifier = {};
    if (switcher)
        switcher->hide();
    if (commit && parts.value(selected)) {
        remember(selected);
        showSession(selected); // Only this path changes focus and records a visit.
    } else if (previousFocus && previousFocus->isVisible() && window()->isActiveWindow() &&
               !QApplication::activeModalWidget()) {
        previousFocus->setFocus(Qt::OtherFocusReason);
    }
}
void TerminalArea::refreshCycle() {
    if (cycleOrder.isEmpty()) {
        if (!cycle.isEmpty())
            finishCycle(false);
        return;
    }
    const auto selected = cycle.value(cycleIndex);
    const auto previous = clients.value(selected);
    const auto previousProject = projectKey(previous.backendId, QDir::cleanPath(previous.cwd));
    cycle = candidates(cycleOrder, cycleProjects);
    if (cycleProjects) {
        QHash<QString, QString> representatives;
        for (const auto &key : cycle) {
            const auto session = clients.value(key);
            representatives[projectKey(session.backendId, QDir::cleanPath(session.cwd))] = key;
        }
        cycle.clear();
        for (const auto &project : cycleProjectOrder)
            if (representatives.contains(project))
                cycle << representatives[project];
        if (representatives.contains(previousProject))
            cycleIndex = cycle.indexOf(representatives[previousProject]);
    }
    if (cycle.isEmpty()) {
        finishCycle(false);
        return;
    }
    const auto index = cycle.indexOf(selected);
    cycleIndex = index < 0 ? qBound(0, cycleIndex, int(cycle.size()) - 1) : index;
    const QSignalBlocker block(switcherList);
    switcherList->clear();
    for (const auto &key : cycle) {
        const auto &session = clients.value(key);
        auto *item =
            new QListWidgetItem(titles.value(key) + "\n" + session.stateText(), switcherList);
        item->setData(Qt::UserRole, key);
        item->setToolTip(titles.value(key) + "\n" + session.id);
    }
    switcherList->setCurrentRow(cycleIndex);
    const auto modifierName = QKeySequence(cycleModifier | Qt::Key_Space)
                                  .toString(QKeySequence::NativeText)
                                  .section('+', 0, 0);
    switcherTitle->setText(
        QString("%1 — release %2 to switch; Esc cancels")
            .arg(cycleProjects ? "Recent projects" : "Recent terminals", modifierName));
    const int width = qMin(850, qMax(200, window()->width() - 40));
    const int height = qMin(qMax(120, window()->height() - 80),
                            qMin(480, 70 + int(cycle.size()) * (fontMetrics().height() * 2 + 8)));
    switcher->setGeometry((window()->width() - width) / 2, (window()->height() - height) / 2, width,
                          height);
}
void TerminalArea::cycleSession(bool backwards, bool projects, Qt::KeyboardModifiers modifier) {
    if (!cycleOrder.isEmpty() && (cycleProjects != projects || cycleModifier != modifier))
        finishCycle(false);
    if (cycleOrder.isEmpty()) {
        const auto available = candidates(recent, projects);
        if (available.isEmpty() || (available.size() == 1 && available.first() == currentKey()))
            return;
        cycleProjects = projects;
        cycleModifier = modifier;
        cycleOrder = recent;
        cycle = available;
        for (const auto &key : cycle) {
            const auto session = clients.value(key);
            cycleProjectOrder << projectKey(session.backendId, QDir::cleanPath(session.cwd));
        }
        cycleOrigin = currentKey();
        cycleFocus = QApplication::focusWidget();
        cycleIndex = cycle.indexOf(cycleOrigin);
        if (cycleIndex < 0 && backwards)
            cycleIndex = 0;
        if (!switcher) {
            switcher = new QFrame(window());
            switcher->setObjectName("recentSwitcher");
            switcher->setFrameShape(QFrame::StyledPanel);
            switcher->setAutoFillBackground(true);
            auto *layout = new QVBoxLayout(switcher);
            switcherTitle = new QLabel;
            switcherTitle->setTextFormat(Qt::PlainText);
            switcherTitle->setWordWrap(true);
            layout->addWidget(switcherTitle);
            switcherList = new QListWidget;
            switcherList->setObjectName("recentSwitcherList");
            switcherList->setAccessibleName("Recent terminals or projects");
            switcherList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
            layout->addWidget(switcherList);
            connect(switcherList, &QListWidget::currentRowChanged, this, [this](int row) {
                if (row >= 0 && row < cycle.size())
                    cycleIndex = row;
            });
        }
    }
    cycleIndex = (cycleIndex + (backwards ? -1 : 1) + cycle.size()) % cycle.size();
    refreshCycle();
    switcher->show();
    switcher->raise();
    switcherList->setFocus(Qt::ShortcutFocusReason);
}
void TerminalArea::switchRecent(bool backwards, bool projects) {
    const auto available = candidates(recent, projects);
    if (available.isEmpty() || (available.size() == 1 && available.first() == currentKey()))
        return;
    auto index = available.indexOf(currentKey());
    if (index < 0 && backwards)
        index = 0;
    focusSession(available[(index + (backwards ? -1 : 1) + available.size()) % available.size()]);
}
bool TerminalArea::eventFilter(QObject *watched, QEvent *event) {
    if (event->type() == QEvent::ApplicationDeactivate) {
        finishCycle(false);
        return false;
    }
    auto *widget = qobject_cast<QWidget *>(watched);
    if (!widget || widget->window() != window())
        return false;
    if (event->type() == QEvent::WindowDeactivate || QApplication::activeModalWidget()) {
        finishCycle(false);
        return false;
    }
    if (event->type() == QEvent::MouseButtonPress &&
        (!switcher || (widget != switcher && !switcher->isAncestorOf(widget))))
        finishCycle(false);
    if (event->type() == QEvent::Resize && watched == window())
        refreshCycle();
    if (event->type() == QEvent::KeyRelease) {
        auto *key = static_cast<QKeyEvent *>(event);
        const bool released =
            !key->isAutoRepeat() &&
            ((key->key() == Qt::Key_Control && cycleModifier == Qt::ControlModifier) ||
             (key->key() == Qt::Key_Alt && cycleModifier == Qt::AltModifier) ||
             (key->key() == Qt::Key_Meta && cycleModifier == Qt::MetaModifier));
        if (released && !cycleOrder.isEmpty()) {
            consumedKeys.remove(key->key());
            finishCycle(true);
            key->accept();
            return true;
        }
        if (consumedKeys.remove(key->key())) {
            key->accept();
            return true;
        }
    }
    if (event->type() != QEvent::ShortcutOverride && event->type() != QEvent::KeyPress)
        return false;
    auto *key = static_cast<QKeyEvent *>(event);
    auto *action = commands ? commands->cycleAction(key) : nullptr;
    if (!cycleOrder.isEmpty() ||
        (action && action->isEnabled() && !QApplication::activePopupWidget())) {
        if (event->type() == QEvent::KeyPress) {
            consumedKeys.insert(key->key());
            if (action) {
                cycleSession(action->property("cycleBackwards").toBool(),
                             action->property("cycleKind").toInt() == 2,
                             key->modifiers() &
                                 (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier));
            } else if (key->key() == Qt::Key_Escape) {
                finishCycle(false);
            } else if (key->key() == Qt::Key_Up || key->key() == Qt::Key_Down) {
                cycleIndex = (cycleIndex + (key->key() == Qt::Key_Up ? -1 : 1) + cycle.size()) %
                             cycle.size();
                switcherList->setCurrentRow(cycleIndex);
            } else if (key->key() == Qt::Key_Home || key->key() == Qt::Key_End) {
                cycleIndex = key->key() == Qt::Key_Home ? 0 : cycle.size() - 1;
                switcherList->setCurrentRow(cycleIndex);
            }
        }
        key->accept();
        return true;
    }
    return false;
}
void TerminalArea::updateSession(const Session &session, const QString &serverName) {
    const auto key = sessionKey(session.backendId, session.id);
    if (!clients.contains(key))
        return;
    auto current = session;
    current.terminalOpen = true;
    current.clientIssue = clients[key].clientIssue;
    clients[key] = current;
    titles[key] = current.title + "  ·  " + serverName + "  ·  " + current.cwd;
    if (currentKey() == key)
        updateCurrent();
    refreshCycle();
}
void TerminalArea::forgetClient(const QString &key) {
    if (key == cycleOrigin)
        finishCycle(false);
    recent.removeAll(key);
    cycleOrder.removeAll(key);
    refreshCycle();
    auto s = clients.take(key);
    if (s.id.isEmpty())
        return;
    s.terminalOpen = false;
    s.clientIssue.clear();
    emit clientStateChanged(s);
}
QString TerminalArea::currentKey() const {
    for (auto it = parts.cbegin(); it != parts.cend(); ++it)
        if (it.value() && it.value()->widget() == stack->currentWidget())
            return it.key();
    return {};
}
void TerminalArea::updateCurrent() {
    const auto key = currentKey();
    exitText->setText(exits.value(key));
    exitBar->setVisible(exits.contains(key));
    emit currentChanged(titles.value(key, "No terminal selected"));
}
void TerminalArea::processExited(int exitCode, bool crashed) {
    for (auto it = parts.cbegin(); it != parts.cend(); ++it)
        if (it.value() == sender()) {
            const auto key = it.key();
            exits[key] =
                crashed ? "Terminal client crashed. Its output is retained below."
                : exitCode
                    ? QString("Terminal client exited with code %1. Its output is retained below.")
                          .arg(exitCode)
                    : "Terminal client closed. Its output is retained below.";
            clients[key].clientIssue = exits[key];
            emit clientStateChanged(clients[key]);
            updateCurrent();
            refreshCycle();
            emit clientExited(key, exitCode, crashed);
            return;
        }
}
void TerminalArea::closeCurrent() {
    closeSession(currentKey());
}
void TerminalArea::closeSession(const QString &key) {
    auto *part = parts.value(key).data();
    if (!part)
        return;
    const bool current = currentKey() == key;
    forgetClient(key);
    titles.remove(key);
    exits.remove(key);
    parts.remove(key);
    if (current)
        stack->setCurrentWidget(empty);
    delete part;
    updateCurrent();
}
void TerminalArea::closeBackend(const QString &backend) {
    QList<KParts::ReadOnlyPart *> doomed;
    for (auto it = parts.begin(); it != parts.end();)
        if (it.key().startsWith(backend + QChar(0x1f))) {
            forgetClient(it.key());
            titles.remove(it.key());
            exits.remove(it.key());
            if (it.value())
                doomed << it.value();
            it = parts.erase(it);
        } else
            ++it;
    for (auto *p : doomed)
        delete p;
}
bool TerminalArea::smokeTest(QString *error) {
    auto *p = create(error);
    if (!p)
        return false;
    qobject_cast<TerminalInterface *>(p)->startProgram(
        "/bin/sh", {"/bin/sh", "-c", "printf 'Archcodex terminal smoke test\\n'; sleep 1"});
    stack->setCurrentWidget(p->widget());
    return true;
}
} // namespace Archcodex
