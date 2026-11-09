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
    bool newSession(const BackendProfile &profile, const QString &path, QString *error);
    void closeCurrent();
    void closeBackend(const QString &backend);
    bool smokeTest(QString *error);
  signals:
    void currentChanged(const QString &title);
    void clientExited(const QString &key, int exitCode, bool crashed);

  private slots:
    void processExited(int exitCode, bool crashed);

  private:
    QStackedWidget *stack;
    QWidget *empty;
    QWidget *exitBar;
    QLabel *exitText;
    QHash<QString, QPointer<KParts::ReadOnlyPart>> parts;
    QHash<QString, QString> titles;
    QHash<QString, QStringList> launches;
    QHash<QString, QString> exits;
    KParts::ReadOnlyPart *create(QString *error);
    bool launch(const QString &key, const QStringList &arguments, const QString &title,
                QString *error);
    QString currentKey() const;
    void updateCurrent();
};
} // namespace Archcodex
