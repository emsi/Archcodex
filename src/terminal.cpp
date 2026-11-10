// SPDX-License-Identifier: GPL-3.0-or-later
#include "terminal.h"
#include <KActionCollection>
#include <KPluginFactory>
#include <KPluginMetaData>
#include <KXMLGUIClient>
#include <QAction>
#include <QApplication>
#include <QDir>
#include <QKeyEvent>
#include <QLabel>
#include <QMetaObject>
#include <QPushButton>
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
    finishCycle();
    remember(key);
    part->widget()->setFocus();
    updateCurrent();
    emit sessionFocused(key);
    return true;
}
bool TerminalArea::focusSession(const QString &key) {
    if (!parts.value(key))
        return false;
    finishCycle();
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
void TerminalArea::finishCycle() {
    if (cycle.isEmpty())
        return;
    cycle.clear();
    remember(currentKey());
}
void TerminalArea::cycleSession(bool backwards) {
    if (cycle.isEmpty()) {
        for (const auto &key : recent)
            if (parts.value(key))
                cycle.append(key);
        cycleIndex = cycle.indexOf(currentKey());
        if (cycleIndex < 0 && backwards)
            cycleIndex = 0;
    }
    if (cycle.isEmpty())
        return;
    cycleIndex = (cycleIndex + (backwards ? -1 : 1) + cycle.size()) % cycle.size();
    showSession(cycle[cycleIndex]);
}
void TerminalArea::switchRecent(bool backwards) {
    cycleSession(backwards);
    finishCycle();
}
bool TerminalArea::eventFilter(QObject *watched, QEvent *event) {
    if (event->type() == QEvent::ApplicationDeactivate) {
        finishCycle();
        return false;
    }
    auto *widget = qobject_cast<QWidget *>(watched);
    if (!widget || widget->window() != window())
        return false;
    if (event->type() == QEvent::WindowDeactivate || event->type() == QEvent::MouseButtonPress)
        finishCycle();
    if (event->type() == QEvent::KeyRelease) {
        const auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Control && !key->isAutoRepeat())
            finishCycle();
    }
    if (event->type() != QEvent::ShortcutOverride && event->type() != QEvent::KeyPress)
        return false;
    auto *key = static_cast<QKeyEvent *>(event);
    const auto modifiers = key->modifiers();
    if ((modifiers & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) ==
            Qt::ControlModifier &&
        (key->key() == Qt::Key_Tab || key->key() == Qt::Key_Backtab)) {
        if (event->type() == QEvent::KeyPress)
            cycleSession(modifiers.testFlag(Qt::ShiftModifier) || key->key() == Qt::Key_Backtab);
        key->accept();
        return true;
    }
    if (event->type() == QEvent::KeyPress && !modifiers.testFlag(Qt::ControlModifier))
        finishCycle();
    return false;
}
void TerminalArea::forgetClient(const QString &key) {
    finishCycle();
    recent.removeAll(key);
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
