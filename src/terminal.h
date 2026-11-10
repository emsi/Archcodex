// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "types.h"
#include <KParts/ReadOnlyPart>
#include <QPointer>
#include <QWidget>
class QStackedWidget;
class QLabel;
namespace Archcodex {
class TerminalArea : public QWidget {
    Q_OBJECT
  public:
    explicit TerminalArea(QWidget *parent = nullptr);
    ~TerminalArea() override;
    bool open(const BackendProfile &profile, const Session &session, QString *error);
    bool restart(const BackendProfile &profile, const Session &session, QString *error);
    bool newSession(const BackendProfile &profile, const QString &path, QString *error);
    bool focusSession(const QString &key);
    void switchRecent(bool backwards = false);
    QString currentKey() const;
    void closeCurrent();
    void closeSession(const QString &key);
    void closeBackend(const QString &backend);
    bool smokeTest(QString *error);
    void setLocalCodexExecutable(const QString &path);
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

  private:
    QStackedWidget *stack;
    QWidget *empty;
    QWidget *exitBar;
    QLabel *exitText;
    QHash<QString, QPointer<KParts::ReadOnlyPart>> parts;
    QHash<QString, QString> titles;
    QHash<QString, QString> exits;
    QHash<QString, Session> clients;
    QStringList recent, cycle;
    QString localCodexExecutable;
    int cycleIndex = 0;
    void remember(const QString &key);
    bool showSession(const QString &key);
    void cycleSession(bool backwards);
    void finishCycle();
    void forgetClient(const QString &key);
    KParts::ReadOnlyPart *create(QString *error);
    bool startSession(const BackendProfile &profile, const Session &session, QString *error);
    bool launch(const QString &key, const QStringList &arguments, const QString &title,
                const Session &session, QString *error);
    void updateCurrent();
};
} // namespace Archcodex
