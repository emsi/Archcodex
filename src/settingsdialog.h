// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QDialog>

class QCheckBox;
namespace Archcodex {
class SettingsDialog : public QDialog {
    Q_OBJECT
  public:
    SettingsDialog(bool hideUnsaved, bool showAllMyCodexSessions, QWidget *parent = nullptr);
  signals:
    void settingsApplied(bool hideUnsaved, bool showAllMyCodexSessions);

  private:
    QCheckBox *hideUnsaved, *showAllMyCodexSessions;
    void apply();
};
} // namespace Archcodex
