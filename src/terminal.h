// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "types.h"
#include <KParts/ReadOnlyPart>
#include <QPointer>
#include <QWidget>
class QStackedWidget;
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

  private:
    QStackedWidget *stack;
    QWidget *empty;
    QHash<QString, QPointer<KParts::ReadOnlyPart>> parts;
    QHash<QString, QString> titles;
    KParts::ReadOnlyPart *create(QString *error);
};
} // namespace Archcodex
