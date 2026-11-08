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
#include <QStackedWidget>
#include <QUuid>
#include <QVBoxLayout>
#include <kde_terminal_interface.h>

namespace Archcodex {
TerminalArea::TerminalArea(QWidget *parent) : QWidget(parent), stack(new QStackedWidget(this)) {
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(stack);
    auto *label = new QLabel(
        "Select a session\n\nAdd a connection to discover projects, or add a project manually.",
        this);
    label->setAlignment(Qt::AlignCenter);
    label->setTextFormat(Qt::PlainText);
    empty = label;
    stack->addWidget(empty);
    connect(stack, &QStackedWidget::currentChanged, this, [this] {
        for (auto it = parts.begin(); it != parts.end(); ++it)
            if (it.value() && it.value()->widget() == stack->currentWidget()) {
                emit currentChanged(titles.value(it.key()));
                return;
            }
        emit currentChanged("No terminal selected");
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
                it = parts.erase(it);
            } else
                ++it;
        if (stack->currentWidget() == empty)
            emit currentChanged("No terminal selected");
    });
    return part;
}
bool TerminalArea::open(const BackendProfile &p, const Session &s, QString *error) {
    if (!p.validate().isEmpty() || s.id.isEmpty() || !validText(s.id, 128)) {
        *error = "Invalid session or connection configuration.";
        return false;
    }
    const auto key = sessionKey(p.id, s.id);
    titles[key] = s.title + "  ·  " + p.name + "  ·  " + s.cwd;
    auto *part = parts.value(key).data();
    if (!part) {
        part = create(error);
        if (!part)
            return false;
        parts[key] = part;
        // Konsole handles the Flatpak host boundary itself. The constant shell wrapper
        // preserves argv exactly and lets the host resolve its configured Codex executable.
        qobject_cast<TerminalInterface *>(part)->startProgram("/bin/sh",
                                                              terminalArguments(p, s.id));
    }
    stack->setCurrentWidget(part->widget());
    part->widget()->setFocus();
    emit currentChanged(s.title + "  ·  " + p.name + "  ·  " + s.cwd);
    return true;
}
bool TerminalArea::newSession(const BackendProfile &p, const QString &path, QString *error) {
    if (!p.validate().isEmpty() || !validText(path) || !QDir::isAbsolutePath(path)) {
        *error = "Select a project with an absolute working directory.";
        return false;
    }
    auto *part = create(error);
    if (!part)
        return false;
    const auto key = sessionKey(p.id, "new-" + QUuid::createUuid().toString());
    parts[key] = part;
    titles[key] = "New session · " + p.name + " · " + path;
    qobject_cast<TerminalInterface *>(part)->startProgram(
        "/bin/sh", {"/bin/sh", "-c", "exec \"$@\"", "archcodex", p.executable, "--remote",
                    p.endpoint(), "--cd", path});
    stack->setCurrentWidget(part->widget());
    part->widget()->setFocus();
    emit currentChanged("New session · " + p.name + " · " + path);
    return true;
}
void TerminalArea::closeCurrent() {
    for (auto it = parts.begin(); it != parts.end(); ++it)
        if (it.value() && it.value()->widget() == stack->currentWidget()) {
            auto *part = it.value().data();
            titles.remove(it.key());
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
