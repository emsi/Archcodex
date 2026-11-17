// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "hosts.h"
#include <QDialog>
#include <optional>
class QLabel;
class QPlainTextEdit;
class QLineEdit;
class QCheckBox;
class QDialogButtonBox;
namespace Archcodex {
std::optional<HostProfile> editSshHost(QWidget *parent, const HostProfile &host,
                                       const QMap<QString, HostProfile> &others);
class SshPromptDialog : public QDialog {
    Q_OBJECT
  public:
    explicit SshPromptDialog(QWidget *parent = nullptr);
    void setPrompt(const QString &host, const QJsonObject &prompt);
    void withdraw();
  signals:
    void answered(const QJsonObject &reply);

  private:
    QJsonObject prompt;
    QPlainTextEdit *description;
    QLineEdit *answer;
    QCheckBox *reveal;
    QDialogButtonBox *buttons;
    bool sent = false, withdrawn = false;
    void submit(bool cancelled);
};
} // namespace Archcodex
