// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QDialog>

class QCheckBox;
class QLineEdit;
class QLabel;
class KActionCollection;
namespace Archcodex {
class Commands;
class SettingsDialog : public QDialog {
    Q_OBJECT
  public:
    SettingsDialog(bool hideUnsaved, bool showAllMyCodexSessions,
                   const QString &localExecutable = {}, QWidget *parent = nullptr,
                   Commands *commands = nullptr);
  signals:
    void settingsApplied(bool hideUnsaved, bool showAllMyCodexSessions,
                         const QString &localExecutable);

  private:
    QCheckBox *hideUnsaved, *showAllMyCodexSessions;
    QLineEdit *localCodex;
    QLabel *validation;
    Commands *commands;
    KActionCollection *shortcutDraft = nullptr;
    bool apply();
};
} // namespace Archcodex
