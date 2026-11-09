// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "discovery.h"
#include <QDialog>
class QLabel;
class QVBoxLayout;
class QScrollArea;
class QPushButton;
namespace Archcodex {
class DiscoveryDialog : public QDialog {
    Q_OBJECT
  public:
    explicit DiscoveryDialog(QWidget *parent = nullptr);
    void setReport(const DiscoveryReport &report);
  signals:
    void retryRequested();
    void addConnectionRequested();

  private:
    QLabel *heading, *context, *checked;
    QScrollArea *scroll;
    QVBoxLayout *cards;
    QPushButton *retry;
    QJsonArray displayedIssues;
    QString copyText, displayedEmptyText;
};
} // namespace Archcodex
