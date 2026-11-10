// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <KActionCollection>
#include <QKeySequence>
#include <QMap>
#include <QObject>
#include <functional>

class QWidget;
class QKeyEvent;
namespace Archcodex {
// Stable action IDs are shared by menus, keyboard input and shortcut settings.
class Commands : public QObject {
    Q_OBJECT
  public:
    explicit Commands(QWidget *window);
    QAction *add(const QString &id, const QString &label, const QString &category,
                 const QList<QKeySequence> &defaults, const std::function<void()> &handler,
                 QWidget *scope = nullptr);
    QAction *adopt(const QString &id, QAction *action, const QString &category);
    KActionCollection *collection() const;
    bool owns(const QKeySequence &sequence) const;
    bool matches(QKeyEvent *event) const;
    QAction *cycleAction(QKeyEvent *event) const;
    using Bindings = QMap<QString, QList<QKeySequence>>;
    static Bindings bindings(KActionCollection *collection);
    KActionCollection *draft(QObject *parent) const;
    QString load(const QString &file);
    QString validate(const Bindings &bindings) const;
    bool apply(const Bindings &bindings, QString *error);
  signals:
    void bindingsChanged();

  private:
    QWidget *window;
    KActionCollection *actions;
    QString configFile;
};
} // namespace Archcodex
