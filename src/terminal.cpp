// SPDX-License-Identifier: GPL-3.0-or-later
#include "terminal.h"
#include <KActionCollection>
#include <KPluginFactory>
#include <KPluginMetaData>
#include <KXMLGUIClient>
#include <QAction>
#include <QDir>
#include <QLabel>
#include <QMetaObject>
#include <QPushButton>
#include <QStackedWidget>
#include <QUuid>
#include <QVBoxLayout>
#include <kde_terminal_interface.h>

namespace Archcodex {
TerminalArea::TerminalArea(QWidget *parent) : QWidget(parent), stack(new QStackedWidget(this)) {
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
        QString error;
        if (!launch(key, launches.value(key), titles.value(key), &error))
            exitText->setText(error);
    });
}
TerminalArea::~TerminalArea() {
    stack->disconnect(this);
    for (auto *p : findChildren<KParts::ReadOnlyPart *>())
        p->disconnect(this);
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
                titles.remove(it.key());
                launches.remove(it.key());
                exits.remove(it.key());
                it = parts.erase(it);
            } else
                ++it;
        updateCurrent();
    });
    return part;
}
bool TerminalArea::open(const BackendProfile &p, const Session &s, QString *error) {
    if (!p.validate().isEmpty() || s.id.isEmpty() || !validText(s.id, 128)) {
        *error = "Invalid session or connection configuration.";
        return false;
    }
    const auto key = sessionKey(p.id, s.id);
    const auto title = s.title + "  ·  " + p.name + "  ·  " + s.cwd;
    auto *part = parts.value(key).data();
    if (!part)
        return launch(key, terminalArguments(p, s.id), title, error);
    titles[key] = title;
    stack->setCurrentWidget(part->widget());
    part->widget()->setFocus();
    updateCurrent();
    return true;
}
bool TerminalArea::newSession(const BackendProfile &p, const QString &path, QString *error) {
    if (!p.validate().isEmpty() || !validText(path) || !QDir::isAbsolutePath(path)) {
        *error = "Select a project with an absolute working directory.";
        return false;
    }
    const auto key = sessionKey(p.id, "new-" + QUuid::createUuid().toString());
    return launch(key,
                  {"/bin/sh", "-c", "exec \"$@\"", "archcodex", p.executable, "--remote",
                   p.endpoint(), "--cd", path},
                  "New session · " + p.name + " · " + path, error);
}
bool TerminalArea::launch(const QString &key, const QStringList &arguments, const QString &title,
                          QString *error) {
    auto *part = create(error);
    if (!part)
        return false;
    auto *old = parts.value(key).data();
    parts[key] = part;
    titles[key] = title;
    launches[key] = arguments;
    exits.remove(key);
    // Konsole already handles the Flatpak host boundary. Arguments stay separate.
    qobject_cast<TerminalInterface *>(part)->startProgram("/bin/sh", arguments);
    stack->setCurrentWidget(part->widget());
    delete old;
    part->widget()->setFocus();
    updateCurrent();
    return true;
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
            updateCurrent();
            emit clientExited(key, exitCode, crashed);
            return;
        }
}
void TerminalArea::closeCurrent() {
    for (auto it = parts.begin(); it != parts.end(); ++it)
        if (it.value() && it.value()->widget() == stack->currentWidget()) {
            auto *part = it.value().data();
            titles.remove(it.key());
            launches.remove(it.key());
            exits.remove(it.key());
            parts.erase(it);
            stack->setCurrentWidget(empty);
            delete part;
            emit currentChanged("No terminal selected");
            return;
        }
}
void TerminalArea::closeBackend(const QString &backend) {
    QList<KParts::ReadOnlyPart *> doomed;
    for (auto it = parts.begin(); it != parts.end();)
        if (it.key().startsWith(backend + QChar(0x1f))) {
            titles.remove(it.key());
            launches.remove(it.key());
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
