// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "types.h"
#include <QDialog>
#include <QHash>

class QTableView;
class QStandardItem;
class QStandardItemModel;
class QSortFilterProxyModel;
class QPlainTextEdit;
class QPushButton;
namespace Archcodex {
struct ConnectionRow {
    QString id;
    QStringList cells;
    QString details;
    bool editable = false, connectable = false, disconnectable = false;
};
ConnectionRow connectionRow(const BackendProfile &profile, const ConnectionState &state,
                            const QString &location);

class ConnectionsDialog : public QDialog {
    Q_OBJECT
  public:
    explicit ConnectionsDialog(const QString &settingsFile, QWidget *parent = nullptr);
    ~ConnectionsDialog() override;
    void setConnections(const QList<ConnectionRow> &connections);
    QString selectedId() const;
  signals:
    void addRequested();
    void editRequested(const QString &id);
    void connectRequested(const QString &id);
    void disconnectRequested(const QString &id);
    void removeRequested(const QString &id);

  private:
    void updateSelection();
    QString settingsFile;
    QTableView *table;
    QStandardItemModel *model;
    QSortFilterProxyModel *sorted;
    QHash<QString, QStandardItem *> rows;
    QPlainTextEdit *details;
    QPushButton *edit, *connectButton, *disconnectButton, *remove;
};
} // namespace Archcodex
