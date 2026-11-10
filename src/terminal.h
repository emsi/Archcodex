// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "types.h"
#include <KParts/ReadOnlyPart>
#include <QPointer>
#include <QWidget>
class QStackedWidget;
class QLabel;
class QKeyEvent;
class QFrame;
class QListWidget;
namespace Archcodex {
class Commands;
class TerminalArea : public QWidget {
    Q_OBJECT
  public:
    explicit TerminalArea(QWidget *parent = nullptr);
    ~TerminalArea() override;
    bool open(const BackendProfile &profile, const Session &session, QString *error);
    bool restart(const BackendProfile &profile, const Session &session, QString *error);
    bool newSession(const BackendProfile &profile, const QString &path, QString *error);
    bool focusSession(const QString &key);
    void switchRecent(bool backwards = false, bool projects = false);
    QString currentKey() const;
    void closeCurrent();
    void closeSession(const QString &key);
    void closeBackend(const QString &backend);
    bool smokeTest(QString *error);
    void setLocalCodexExecutable(const QString &path);
    void setCommands(Commands *commands);
    void focusCurrent();
    Session currentSession() const;
    bool triggerAction(const QString &name);
    void updateSession(const Session &session, const QString &serverName);
  signals:
    void currentChanged(const QString &title);
    void clientStateChanged(const Archcodex::Session &session);
    void clientExited(const QString &key, int exitCode, bool crashed);
    void restartRequested(const Archcodex::Session &session);
    void sessionFocused(const QString &key);

  protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

  private slots:
    void processExited(int exitCode, bool crashed);
    void overrideShortcut(QKeyEvent *event, bool &override);

  private:
    QStackedWidget *stack;
    QWidget *empty;
    QWidget *exitBar;
    QLabel *exitText;
    QHash<QString, QPointer<KParts::ReadOnlyPart>> parts;
    QHash<QString, QString> titles;
    QHash<QString, QString> exits;
    QHash<QString, Session> clients;
    QStringList recent, cycle, cycleOrder, cycleProjectOrder;
    QString localCodexExecutable;
    int cycleIndex = 0;
    bool cycleProjects = false;
    Qt::KeyboardModifiers cycleModifier;
    QPointer<QWidget> cycleFocus;
    QString cycleOrigin;
    QSet<int> consumedKeys;
    QFrame *switcher = nullptr;
    QListWidget *switcherList = nullptr;
    QLabel *switcherTitle = nullptr;
    QPointer<Commands> commands;
    void updateShortcuts();
    void remember(const QString &key);
    bool showSession(const QString &key);
    void cycleSession(bool backwards, bool projects, Qt::KeyboardModifiers modifier);
    void finishCycle(bool commit);
    void refreshCycle();
    QStringList candidates(const QStringList &order, bool projects) const;
    void forgetClient(const QString &key);
    KParts::ReadOnlyPart *create(QString *error);
    bool startSession(const BackendProfile &profile, const Session &session, QString *error);
    bool launch(const QString &key, const QStringList &arguments, const QString &title,
                const Session &session, QString *error);
    void updateCurrent();
};
} // namespace Archcodex
