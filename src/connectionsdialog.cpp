// SPDX-License-Identifier: GPL-3.0-or-later
#include "connectionsdialog.h"
#include <QActionGroup>
#include <QCollator>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QSet>
#include <QSettings>
#include <QSignalBlocker>
#include <QSortFilterProxyModel>
#include <QStandardItemModel>
#include <QTableView>
#include <QToolButton>
#include <QVBoxLayout>

namespace Archcodex {
namespace {
enum Role { Id = Qt::UserRole, Details, Editable, Connectable, Disconnectable };
const QStringList columns{"backend",     "configuration", "location",
                          "environment", "status",        "details"};
const QStringList headings{"Backend type",     "Configuration / instance", "Location",
                           "Environment mode", "Connection status",        "Status details"};
class ConnectionSort : public QSortFilterProxyModel {
    QCollator collator;

  public:
    explicit ConnectionSort(QObject *parent) : QSortFilterProxyModel(parent) {
        collator.setCaseSensitivity(Qt::CaseInsensitive);
    }

  protected:
    bool lessThan(const QModelIndex &a, const QModelIndex &b) const override {
        for (int column : {a.column(), 2, 1}) {
            const int order = collator.compare(a.siblingAtColumn(column).data().toString(),
                                               b.siblingAtColumn(column).data().toString());
            if (order)
                return order < 0;
        }
        return a.siblingAtColumn(0).data(Id).toString() < b.siblingAtColumn(0).data(Id).toString();
    }
};
} // namespace

ConnectionRow connectionRow(const BackendProfile &p, const ConnectionState &state,
                            const QString &location) {
    return {p.id,
            {p.isContainer() ? "myCodex" : "Codex", serverConfigurationName(p), location,
             p.isContainer() ? p.identity["stateMode"].toString("Unknown storage") : QString(),
             state.label(), state.details},
            serverDetails(p) + "\nStatus: " + state.description(),
            p.isLocal()};
}

ConnectionsDialog::ConnectionsDialog(const QString &path, QWidget *parent)
    : QDialog(parent), settingsFile(path) {
    setObjectName("connectionsDialog");
    setWindowTitle("Connections — Codex servers");
    setWindowModality(Qt::NonModal);
    setAttribute(Qt::WA_DeleteOnClose);
    resize(1100, 480);
    auto *layout = new QVBoxLayout(this);
    model = new QStandardItemModel(0, headings.size(), this);
    model->setHorizontalHeaderLabels(headings);
    sorted = new ConnectionSort(this);
    sorted->setSourceModel(model);
    table = new QTableView;
    table->setObjectName("connectionsTable");
    table->setModel(sorted);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setWordWrap(false);
    table->setTextElideMode(Qt::ElideRight);
    table->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    table->setShowGrid(false);
    table->verticalHeader()->hide();
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    table->horizontalHeader()->setMinimumSectionSize(60);
    table->setSortingEnabled(true);
    table->sortByColumn(1, Qt::AscendingOrder);
    for (int i = 0; i < headings.size(); ++i)
        table->setColumnWidth(i, fontMetrics().horizontalAdvance(headings[i] + "    ") + 24);
    table->setColumnWidth(1, 260);
    table->setColumnWidth(5, 280);
    layout->addWidget(table, 1);
    auto *toggle = new QToolButton;
    toggle->setObjectName("connectionDetailsToggle");
    toggle->setText("Details");
    toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    toggle->setCheckable(true);
    toggle->setArrowType(Qt::RightArrow);
    layout->addWidget(toggle);
    details = new QPlainTextEdit;
    details->setObjectName("connectionDetails");
    details->setReadOnly(true);
    details->setMaximumHeight(fontMetrics().height() * 8);
    details->setPlaceholderText("Select a connection to see its details.");
    details->hide();
    layout->addWidget(details);
    connect(toggle, &QToolButton::toggled, this, [this, toggle](bool expanded) {
        details->setVisible(expanded);
        toggle->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
    });
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    layout->addWidget(buttons);
    auto button = [&](const QString &label, const QString &name, auto callback) {
        auto *b = buttons->addButton(label, QDialogButtonBox::ActionRole);
        b->setObjectName(name);
        b->setAutoDefault(false);
        connect(b, &QPushButton::clicked, this, callback);
        return b;
    };
    button("Add local…", "connectionAdd", [this] { emit addRequested(); });
    edit = button("Edit…", "connectionEdit", [this] { emit editRequested(selectedId()); });
    connectButton =
        button("Connect", "connectionConnect", [this] { emit connectRequested(selectedId()); });
    disconnectButton = button("Disconnect", "connectionDisconnect",
                              [this] { emit disconnectRequested(selectedId()); });
    remove = button("Remove", "connectionRemove", [this] { emit removeRequested(selectedId()); });
    buttons->button(QDialogButtonBox::Close)->setAutoDefault(false);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
    connect(table->selectionModel(), &QItemSelectionModel::currentChanged, this,
            &ConnectionsDialog::updateSelection);
    connect(table->horizontalHeader(), &QHeaderView::sortIndicatorChanged, this, [this] {
        if (table->currentIndex().isValid())
            table->scrollTo(table->currentIndex()); // An explicit user sort, not a status update.
    });
    // Accessible through the Menu key / Shift+F10 as well as a right click.
    table->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(table, &QWidget::customContextMenuRequested, this, [this](const QPoint &point) {
        auto *menu = new QMenu(table);
        menu->setAttribute(Qt::WA_DeleteOnClose);
        auto *group = new QActionGroup(menu);
        for (int i = 0; i < headings.size(); ++i) {
            auto *action = menu->addAction("Sort by " + headings[i]);
            action->setCheckable(true);
            action->setChecked(sorted->sortColumn() == i);
            group->addAction(action);
            connect(action, &QAction::triggered, this, [this, i] {
                table->sortByColumn(i, table->horizontalHeader()->sortIndicatorOrder());
            });
        }
        menu->addSeparator();
        auto *descending = menu->addAction("Descending");
        descending->setCheckable(true);
        descending->setChecked(sorted->sortOrder() == Qt::DescendingOrder);
        connect(descending, &QAction::triggered, this, [this](bool checked) {
            table->sortByColumn(sorted->sortColumn(),
                                checked ? Qt::DescendingOrder : Qt::AscendingOrder);
        });
        menu->popup(table->viewport()->mapToGlobal(point));
    });
    QSettings settings(settingsFile, QSettings::IniFormat);
    settings.beginGroup("connectionsTable");
    restoreGeometry(settings.value("geometry").toByteArray());
    if (settings.value("columns").toStringList() == columns)
        table->horizontalHeader()->restoreState(settings.value("header").toByteArray());
    const auto column = columns.indexOf(settings.value("sortColumn", "configuration").toString());
    table->sortByColumn(column < 0 ? 1 : column, settings.value("descending", false).toBool()
                                                     ? Qt::DescendingOrder
                                                     : Qt::AscendingOrder);
    toggle->setChecked(settings.value("detailsVisible", false).toBool());
    updateSelection();
}
ConnectionsDialog::~ConnectionsDialog() {
    QSettings settings(settingsFile, QSettings::IniFormat);
    settings.beginGroup("connectionsTable");
    settings.setValue("geometry", saveGeometry());
    settings.setValue("columns", columns);
    settings.setValue("header", table->horizontalHeader()->saveState());
    settings.setValue("sortColumn", columns.value(sorted->sortColumn(), "configuration"));
    settings.setValue("descending", sorted->sortOrder() == Qt::DescendingOrder);
    settings.setValue("detailsVisible",
                      findChild<QToolButton *>("connectionDetailsToggle")->isChecked());
}
QString ConnectionsDialog::selectedId() const {
    return table->currentIndex().siblingAtColumn(0).data(Id).toString();
}
void ConnectionsDialog::updateSelection() {
    const auto current = table->currentIndex().siblingAtColumn(0);
    const bool valid = current.isValid();
    edit->setEnabled(valid && current.data(Editable).toBool());
    edit->setToolTip(valid && !current.data(Editable).toBool()
                         ? "Remote and container endpoints are managed in Connections → Hosts."
                         : QString());
    connectButton->setEnabled(valid && current.data(Connectable).toBool());
    disconnectButton->setEnabled(valid && current.data(Disconnectable).toBool());
    remove->setEnabled(valid);
    const auto text = current.data(Details).toString();
    if (details->toPlainText() != text)
        details->setPlainText(text);
}
void ConnectionsDialog::setConnections(const QList<ConnectionRow> &connections) {
    const auto selected = selectedId();
    const auto top = table->indexAt(QPoint(0, 0));
    const auto anchor = top.siblingAtColumn(0).data(Id).toString();
    const int offset = table->visualRect(top).top(), scroll = table->verticalScrollBar()->value();
    const QSignalBlocker selection(table->selectionModel());
    bool changed = false;
    auto begin = [&] {
        if (!changed) {
            sorted->setDynamicSortFilter(false);
            changed = true;
        }
    };
    QSet<QString> present;
    for (const auto &connection : connections) {
        present.insert(connection.id);
        auto *first = rows.value(connection.id);
        if (!first) {
            begin();
            QList<QStandardItem *> items;
            for (int i = 0; i < headings.size(); ++i)
                items << new QStandardItem;
            first = items.first();
            first->setData(connection.id, Id);
            model->appendRow(items);
            rows[connection.id] = first;
        }
        auto set = [&](QStandardItem *item, int role, const QVariant &value) {
            if (item->data(role) != value) {
                begin();
                item->setData(value, role);
            }
        };
        for (int i = 0; i < headings.size(); ++i) {
            auto *item = model->item(first->row(), i);
            set(item, Qt::DisplayRole, connection.cells.value(i));
            set(item, Qt::ToolTipRole,
                "<p>" + connection.cells.value(i).toHtmlEscaped().replace('\n', "<br>") + "</p>");
        }
        set(first, Details, connection.details);
        set(first, Editable, connection.editable);
        set(first, Connectable, connection.connectable);
        set(first, Disconnectable, connection.disconnectable);
    }
    for (auto it = rows.begin(); it != rows.end();) {
        if (present.contains(it.key())) {
            ++it;
        } else {
            begin();
            model->removeRow(it.value()->row());
            it = rows.erase(it);
        }
    }
    if (changed) {
        sorted->setDynamicSortFilter(true);
        if (!rows.contains(selected)) {
            table->clearSelection();
            table->setCurrentIndex({});
        } else if (selectedId() != selected) {
            table->setCurrentIndex(sorted->mapFromSource(rows[selected]->index()));
        }
        if (rows.contains(anchor)) {
            table->scrollTo(sorted->mapFromSource(rows[anchor]->index()),
                            QAbstractItemView::PositionAtTop);
            table->verticalScrollBar()->setValue(table->verticalScrollBar()->value() - offset);
        } else {
            table->verticalScrollBar()->setValue(scroll);
        }
    }
    updateSelection();
}
} // namespace Archcodex
