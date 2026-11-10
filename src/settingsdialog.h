// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QDialog>

class QCheckBox;
class QLineEdit;
class QLabel;
namespace Archcodex {
class SettingsDialog : public QDialog {
    Q_OBJECT
  public:
    SettingsDialog(bool hideUnsaved, bool showAllMyCodexSessions,
                   const QString &localExecutable = {}, QWidget *parent = nullptr);
  signals:
    void settingsApplied(bool hideUnsaved, bool showAllMyCodexSessions,
                         const QString &localExecutable);

  private:
    QCheckBox *hideUnsaved, *showAllMyCodexSessions;
    QLineEdit *localCodex;
    QLabel *validation;
    bool apply();
};
} // namespace Archcodex
