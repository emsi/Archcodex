// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QDialog>

namespace Archcodex {
class Updater;
class UpdateDialog : public QDialog {
    Q_OBJECT
  public:
    UpdateDialog(Updater *updater, const QString &version, QWidget *parent = nullptr);
  signals:
    void skipVersion(const QString &version);
};
} // namespace Archcodex
