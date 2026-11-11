// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "types.h"
#include "workspace.h"
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
    WorkspaceView prepare(const BackendProfile &profile, const Session &session, bool activate);
    bool startPrepared(const QString &viewId, const BackendProfile &profile, const Session &session,
                       QString *error);
    void restoreWorkspace(const WorkspaceSnapshot &snapshot,
                          const QHash<QString, BackendProfile> &profiles);
    WorkspaceSnapshot workspace() const;
    WorkspaceView view(const QString &key) const;
    void openingState(const QString &viewId, const QString &text, bool needsAttention);
    bool hasClient(const QString &key) const;
    void freezeWorkspace();
  signals:
    void currentChanged(const QString &title);
    void currentSessionChanged(const QString &key);
    void clientStateChanged(const Archcodex::Session &session);
    void clientExited(const QString &key, int exitCode, bool crashed);
    void restartRequested(const Archcodex::Session &session);
    void sessionFocused(const QString &key);
    void workspaceChanged();
    void viewClosed(const QString &viewId);
    void connectionsRequested();

  protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

  private slots:
    void processExited(int exitCode, bool crashed);
    void overrideShortcut(QKeyEvent *event, bool &override);

  private:
    QStackedWidget *stack;
    QWidget *empty;
    struct Page {
        QWidget *widget = nullptr;
        QWidget *banner = nullptr;
        QLabel *message = nullptr;
    };
    QHash<QString, Page> pages;
    QHash<QString, QPointer<KParts::ReadOnlyPart>> parts;
    QHash<QString, QString> titles;
    QHash<QString, QString> exits;
    QHash<QString, Session> clients;
    QHash<QString, WorkspaceView> views;
    QHash<QString, QWidget *> placeholders;
    bool frozen = false;
    QStringList recent, cycle, cycleOrder, cycleProjectOrder;
    QString localCodexExecutable;
    QString displayedKey;
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
    bool startSession(const BackendProfile &profile, const Session &session, QString *error,
                      bool activate = true);
    bool launch(const QString &key, const QStringList &arguments, const QString &title,
                const Session &session, QString *error, bool activate = true);
    QWidget *viewWidget(const QString &key) const;
    QWidget *addPage(const QString &key, QWidget *content);
    void updateStatus(const QString &key);
    void addPlaceholder(const WorkspaceView &view, const QString &serverName);
    void updateCurrent();
};
} // namespace Archcodex
