// SPDX-License-Identifier: GPL-3.0-or-later
#include "commands.h"
#include <KActionCategory>
#include <KConfig>
#include <KConfigGroup>
#include <QAction>
#include <QKeyEvent>
#include <QWidget>

namespace Archcodex {
static QKeyCombination canonical(QKeyCombination combination) {
    auto key = combination.key();
    auto modifiers = combination.keyboardModifiers();
    if (key == Qt::Key_Backtab) {
        key = Qt::Key_Tab;
        modifiers |= Qt::ShiftModifier;
    } else if (key == Qt::Key_AsciiTilde) {
        key = Qt::Key_QuoteLeft;
        modifiers |= Qt::ShiftModifier;
    }
    return QKeyCombination(modifiers, key);
}
Commands::Commands(QWidget *owner)
    : QObject(owner), window(owner), actions(new KActionCollection(this)) {
    actions->setComponentDisplayName("Archcodex");
}
QAction *Commands::add(const QString &id, const QString &label, const QString &category,
                       const QList<QKeySequence> &defaults, const std::function<void()> &handler,
                       QWidget *scope) {
    auto *action = new QAction(label, this);
    adopt(id, action, category);
    if (scope) {
        window->removeAction(action);
        scope->addAction(action);
        action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    }
    actions->setDefaultShortcuts(action, defaults);
    connect(action, &QAction::triggered, this, handler);
    return action;
}
QAction *Commands::adopt(const QString &id, QAction *action, const QString &category) {
    actions->addAction(id, action);
    action->setProperty("category", category);
    action->setShortcutContext(Qt::WindowShortcut);
    action->setAutoRepeat(false);
    window->addAction(action);
    return action;
}
KActionCollection *Commands::collection() const {
    return actions;
}
bool Commands::owns(const QKeySequence &sequence) const {
    if (sequence.isEmpty())
        return false;
    for (const auto *action : actions->actions())
        if (action->shortcuts().contains(sequence))
            return true;
    return false;
}
bool Commands::matches(QKeyEvent *event) const {
    return owns(QKeySequence(event->keyCombination()));
}
QAction *Commands::cycleAction(QKeyEvent *event) const {
    for (auto *action : actions->actions())
        if (action->property("cycleKind").toInt())
            for (const auto &sequence : action->shortcuts())
                if (sequence.count() == 1 &&
                    canonical(sequence[0]) == canonical(event->keyCombination()))
                    return action;
    return nullptr;
}
Commands::Bindings Commands::bindings(KActionCollection *collection) {
    Bindings result;
    for (auto *action : collection->actions())
        result[action->objectName()] = action->shortcuts();
    return result;
}
KActionCollection *Commands::draft(QObject *parent) const {
    auto *result = new KActionCollection(parent);
    result->setComponentDisplayName("Archcodex");
    QHash<QString, KActionCategory *> categories;
    for (auto *original : actions->actions()) {
        const auto name = original->property("category").toString();
        if (!categories.contains(name))
            categories[name] = new KActionCategory(name, result);
        auto *copy = categories[name]->addAction(original->objectName());
        copy->setText(original->text());
        copy->setShortcutContext(original->shortcutContext());
        result->setDefaultShortcuts(copy, KActionCollection::defaultShortcuts(original));
        copy->setShortcuts(original->shortcuts());
        // Draft actions have no associated widgets or handlers. Editing a key
        // cannot change live input routing until the whole draft is validated.
    }
    return result;
}
QString Commands::validate(const Bindings &values) const {
    QMap<QString, QString> used;
    for (auto *action : actions->actions()) {
        const auto id = action->objectName();
        const bool cycle = action->property("cycleKind").toInt();
        if (cycle && values.value(id).size() > 1)
            return action->text() + ": use one shortcut for a held-key switcher.";
        for (const auto &sequence : values.value(id)) {
            if (sequence.isEmpty())
                continue;
            if (sequence.count() != 1)
                return action->text() +
                       ": use a single key combination, not a multi-step sequence.";
            const auto combination = sequence[0];
            const auto key = combination.key();
            const auto modifiers = combination.keyboardModifiers();
            const bool function = key >= Qt::Key_F1 && key <= Qt::Key_F35;
            if (!(modifiers & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) &&
                !function && !(key == Qt::Key_Insert && modifiers == Qt::ShiftModifier))
                return action->text() +
                       ": use Ctrl, Alt or Meta, or a function key, to preserve ordinary typing.";
            if (cycle) {
                const auto held =
                    modifiers & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);
                if (held != Qt::ControlModifier && held != Qt::AltModifier &&
                    held != Qt::MetaModifier)
                    return action->text() +
                           ": hold one of Ctrl, Alt or Meta, optionally with Shift.";
            }
            const auto portable =
                QKeySequence(canonical(combination)).toString(QKeySequence::PortableText);
            if (used.contains(portable) && used[portable] != id)
                return portable + " is assigned to both “" +
                       actions->action(used[portable])->text() + "” and “" + action->text() + "”.";
            used[portable] = id;
        }
    }
    for (const auto kind : {1, 2}) {
        Qt::KeyboardModifiers held;
        for (auto *action : actions->actions())
            if (action->property("cycleKind").toInt() == kind)
                for (const auto &sequence : values.value(action->objectName())) {
                    if (sequence.isEmpty())
                        continue;
                    const auto modifier =
                        sequence[0].keyboardModifiers() &
                        (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);
                    if (held && held != modifier)
                        return "Forward and backward cycling must use the same held modifier.";
                    held = modifier;
                }
    }
    return {};
}
QString Commands::load(const QString &file) {
    configFile = file;
    const auto defaults = bindings(actions);
    KConfig config(file, KConfig::SimpleConfig);
    auto group = config.group("Shortcuts");
    actions->readSettings(&group);
    const auto issue = validate(bindings(actions));
    if (!issue.isEmpty())
        for (auto *action : actions->actions())
            action->setShortcuts(defaults.value(action->objectName()));
    emit bindingsChanged();
    return issue;
}
bool Commands::apply(const Bindings &values, QString *error) {
    *error = validate(values);
    if (!error->isEmpty())
        return false;
    const auto old = bindings(actions);
    for (auto *action : actions->actions())
        action->setShortcuts(values.value(action->objectName()));
    if (!configFile.isEmpty()) {
        KConfig config(configFile, KConfig::SimpleConfig);
        auto group = config.group("Shortcuts");
        actions->writeSettings(&group);
        if (!config.sync()) {
            for (auto *action : actions->actions())
                action->setShortcuts(old.value(action->objectName()));
            *error = "Could not save keyboard shortcuts. Existing shortcuts have been kept.";
            return false;
        }
    }
    emit bindingsChanged();
    return true;
}
} // namespace Archcodex
