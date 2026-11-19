// SPDX-License-Identifier: GPL-3.0-or-later
#include "connectionsdialog.h"
#include <QAbstractItemModelTester>
#include <QHeaderView>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QSettings>
#include <QSortFilterProxyModel>
#include <QTableView>
#include <QToolButton>
#include <QtTest>
using namespace Archcodex;
namespace {
QModelIndex row(QTableView *table, const QString &id) {
    for (int i = 0; i < table->model()->rowCount(); ++i)
        if (table->model()->index(i, 0).data(Qt::UserRole).toString() == id)
            return table->model()->index(i, 0);
    return {};
}
} // namespace
class ConnectionsTests : public QObject {
    Q_OBJECT
  private slots:
    void fieldsComeFromMetadataAndKeepPunctuation() {
        BackendProfile p{"manual", "Literal · name — unchanged"};
        auto r = connectionRow(p, {}, "This Computer");
        QCOMPARE(r.cells[0], "Codex");
        QCOMPARE(r.cells[1], p.name);
        QVERIFY(r.cells[3].isEmpty());
        QVERIFY(r.editable);
        p.discovered = true;
        p.identity = {{"codexHome", "/home/alice/project · name/.codex"},
                      {"ownerHome", "/home/alice"}};
        r = connectionRow(
            p, {ConnectionState::Phase::Connected, "History unavailable: <b>literal</b> — detail"},
            "alice@server");
        QCOMPARE(r.cells[1], "~/project · name/.codex");
        QCOMPARE(r.cells[4], "Connected");
        QCOMPARE(r.cells[5], "History unavailable: <b>literal</b> — detail");
        p.kind = "mycodex";
        p.identity = {{"containerName", "container · alpha"}, {"stateMode", "Shared"}};
        r = connectionRow(p, {ConnectionState::Phase::WaitingForHost, "Authentication required"},
                          "alias · port 2200");
        QCOMPARE(r.cells.mid(0, 4),
                 (QStringList{"myCodex", "container · alpha", "alias · port 2200", "Shared"}));
        QVERIFY(!r.editable);
        QCOMPARE(r.cells[4], "Waiting for host");
    }
    void liveSortPreservesIdentityAndAvoidsResets() {
        QTemporaryDir data;
        ConnectionsDialog dialog(data.filePath("window.ini"));
        dialog.setAttribute(Qt::WA_DeleteOnClose, false);
        auto *table = dialog.findChild<QTableView *>();
        auto *proxy = qobject_cast<QSortFilterProxyModel *>(table->model());
        QAbstractItemModelTester tester(proxy,
                                        QAbstractItemModelTester::FailureReportingMode::QtTest);
        QList<ConnectionRow> records;
        for (int i = 0; i < 80; ++i) {
            BackendProfile p{QString::number(i), QString("Server %1").arg(i, 3, 10, QChar('0'))};
            auto r = connectionRow(p, {}, i % 2 ? "remote" : "This Computer");
            r.connectable = true;
            records << r;
        }
        dialog.setConnections(records);
        dialog.resize(850, 360);
        dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        table->setCurrentIndex(row(table, "60"));
        table->scrollTo(table->currentIndex());
        QCoreApplication::processEvents();
        const QPersistentModelIndex selected(table->currentIndex());
        QSignalSpy resets(proxy->sourceModel(), &QAbstractItemModel::modelReset);
        QSignalSpy updates(proxy->sourceModel(), &QAbstractItemModel::dataChanged);
        QSignalSpy layouts(proxy, &QAbstractItemModel::layoutChanged);
        const int scroll = table->verticalScrollBar()->value();
        dialog.setConnections(records);
        QCOMPARE(updates.size(), 0);
        QCOMPARE(layouts.size(), 0);
        QCOMPARE(table->verticalScrollBar()->value(), scroll);
        records[60].cells[1] = "AAA promoted";
        records[60].cells[5] = "<b>literal</b>\n" + QString(4000, 'x');
        records[60].details = records[60].cells[5];
        dialog.setConnections(records);
        QCOMPARE(selected.row(), 0);
        QCOMPARE(dialog.selectedId(), "60");
        QCOMPARE(resets.size(), 0);
        QSignalSpy requested(&dialog, &ConnectionsDialog::connectRequested);
        QTest::mouseClick(dialog.findChild<QPushButton *>("connectionConnect"), Qt::LeftButton);
        QCOMPARE(requested.size(), 1);
        QCOMPARE(requested.first().first().toString(), "60");
        QCOMPARE(dialog.findChild<QPlainTextEdit *>()->toPlainText(), records[60].details);
        for (int column = 0; column < 6; ++column) {
            table->sortByColumn(column, Qt::DescendingOrder);
            QCOMPARE(dialog.selectedId(), "60");
            QCOMPARE(table->currentIndex(), selected);
        }
        records.removeAt(60);
        dialog.setConnections(records);
        QVERIFY(!selected.isValid());
        QVERIFY(dialog.selectedId().isEmpty());
        QVERIFY(!dialog.findChild<QPushButton *>("connectionRemove")->isEnabled());
        QVERIFY(dialog.findChild<QPlainTextEdit *>()->toPlainText().isEmpty());
    }
    void dialogIsNonmodalAndRestoresPresentation() {
        QTemporaryDir data;
        const auto path = data.filePath("window.ini");
        QWidget parent;
        QLineEdit input(&parent);
        parent.show();
        QPointer<ConnectionsDialog> dialog = new ConnectionsDialog(path, &parent);
        dialog->setConnections(
            {connectionRow({"id", "Long · name — with punctuation"}, {}, "This Computer")});
        auto *table = dialog->findChild<QTableView *>();
        dialog->show();
        QVERIFY(QTest::qWaitForWindowExposed(dialog));
        QCOMPARE(dialog->windowModality(), Qt::NonModal);
        QVERIFY(!QApplication::activeModalWidget());
        input.setFocus();
        QTest::keyClicks(&input, "still usable");
        QCOMPARE(input.text(), "still usable");
        table->setColumnWidth(1, 347);
        table->sortByColumn(2, Qt::DescendingOrder);
        dialog->findChild<QToolButton *>()->setChecked(true);
        table->setCurrentIndex(table->model()->index(0, 0));
        const auto screenshots = qEnvironmentVariable("ARCHCODEX_TEST_SCREENSHOT_DIR");
        if (!screenshots.isEmpty())
            QVERIFY(dialog->grab().save(screenshots + "/connections.png"));
        auto font = dialog->font();
        font.setPointSize(18);
        dialog->setFont(font);
        dialog->resize(580, 420);
        QCoreApplication::processEvents();
        QVERIFY(table->viewport()->height() > 50);
        QVERIFY(table->horizontalScrollBar()->maximum() > 0);
        if (!screenshots.isEmpty())
            QVERIFY(dialog->grab().save(screenshots + "/connections-large-font.png"));
        dialog->close();
        QTRY_VERIFY(!dialog);
        dialog = new ConnectionsDialog(path, &parent);
        table = dialog->findChild<QTableView *>();
        QCOMPARE(table->columnWidth(1), 347);
        QCOMPARE(table->horizontalHeader()->sortIndicatorSection(), 2);
        QCOMPARE(table->horizontalHeader()->sortIndicatorOrder(), Qt::DescendingOrder);
        QVERIFY(dialog->findChild<QToolButton *>()->isChecked());
        delete dialog.data();
    }
};
QTEST_MAIN(ConnectionsTests)
#include "test_connections.moc"
