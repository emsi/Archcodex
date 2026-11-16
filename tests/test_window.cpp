// SPDX-License-Identifier: GPL-3.0-or-later
#include "commands.h"
#include "discovery.h"
#include "discoverydialog.h"
#include "fixture.h"
#include "notifications.h"
#include "opening.h"
#include "ptyprobe.h"
#include "searchbenchmark.h"
#include "sessionmodel.h"
#include "settingsdialog.h"
#include "terminal.h"
#include "testenvironment.h"
#include "updater.h"
#include "window.h"
#include "worker.h"
#include <KCommandBar>
#include <KShortcutsEditor>
#include <KSqueezedTextLabel>
#include <QAction>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDBusConnection>
#include <QDBusPendingCallWatcher>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QHelpEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProxyStyle>
#include <QPushButton>
#include <QScopeGuard>
#include <QScrollBar>
#include <QSettings>
#include <QSplitter>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStatusBar>
#include <QTabBar>
#include <QToolButton>
#include <QToolTip>
#include <QTreeView>
#include <QtTest>

namespace {
class TreeActivationStyle : public QProxyStyle {
    bool singleClick;

  public:
    explicit TreeActivationStyle(bool single) : singleClick(single) {}
    int styleHint(StyleHint hint, const QStyleOption *option, const QWidget *widget,
                  QStyleHintReturn *data) const override {
        if (hint == SH_ItemView_ActivateItemOnSingleClick)
            return singleClick;
        return QProxyStyle::styleHint(hint, option, widget, data);
    }
};
QList<QModelIndex> serverNodes(QTreeView *tree) {
    QList<QModelIndex> found;
    const auto walk = [&](auto &&self, const QModelIndex &parent) -> void {
        for (int i = 0; i < tree->model()->rowCount(parent); ++i) {
            const auto index = tree->model()->index(i, 0, parent);
            if (index.data(KindRole).toInt() == int(NodeKind::Server))
                found << index;
            else
                self(self, index);
        }
    };
    walk(walk, {});
    return found;
}
QModelIndex serverNode(QTreeView *tree, int row = 0) {
    return serverNodes(tree).value(row);
}
QModelIndex nodeWithKey(QTreeView *tree, const QString &key) {
    QModelIndex found;
    const auto walk = [&](auto &&self, const QModelIndex &parent) -> void {
        for (int i = 0; i < tree->model()->rowCount(parent); ++i) {
            const auto index = tree->model()->index(i, 0, parent);
            if (index.data(KeyRole).toString() == key)
                found = index;
            self(self, index);
        }
    };
    walk(walk, {});
    return found;
}
QStringList visibleThreads(QTreeView *tree) {
    QStringList ids;
    const auto walk = [&](auto &&self, const QModelIndex &parent) -> void {
        for (int i = 0; i < tree->model()->rowCount(parent); ++i) {
            const auto index = tree->model()->index(i, 0, parent);
            if (index.data(KindRole).toInt() == int(NodeKind::Session))
                ids << index.data(ThreadRole).toString();
            self(self, index);
        }
    };
    walk(walk, {});
    ids.sort();
    return ids;
}
class SettingsShown : public QObject {
    std::function<void(SettingsDialog *)> callback;
    bool eventFilter(QObject *object, QEvent *event) override {
        if (event->type() == QEvent::Show)
            if (auto *dialog = qobject_cast<SettingsDialog *>(object))
                QTimer::singleShot(0, dialog, [this, dialog] { callback(dialog); });
        return false;
    }

  public:
    explicit SettingsShown(std::function<void(SettingsDialog *)> callback)
        : callback(std::move(callback)) {
        qApp->installEventFilter(this);
    }
    ~SettingsShown() override {
        qApp->removeEventFilter(this);
    }
};
} // namespace

class WindowNotificationDaemon : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.Notifications")
  public:
    QStringList bodies;
    QList<uint> closed;
    uint nextId = 0;
  public slots:
    QStringList GetCapabilities() {
        return {"actions", "body", "body-markup"};
    }
    QString GetServerInformation(QString &vendor, QString &version, QString &specification) {
        vendor = "Archcodex tests";
        version = "1";
        specification = "1.2";
        return "Window fixture";
    }
    uint Notify(const QString &, uint replaces, const QString &, const QString &,
                const QString &body, const QStringList &, const QVariantMap &, int) {
        bodies << body;
        return replaces ? replaces : ++nextId;
    }
    void CloseNotification(uint id) {
        closed << id;
    }
};
class WindowTests : public QObject {
    Q_OBJECT
    TestHome home;
  private slots:
    void largeSearchPerformance_data() {
        QTest::addColumn<int>("delay");
        QTest::newRow("rapid") << 0;
        QTest::newRow("delayed") << 40;
    }
    void largeSearchPerformance() {
        QFETCH(int, delay);
        runSearchBenchmark(delay);
    }
    void searchWorkloads_data() {
        QTest::addColumn<QString>("shape");
        QTest::addColumn<int>("delay");
        for (const auto &shape : {"idle", "clustered", "distinct", "nested", "single"})
            QTest::newRow(shape) << QString(shape) << 0;
        QTest::newRow("distinct-delayed") << QString("distinct") << 40;
    }
    void searchWorkloads() {
        QFETCH(QString, shape);
        QFETCH(int, delay);
        runSearchBenchmark(delay, shape);
    }
    void singleClickOpensOnce_data() {
        QTest::addColumn<bool>("singleClickStyle");
        QTest::newRow("desktop-double-click") << false;
        QTest::newRow("desktop-single-click") << true;
    }
    void singleClickOpensOnce() {
        QFETCH(bool, singleClickStyle);
        PtyProbe probe;
        QVERIFY(probe.install());
        Fixture fixture;
        QTemporaryDir data;
        auto profile = fixture.profile();
        profile.executable = probe.executable();
        {
            Store store(data.filePath("metadata.sqlite"));
            store.save(profile);
        }
        Window window(data.path(), false);
        window.resize(1100, 700);
        window.show();
        window.activateWindow();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *tree = window.findChild<QTreeView *>();
        auto *area = window.findChild<TerminalArea *>();
        auto *style = new TreeActivationStyle(singleClickStyle);
        style->setParent(tree);
        tree->setStyle(style);
        QTRY_VERIFY(window.findChild<Backend *>());
        auto *backend = window.findChild<Backend *>();
        backend->connectServer();
        const auto key = sessionKey(profile.id, "thread-1");
        QTRY_VERIFY(nodeWithKey(tree, key).isValid());
        QTRY_VERIFY(backend->ready());
        tree->expandAll();
        const auto folder = nodeWithKey(tree, key).parent();
        QTest::mouseClick(tree->viewport(), Qt::LeftButton, {}, tree->visualRect(folder).center());
        QVERIFY(area->workspace().views.isEmpty());
        fixture.delayRead = true;
        const auto reads = fixture.readIds.size();
        QSignalSpy focused(area, &TerminalArea::sessionFocused);
        auto point = tree->visualRect(nodeWithKey(tree, key)).center();
        QTest::mouseClick(tree->viewport(), Qt::LeftButton, {}, point);
        QCOMPARE(area->currentKey(), key);
        QCOMPARE(tree->currentIndex().data(KeyRole).toString(), key);
        QCOMPARE(focused.size(), 1);
        QTRY_COMPARE(fixture.readIds.size(), reads + 1);
        // A double click consists of that first click plus this second press/release.
        QTest::mouseDClick(tree->viewport(), Qt::LeftButton, {}, point);
        QTest::mouseRelease(tree->viewport(), Qt::LeftButton, {}, point);
        QCOMPARE(focused.size(), 1);
        QCOMPARE(tree->currentIndex().data(KeyRole).toString(), key);
        QCOMPARE(area->workspace().views.size(), 1);
        fixture.delayRead = false;
        fixture.reply(fixture.readId, {{"thread", fixture.thread()}});
        QTRY_VERIFY(area->hasClient(key));
        QTRY_VERIFY(!probe.records("thread-1").isEmpty());
        QCOMPARE(area->findChildren<KParts::ReadOnlyPart *>().size(), 1);
        QCOMPARE(fixture.readIds.size(), reads + 1);
        QCOMPARE(focused.size(), 1); // Completing the open is not another visit.

        auto other = Session::fromThread(profile.id, fixture.thread("other"));
        backend->sessionChanged(other);
        const auto otherKey = sessionKey(profile.id, other.id);
        tree->expandAll();
        point = tree->visualRect(nodeWithKey(tree, otherKey)).center();
        QTest::mouseClick(tree->viewport(), Qt::RightButton, {}, point);
        QCOMPARE(area->currentKey(), key);
        QCOMPARE(focused.size(), 1);
        tree->setCurrentIndex(nodeWithKey(tree, otherKey));
        tree->setFocus();
        QTest::keyClick(tree, Qt::Key_Return);
        QCOMPARE(area->currentKey(), otherKey);
        QTRY_VERIFY(area->hasClient(otherKey));
        QCOMPARE(focused.size(), 2);
        QCOMPARE(area->workspace().views.size(), 2);
        tree->expandAll();
        point = tree->visualRect(nodeWithKey(tree, key)).center();
        QTest::mouseClick(tree->viewport(), Qt::LeftButton, {}, point);
        QCOMPARE(area->currentKey(), key);
        QCOMPARE(focused.size(), 3);
        QTest::mouseDClick(tree->viewport(), Qt::LeftButton, {}, point);
        QTest::mouseRelease(tree->viewport(), Qt::LeftButton, {}, point);
        QCOMPARE(tree->currentIndex().data(KeyRole).toString(), key);
        QCOMPARE(focused.size(), 3);
        QCOMPARE(area->findChildren<KParts::ReadOnlyPart *>().size(), 2);
        QVERIFY(area->isAncestorOf(QApplication::focusWidget()));
        window.close();
    }
    void selectionFollowsDisplayedSession() {
        PtyProbe probe;
        QVERIFY(probe.install());
        QTemporaryDir data;
        BackendProfile first{"first", "First", "local-unix", "/unused", probe.executable(), false};
        auto second = first;
        second.id = "second";
        second.name = "Second";
        {
            Store store(data.filePath("metadata.sqlite"));
            store.save(first);
            store.save(second);
        }
        Window window(data.path(), false);
        window.resize(1100, 700);
        window.show();
        window.activateWindow();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QTRY_COMPARE(window.findChildren<Backend *>().size(), 2);
        QHash<QString, Backend *> backends;
        for (auto *backend : window.findChildren<Backend *>())
            backends[backend->profile.id] = backend;
        auto *area = window.findChild<TerminalArea *>();
        auto *tree = window.findChild<QTreeView *>();
        auto *tabs = window.findChild<QTabBar *>();
        auto *query = window.findChild<QLineEdit *>("sessionSearch");
        auto a = Session::fromThread(first.id, {{"id", "same-id"},
                                                {"name", "Identical title"},
                                                {"cwd", "/work/a"},
                                                {"path", "/saved/a"},
                                                {"status", QJsonObject{{"type", "idle"}}}});
        auto b = a;
        b.backendId = second.id;
        b.status = {{"type", "active"}, {"activeFlags", QJsonArray{}}};
        auto c = a;
        c.id = "other-project";
        c.cwd = "/work/c";
        for (const auto &s : {a, b, c})
            backends[s.backendId]->sessionChanged(s);
        const auto aKey = sessionKey(a.backendId, a.id);
        const auto bKey = sessionKey(b.backendId, b.id);
        const auto cKey = sessionKey(c.backendId, c.id);
        const auto selected = [&] { return tree->currentIndex().data(KeyRole).toString(); };
        const auto revealed = [&](const QString &key) {
            const auto index = nodeWithKey(tree, key);
            QVERIFY(index.isValid());
            QCOMPARE(selected(), key);
            QCOMPARE(tree->selectionModel()->selectedRows(), QList<QModelIndex>{index});
            for (auto parent = index.parent(); parent.isValid(); parent = parent.parent())
                QVERIFY(tree->isExpanded(parent));
        };
        tree->collapseAll();
        QString error;
        QVERIFY2(area->open(first, a, &error), qPrintable(error));
        revealed(aKey);
        const auto pending = area->prepare(second, b, false);
        const auto pendingOther = area->prepare(first, c, false);
        QCOMPARE(area->currentKey(), aKey);
        revealed(aKey);
        QSignalSpy focused(area, &TerminalArea::sessionFocused);
        QSignalSpy changed(area, &TerminalArea::currentSessionChanged);
        const auto recent = area->workspace().recent;
        auto *focus = QApplication::focusWidget();
        QVERIFY2(area->startPrepared(pending.id, second, b, &error), qPrintable(error));
        QVERIFY2(area->startPrepared(pendingOther.id, first, c, &error), qPrintable(error));
        QTRY_VERIFY(!probe.records(b.id).isEmpty());
        QCOMPARE(QApplication::focusWidget(), focus);
        QCOMPARE(area->workspace().recent, recent);
        QCOMPARE(focused.size(), 0);
        QCOMPARE(changed.size(), 0);
        revealed(aKey);

        const auto cycle = [&](int key, bool reverse, bool commit, const QString &target) {
            const auto origin = area->currentKey();
            const auto history = area->workspace().recent;
            const auto modifiers =
                Qt::ControlModifier | (reverse ? Qt::ShiftModifier : Qt::NoModifier);
            QKeyEvent shortcut(QEvent::ShortcutOverride, key, modifiers);
            QKeyEvent press(QEvent::KeyPress, key, modifiers);
            QApplication::sendEvent(QApplication::focusWidget(), &shortcut);
            QApplication::sendEvent(QApplication::focusWidget(), &press);
            QCOMPARE(area->currentKey(), origin);
            QCOMPARE(selected(), origin);
            QCOMPARE(area->workspace().recent, history);
            if (!commit)
                QTest::keyClick(QApplication::focusWidget(), Qt::Key_Escape);
            QKeyEvent release(QEvent::KeyRelease, Qt::Key_Control, Qt::NoModifier);
            QApplication::sendEvent(QApplication::focusWidget(), &release);
            QCOMPARE(area->currentKey(), target);
            revealed(target);
        };
        cycle(Qt::Key_Tab, false, false, aKey);
        QCOMPARE(focused.size(), 0);
        cycle(Qt::Key_Tab, false, true, bKey);
        cycle(Qt::Key_Tab, true, true, cKey);
        cycle(Qt::Key_QuoteLeft, false, true, bKey);
        cycle(Qt::Key_QuoteLeft, true, true, aKey);
        QCOMPARE(focused.size(), 4);
        QCOMPARE(changed.size(), 4);
        const auto visits = area->workspace().recent;
        focused.clear();
        changed.clear();

        // Each tab keeps its filter; hidden/missing current sessions leave no
        // other backend's identically named/numbered session selected.
        tabs->setCurrentIndex(int(SessionView::Running));
        QVERIFY(nodeWithKey(tree, bKey).isValid());
        QVERIFY(!tree->currentIndex().isValid());
        QVERIFY(tree->selectionModel()->selectedRows().isEmpty());
        tabs->setCurrentIndex(int(SessionView::Waiting));
        revealed(aKey);
        query->setFocus();
        query->setText("late title");
        QVERIFY(!tree->currentIndex().isValid());
        auto neighbour = a;
        neighbour.id = "matching-neighbour";
        neighbour.title = "Late title neighbour";
        backends[first.id]->sessionChanged(neighbour);
        QVERIFY(!tree->currentIndex().isValid());
        a.title = "Late title";
        backends[first.id]->sessionChanged(a);
        revealed(aKey);
        QCOMPARE(QApplication::focusWidget(), query);
        QCOMPARE(query->text(), "late title");
        backends[first.id]->sessionRemoved(neighbour.id);
        a.status = {{"type", "active"}, {"activeFlags", QJsonArray{}}};
        backends[first.id]->sessionChanged(a);
        QVERIFY(!tree->currentIndex().isValid());
        tabs->setCurrentIndex(int(SessionView::Running));
        revealed(aKey);
        tabs->setCurrentIndex(int(SessionView::Opened));
        revealed(aKey);
        query->setText("other-project");
        QVERIFY(!tree->currentIndex().isValid());
        QCOMPARE(visibleThreads(tree), QStringList{c.id});
        query->clear();
        revealed(aKey);
        tabs->setCurrentIndex(int(SessionView::Waiting));
        QCOMPARE(query->text(), "late title");
        QVERIFY(!tree->currentIndex().isValid());
        tabs->setCurrentIndex(int(SessionView::All));
        revealed(aKey);
        tree->collapseAll();
        a.cwd = "/elsewhere/deep/project";
        backends[first.id]->sessionChanged(a);
        revealed(aKey);
        b.recency = QDateTime::currentSecsSinceEpoch() + 100;
        backends[second.id]->sessionChanged(b);
        revealed(aKey);
        auto *model = qobject_cast<SessionModel *>(
            qobject_cast<QSortFilterProxyModel *>(tree->model())->sourceModel());
        QVERIFY(model);
        model->remove(a.backendId, a.id);
        QVERIFY(!tree->currentIndex().isValid());
        QVERIFY(nodeWithKey(tree, bKey).isValid());
        backends[first.id]->sessionChanged(a);
        revealed(aKey);
        QCOMPARE(focused.size(), 0);
        QCOMPARE(changed.size(), 0);
        QCOMPARE(area->workspace().recent, visits);
        area->closeSession(bKey);
        revealed(aKey);
        area->closeCurrent();
        QVERIFY(area->currentKey().isEmpty());
        QVERIFY(!tree->currentIndex().isValid());
        QVERIFY(tree->selectionModel()->selectedRows().isEmpty());
        tabs->setCurrentIndex(int(SessionView::Opened));
        QVERIFY(!tree->currentIndex().isValid());
        QCOMPARE(changed.size(), 1);
        QCOMPARE(changed.first().first().toString(), QString());
        window.close();
    }
    void terminalSwitchingPreservesPtySize() {
        PtyProbe probe;
        QVERIFY(probe.install());
        QTemporaryDir data;
        BackendProfile p{"pty",
                         "myCodex · long-container-name · remote.example.test · Shared",
                         "local-unix",
                         "/unused",
                         probe.executable(),
                         false};
        {
            Store store(data.filePath("metadata.sqlite"));
            store.save(p);
        }
        Window window(data.path(), false);
        window.resize(1100, 700);
        window.show();
        window.activateWindow();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QTRY_VERIFY(window.findChild<Backend *>());
        auto *area = window.findChild<TerminalArea *>();
        auto *tree = window.findChild<QTreeView *>();
        auto *tabs = window.findChild<QTabBar *>();
        tabs->setCurrentIndex(int(SessionView::Opened));
        auto a = Session::fromThread(p.id, {{"id", "short"}, {"name", "Short"}, {"cwd", "/work"}});
        auto b = Session::fromThread(p.id,
                                     {{"id", "long"},
                                      {"name", QString("A long session title ").repeated(12)},
                                      {"cwd", "/work/" + QString("nested-project/").repeated(10)}});
        QString error;
        QVERIFY2(area->open(p, a, &error), qPrintable(error));
        auto *first = area->findChildren<KParts::ReadOnlyPart *>().last();
        QTRY_VERIFY(!probe.records(a.id).isEmpty());
        QVERIFY2(area->open(p, b, &error), qPrintable(error));
        auto *second = area->findChildren<KParts::ReadOnlyPart *>().last();
        QTRY_VERIFY(!probe.records(b.id).isEmpty());
        // Let queued Konsole display/layout work finish, then acknowledge input
        // through each PTY. This bounded observation period is test-only.
        const auto checkpoint = [&](KParts::ReadOnlyPart *part, const QString &id) {
            QTest::qWait(100);
            const auto count = probe.records(id).size();
            PtyProbe::send(part);
            QTRY_VERIFY(probe.records(id).size() > count);
            QCOMPARE(probe.records(id).last().toObject()["event"].toString(), "checkpoint");
        };
        checkpoint(first, a.id);
        checkpoint(second, b.id);
        const auto aSize = PtyProbe::size(probe.records(a.id).last());
        const auto bSize = PtyProbe::size(probe.records(b.id).last());
        QVERIFY(aSize.width() > 10 && aSize.height() > 5);
        for (const auto &id : {a.id, b.id})
            for (const auto &record : probe.records(id)) {
                QCOMPARE(PtyProbe::size(record), aSize);
                QVERIFY(record.toObject()["event"] != "resize");
            }
        const auto unchanged = [&](const QString &id, qsizetype since, QSize size) {
            const auto records = probe.records(id);
            for (qsizetype i = since; i < records.size(); ++i) {
                QCOMPARE(PtyProbe::size(records[i]), size);
                QVERIFY2(records[i].toObject()["event"] != "resize",
                         QJsonDocument(records).toJson(QJsonDocument::Compact).constData());
            }
        };
        QSignalSpy focused(area, &TerminalArea::sessionFocused);
        for (int i = 0; i < 4; ++i) {
            const auto aCount = probe.records(a.id).size();
            const auto bCount = probe.records(b.id).size();
            // Projects activation must reuse the terminal, select its row and
            // record one visit; holding Ctrl+Tab must leave that view untouched.
            const auto index = nodeWithKey(tree, sessionKey(p.id, a.id));
            QVERIFY(index.isValid());
            tree->setCurrentIndex(index);
            tree->setFocus();
            QTest::keyClick(tree, Qt::Key_Return);
            QCOMPARE(area->currentKey(), sessionKey(p.id, a.id));
            QTRY_VERIFY(area->isAncestorOf(QApplication::focusWidget()));
            checkpoint(first, a.id);
            const auto recent = area->workspace().recent;
            QKeyEvent tab(QEvent::KeyPress, Qt::Key_Tab, Qt::ControlModifier);
            QKeyEvent shortcut(QEvent::ShortcutOverride, Qt::Key_Tab, Qt::ControlModifier);
            QApplication::sendEvent(QApplication::focusWidget(), &shortcut);
            QApplication::sendEvent(QApplication::focusWidget(), &tab);
            QCOMPARE(area->currentKey(), sessionKey(p.id, a.id));
            QCOMPARE(area->workspace().recent, recent);
            QKeyEvent release(QEvent::KeyRelease, Qt::Key_Control, Qt::NoModifier);
            QApplication::sendEvent(QApplication::focusWidget(), &release);
            QCOMPARE(area->currentKey(), sessionKey(p.id, b.id));
            QCOMPARE(tree->currentIndex().data(KeyRole).toString(), sessionKey(p.id, b.id));
            QCOMPARE(area->workspace().recent.first(), area->view(sessionKey(p.id, b.id)).id);
            checkpoint(second, b.id);
            unchanged(a.id, aCount, aSize);
            unchanged(b.id, bCount, bSize);
        }
        QCOMPARE(focused.size(), 8);
        QCOMPARE(area->findChildren<KParts::ReadOnlyPart *>().size(), 2);
        QCOMPARE(aSize, bSize);
        auto *title = window.findChild<KSqueezedTextLabel *>("terminalTitle");
        QVERIFY(title);
        const auto fullTitle = b.title + "  ·  " + p.name + "  ·  " + b.cwd;
        QCOMPARE(title->fullText(), fullTitle);
        QCOMPARE(title->toolTip(), fullTitle);
        QCOMPARE(title->accessibleName(), fullTitle);
        QVERIFY(title->isSqueezed());
        const auto beforeRename = probe.records(b.id).size();
        b.title = "Renamed <b>literally</b>\nwith a second line\u2028and another";
        area->updateSession(b, p.name);
        checkpoint(second, b.id);
        unchanged(b.id, beforeRename, bSize);
        QVERIFY(!title->fullText().contains('\n'));
        QVERIFY(title->accessibleName().contains(b.title));
        QHelpEvent help(QEvent::ToolTip, QPoint(4, 4), title->mapToGlobal(QPoint(4, 4)));
        QApplication::sendEvent(title, &help);
        QVERIFY(QToolTip::text().contains("&lt;b&gt;literally&lt;/b&gt;"));
        QVERIFY(QToolTip::text().contains(b.cwd));
        QToolTip::hideText();

        // Real window and sidebar resizing still reaches the child.
        window.resize(window.width() + 160, window.height() + 100);
        checkpoint(second, b.id);
        auto larger = PtyProbe::size(probe.records(b.id).last());
        QVERIFY(larger.width() > bSize.width() && larger.height() > bSize.height());
        auto *splitter = window.findChild<QSplitter *>();
        auto sizes = splitter->sizes();
        sizes[0] += 100;
        sizes[1] -= 100;
        splitter->setSizes(sizes);
        checkpoint(second, b.id);
        QVERIFY(PtyProbe::size(probe.records(b.id).last()).width() < larger.width());
        // A changed font must also reflow normally, not use a fixed pixel height.
        auto font = title->font();
        font.setPointSize(font.pointSize() + 8);
        title->setFont(font);
        checkpoint(second, b.id);
        QVERIFY(title->height() >= title->fontMetrics().height());
        const auto resized = PtyProbe::size(probe.records(b.id).last());
        QVERIFY(area->focusSession(sessionKey(p.id, a.id)));
        checkpoint(first, a.id);
        QCOMPARE(PtyProbe::size(probe.records(a.id).last()), resized);
        const auto screenshots = qEnvironmentVariable("ARCHCODEX_TEST_SCREENSHOT_DIR");
        if (!screenshots.isEmpty())
            QVERIFY(window.grab().save(screenshots + "/terminal-header.png"));
        area->closeBackend(p.id);
        window.close();
    }
    void onlyTheViewedTerminalIsSuppressed() {
        auto bus = QDBusConnection::connectToBus(QDBusConnection::SessionBus,
                                                 "window-notification-fixture");
        WindowNotificationDaemon desktop;
        QVERIFY(bus.registerService("org.freedesktop.Notifications"));
        QVERIFY(bus.registerObject("/org/freedesktop/Notifications", &desktop,
                                   QDBusConnection::ExportAllSlots));
        const auto unregister = qScopeGuard([&] {
            bus.unregisterObject("/org/freedesktop/Notifications");
            bus.unregisterService("org.freedesktop.Notifications");
            QDBusConnection::disconnectFromBus("window-notification-fixture");
        });
        const auto delivered = [&] {
            // Receiving Notify in the fixture precedes the client's delivery
            // acknowledgement. Fence that reply before simulating a later visit.
            QDBusPendingCallWatcher barrier(QDBusConnection::sessionBus().asyncCall(
                QDBusMessage::createMethodCall("org.freedesktop.Notifications",
                                               "/org/freedesktop/Notifications",
                                               "org.freedesktop.DBus.Peer", "Ping")));
            QTRY_VERIFY(barrier.isFinished());
            QVERIFY2(!barrier.isError(), qPrintable(barrier.error().message()));
        };
        QTemporaryDir data;
        Fixture fixture;
        auto p = fixture.profile();
        {
            Store store(data.filePath("metadata.sqlite"));
            store.save(p);
        }
        Window window(data.path(), false);
        window.show();
        QTRY_VERIFY(window.findChild<Backend *>());
        auto *backend = window.findChild<Backend *>();
        QVERIFY(backend);
        backend->connectServer();
        QTRY_VERIFY(backend->ready());
        auto *area = window.findChild<TerminalArea *>();
        const auto first = Session::fromThread(p.id, fixture.thread());
        auto second = Session::fromThread(p.id, fixture.thread("second"));
        second.title = "Second conversation";
        area->prepare(p, first, true);
        area->prepare(p, second, false);
        window.activateWindow();
        QTRY_VERIFY(window.isActiveWindow());
        emit backend->attentionRequested({first, SessionAttention::Reason::Waiting});
        emit backend->attentionRequested({second, SessionAttention::Reason::Waiting});
        QTRY_COMPARE(desktop.bodies.size(), 1);
        delivered();
        QVERIFY(desktop.bodies.first().contains(second.title));
        area->focusSession(sessionKey(p.id, second.id));
        QTRY_COMPARE(desktop.closed.size(), 1);
        emit backend->attentionRequested({second, SessionAttention::Reason::Waiting});
        QTest::qWait(30);
        QCOMPARE(desktop.bodies.size(), 1);
        QWidget other;
        other.show();
        other.activateWindow();
        QTRY_VERIFY(!window.isActiveWindow());
        emit backend->attentionRequested({second, SessionAttention::Reason::Waiting});
        QTRY_COMPARE(desktop.bodies.size(), 2);
        delivered();
        window.activateWindow();
        QTRY_VERIFY(window.isActiveWindow());
        QTRY_COMPARE(desktop.closed.size(), 2);
        window.showMinimized();
        emit backend->attentionRequested({second, SessionAttention::Reason::Waiting});
        QTRY_COMPARE(desktop.bodies.size(), 3);
        delivered();
    }
    void notificationClicksRouteByIdentityAcrossFiltersAndHandleStaleTargets() {
        QTemporaryDir data;
        Fixture first, second;
        auto a = first.profile(), b = second.profile();
        b.id = "other-backend";
        a.executable = b.executable = "/bin/true";
        {
            Store store(data.filePath("metadata.sqlite"));
            store.save(a);
            store.save(b);
        }
        Window window(data.path(), false);
        window.show();
        QTRY_COMPARE(window.findChildren<Backend *>().size(), 2);
        for (auto *backend : window.findChildren<Backend *>())
            backend->connectServer();
        QTRY_VERIFY(first.methods.contains("thread/read") &&
                    second.methods.contains("thread/read"));
        auto *notifications = window.findChild<Notifications *>();
        auto *area = window.findChild<TerminalArea *>();
        auto *tabs = window.findChild<QTabBar *>();
        auto *tree = window.findChild<QTreeView *>();
        QTRY_COMPARE(visibleThreads(tree).size(), 2);
        tabs->setCurrentIndex(int(SessionView::Running)); // Both sessions are hidden.
        QCOMPARE(visibleThreads(tree).size(), 0);
        emit notifications->openRequested(b.id, "thread-1", {});
        QTRY_COMPARE(area->currentKey(), sessionKey(b.id, "thread-1"));
        QVERIFY(!tree->currentIndex().isValid());
        QCOMPARE(tabs->currentIndex(), int(SessionView::Running));
        tabs->setCurrentIndex(int(SessionView::All));
        QCOMPARE(tree->currentIndex().data(KeyRole).toString(), area->currentKey());
        const auto existing = area->view(area->currentKey()).id;
        emit notifications->openRequested(a.id, "thread-1", {});
        QTRY_COMPARE(area->currentKey(), sessionKey(a.id, "thread-1"));
        QCOMPARE(tree->currentIndex().data(KeyRole).toString(), area->currentKey());
        emit notifications->openRequested(b.id, "thread-1", {});
        QCOMPARE(area->view(area->currentKey()).id, existing);
        QCOMPARE(area->workspace().views.size(), 2);
        // All's asynchronous results are a separate model. An empty/partial
        // result must not keep a stale selection or select another backend's ID.
        QTRY_COMPARE(area->findChildren<KParts::ReadOnlyPart *>().size(), 2);
        window.activateWindow();
        QTRY_VERIFY(window.isActiveWindow());
        first.delaySearch = second.delaySearch = true;
        auto *query = window.findChild<QLineEdit *>("sessionSearch");
        query->setFocus();
        QCOMPARE(QApplication::focusWidget(), query);
        query->setText("Example");
        QTest::keyClick(query, Qt::Key_Return);
        QVERIFY(!tree->currentIndex().isValid());
        QTRY_COMPARE(first.searchIds.size(), 1);
        QTRY_COMPARE(second.searchIds.size(), 1);
        first.reply(first.searchIds.first(),
                    {{"data", QJsonArray{QJsonObject{{"thread", first.thread()}}}},
                     {"nextCursor", QJsonValue::Null}});
        QTRY_COMPARE(visibleThreads(tree).size(), 1);
        QVERIFY(!tree->currentIndex().isValid());
        second.reply(second.searchIds.first(),
                     {{"data", QJsonArray{QJsonObject{{"thread", second.thread()}}}},
                      {"nextCursor", QJsonValue::Null}});
        QTRY_COMPARE(tree->currentIndex().data(KeyRole).toString(), area->currentKey());
        QCOMPARE(QApplication::focusWidget(), query);
        QCOMPARE(query->text(), "Example");
        query->clear();
        QCOMPARE(tree->currentIndex().data(KeyRole).toString(), area->currentKey());
        QCOMPARE(area->workspace().views.size(), 2);
        emit notifications->openRequested("removed-backend", "thread-1", {});
        QVERIFY(window.statusBar()->currentMessage().contains("unavailable"));
        QCOMPARE(area->workspace().views.size(), 2);
        first.readErrors["removed-session"] = "thread not found: removed-session";
        emit notifications->openRequested(a.id, "removed-session", {});
        QTRY_VERIFY(area->currentSession().clientIssue.contains("thread not found"));
        QVERIFY(first.readIds.contains("removed-session"));
        QCOMPARE(area->workspace().views.size(), 3); // Keep the verification error visible.
        QVERIFY(!area->hasClient(sessionKey(a.id, "removed-session")));
        window.close();
    }
    void notificationClickBeforeMetadataVerifiesTheSession_data() {
        QTest::addColumn<QString>("failure");
        QTest::newRow("available") << QString{};
        QTest::newRow("removed") << "thread not found";
        QTest::newRow("wrong-identity") << "Server returned metadata for a different session";
    }
    void notificationClickBeforeMetadataVerifiesTheSession() {
        QFETCH(QString, failure);
        QTemporaryDir data;
        Fixture fixture;
        fixture.delaySnapshot = fixture.delayRead = true;
        auto p = fixture.profile();
        p.executable = "/bin/true";
        {
            Store store(data.filePath("metadata.sqlite"));
            store.save(p);
        }
        Window window(data.path(), false);
        window.show();
        QTRY_VERIFY(window.findChild<Backend *>());
        auto *backend = window.findChild<Backend *>();
        auto *area = window.findChild<TerminalArea *>();
        auto *notifications = window.findChild<Notifications *>();
        auto *tree = window.findChild<QTreeView *>();
        QSignalSpy attention(backend, &Backend::attentionRequested);
        QSignalSpy launches(window.findChild<OpeningCoordinator *>(),
                            &OpeningCoordinator::launchRequested);
        backend->connectServer();
        QTRY_VERIFY(!fixture.snapshotId.isUndefined());
        QVERIFY(backend->ready());
        const QString id = "live-session";
        const auto key = sessionKey(p.id, id);
        fixture.notify(
            "thread/status/changed",
            {{"threadId", id},
             {"status", QJsonObject{{"type", "active"}, {"activeFlags", QJsonArray{}}}}});
        fixture.notify("thread/status/changed",
                       {{"threadId", id}, {"status", QJsonObject{{"type", "idle"}}}});
        QTRY_COMPARE(fixture.readIds, QStringList{id});
        QCOMPARE(attention.size(), 0); // New alerts wait for classification now.
        const auto metadataRead = fixture.readId;
        QVERIFY(visibleThreads(window.findChild<QTreeView *>()).isEmpty());
        QVERIFY(area->workspace().views.isEmpty());

        // A previously delivered action must still verify its exact target even
        // when the GUI has no cached metadata. Keep this routing regression separate
        // from the backend's new notification eligibility check.
        emit notifications->openRequested(p.id, id, {});
        QTRY_COMPARE(area->workspace().views.size(), 1);
        QTRY_COMPARE(fixture.readIds, (QStringList{id, id}));
        const auto verificationRead = fixture.readId;
        QVERIFY(verificationRead != metadataRead);
        QCOMPARE(area->currentKey(), key);
        QCOMPARE(tree->currentIndex().data(KeyRole).toString(), key);
        const auto viewId = area->view(key).id;
        QCOMPARE(launches.size(), 0);
        QVERIFY(area->findChildren<KParts::ReadOnlyPart *>().isEmpty());
        emit notifications->openRequested(p.id, id, {}); // Reuse the pending verification.
        QCOMPARE(area->view(key).id, viewId);

        if (failure == "thread not found")
            fixture.reject(verificationRead, failure);
        else
            fixture.reply(verificationRead,
                          {{"thread", fixture.thread(failure.isEmpty() ? id : "another-session")}});
        if (failure.isEmpty()) {
            QTRY_COMPARE(launches.size(), 1);
            const auto verified = qvariant_cast<Session>(launches.first().at(2));
            QCOMPARE(verified.id, id);
            QCOMPARE(verified.backendId, p.id);
            QCOMPARE(verified.cwd, "/work/project");
            QCOMPARE(area->view(key).title, "Example");
            QCOMPARE(area->view(key).cwd, "/work/project");
            QCOMPARE(tree->currentIndex().data(KeyRole).toString(), key);
        } else {
            QTRY_VERIFY(area->currentSession().clientIssue.contains(failure));
            QCOMPARE(launches.size(), 0);
            QCOMPARE(area->view(key).disposition, ViewDisposition::Disconnected);
            QVERIFY(area->findChildren<KParts::ReadOnlyPart *>().isEmpty());
        }
        emit notifications->openRequested(p.id, id, {});
        QCOMPARE(area->view(key).id, viewId);
        QCOMPARE(area->workspace().views.size(), 1);
        QCOMPARE(fixture.readIds, (QStringList{id, id}));
        QVERIFY(backend->ready());
        window.close();
    }
    void contextMenuNewTracksRenameStatusAndRestoresTheSameConversation() {
        QTemporaryDir data;
        Fixture fixture;
        auto p = fixture.profile();
        QFile script(data.filePath("client"));
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write(
            "#!/usr/bin/python3\nimport json,sys,time\n"
            "open(__file__+'.args','w').write(json.dumps(sys.argv[1:]))\ntime.sleep(30)\n");
        script.close();
        QVERIFY(script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        p.executable = script.fileName();
        p.identity["codexHome"] = "/home/test/.codex-work";
        {
            Store store(data.filePath("metadata.sqlite"));
            store.save(p);
        }
        const auto key = sessionKey(p.id, "created-1");
        QString viewId;
        {
            Window window(data.path(), false);
            window.show();
            QTRY_VERIFY(window.findChild<Backend *>());
            auto *backend = window.findChild<Backend *>();
            QVERIFY(backend);
            backend->connectServer();
            auto *tree = window.findChild<QTreeView *>();
            auto *tabs = window.findChild<QTabBar *>();
            auto *area = window.findChild<TerminalArea *>();
            QTRY_VERIFY(nodeWithKey(tree, sessionKey(p.id, "thread-1")).isValid());
            tree->expandAll();
            const auto item = nodeWithKey(tree, sessionKey(p.id, "thread-1"));
            tree->scrollTo(item);
            tree->customContextMenuRequested(tree->visualRect(item).center());
            QPointer<QMenu> menu = tree->findChild<QMenu *>("sessionContextMenu");
            QVERIFY(menu);
            QCOMPARE(menu->actions()[1]->text(), "New");
            menu->actions()[1]->trigger();
            if (menu) {
                menu->hide();
                delete menu;
            }
            QTRY_VERIFY(area->hasClient(key));
            QCOMPARE(area->currentKey(), key);
            QCOMPARE(area->workspace().views.size(), 1);
            QCOMPARE(area->workspace().views.first().threadId, "created-1");
            viewId = area->workspace().views.first().id;
            QCOMPARE(visibleThreads(tree), (QStringList{"created-1", "thread-1"}));
            QTRY_COMPARE(nodeWithKey(tree, key).data().toString(), "New session");
            QTRY_VERIFY(QFile::exists(script.fileName() + ".args"));
            QFile arguments(script.fileName() + ".args");
            QVERIFY(arguments.open(QIODevice::ReadOnly));
            const auto args = QJsonDocument::fromJson(arguments.readAll()).array();
            QCOMPARE(args, (QJsonArray{"--remote", p.endpoint(), "resume", "--", "created-1"}));
            fixture.createdThreads["created-1"]["name"] = "Renamed in the TUI";
            fixture.notify("thread/name/updated",
                           {{"threadId", "created-1"}, {"threadName", "Renamed in the TUI"}});
            QTRY_COMPARE(nodeWithKey(tree, key).data().toString(), "Renamed in the TUI");
            QTRY_COMPARE(area->workspace().views.first().title, "Renamed in the TUI");
            fixture.notify(
                "thread/status/changed",
                {{"threadId", "created-1"},
                 {"status", QJsonObject{{"type", "active"}, {"activeFlags", QJsonArray{}}}}});
            tabs->setCurrentIndex(int(SessionView::Running));
            QTRY_COMPARE(visibleThreads(tree), QStringList{"created-1"});
            QTRY_VERIFY(area->currentSession().isRunning());
            fixture.notify("thread/status/changed",
                           {{"threadId", "created-1"}, {"status", QJsonObject{{"type", "idle"}}}});
            QTRY_VERIFY(visibleThreads(tree).isEmpty());
            for (auto tab : {SessionView::Waiting, SessionView::Active, SessionView::Opened}) {
                tabs->setCurrentIndex(int(tab));
                QTRY_VERIFY(visibleThreads(tree).contains("created-1"));
            }
            QTRY_VERIFY(area->currentSession().isWaiting());
            window.close();
        }
        QVERIFY(Worker::finishPending());
        QVERIFY(QFile::remove(script.fileName() + ".args"));
        {
            Store store(data.filePath("metadata.sqlite"));
            const auto saved = store.workspace();
            QCOMPARE(saved.views.size(), 1);
            QCOMPARE(saved.views.first().threadId, "created-1");
            QCOMPARE(saved.views.first().title, "Renamed in the TUI");
            QCOMPARE(saved.views.first().disposition, ViewDisposition::Automatic);
        }
        {
            Window window(data.path(), false);
            window.show();
            auto *area = window.findChild<TerminalArea *>();
            QTRY_VERIFY(window.findChild<Backend *>());
            auto *backend = window.findChild<Backend *>();
            backend->connectServer();
            QTRY_VERIFY(area->hasClient(key));
            QTRY_VERIFY(QFile::exists(script.fileName() + ".args"));
            QCOMPARE(area->workspace().views.first().id, viewId);
            QCOMPARE(fixture.starts.size(),
                     1); // Restore must not create a replacement conversation.
            auto session = area->currentSession();
            area->closeCurrent();
            backend->sessionStarted(1, session,
                                    {}); // A duplicate completion cannot reopen a closed view.
            QVERIFY(area->workspace().views.isEmpty());
            window.close();
        }
        QVERIFY(Worker::finishPending());
    }
    void restoreWorkspaceFromExactIdsAndPreserveOfflineIntent() {
        QTemporaryDir data;
        Fixture fixture;
        auto p = fixture.profile();
        QFile script(data.filePath("client"));
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write("#!/usr/bin/python3\nimport time\ntime.sleep(30)\n");
        script.close();
        QVERIFY(script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        p.executable = script.fileName();
        p.identity["codexHome"] = "/home/test/.codex-work";
        WorkspaceView v;
        v.id = "stable-view";
        v.backendId = p.id;
        v.threadId = "beyond-first-page";
        v.title = "Remembered";
        v.cwd = "/work/project";
        v.sourceIdentity = workspaceSourceIdentity(p);
        WorkspaceSnapshot saved{1, {v}, {v.id}, v.id};
        {
            Store store(data.filePath("metadata.sqlite"));
            store.save(p);
            store.save(saved);
            auto cached = Session::fromThread(p.id, fixture.thread(v.threadId));
            cached.opened = 123;
            store.save(cached);
        }
        {
            Window window(data.path(), false);
            window.show();
            auto *area = window.findChild<TerminalArea *>();
            QSignalSpy focused(area, &TerminalArea::sessionFocused);
            QTRY_COMPARE(area->workspace().views.size(), 1);
            QCOMPARE(area->findChildren<KParts::ReadOnlyPart *>().size(), 0);
            QCOMPARE(area->currentKey(), v.key());
            auto *tree = window.findChild<QTreeView *>();
            QCOMPARE(tree->currentIndex().data(KeyRole).toString(), v.key());
            auto *tabs = window.findChild<QTabBar *>();
            tabs->setCurrentIndex(int(SessionView::Opened));
            QTRY_COMPARE(visibleThreads(tree), QStringList{v.threadId});
            QCOMPARE(tree->currentIndex().data(KeyRole).toString(), v.key());
            const auto screenshots = qEnvironmentVariable("ARCHCODEX_TEST_SCREENSHOT_DIR");
            if (!screenshots.isEmpty())
                QVERIFY(window.grab().save(screenshots + "/workspace-placeholder.png"));
            auto *backend = window.findChild<Backend *>();
            QVERIFY(backend);
            backend->connectServer();
            QTRY_VERIFY(area->hasClient(v.key()));
            QCOMPARE(tree->currentIndex().data(KeyRole).toString(), v.key());
            QVERIFY(fixture.readIds.contains(v.threadId));
            QCOMPARE(focused.size(), 0);
            QCOMPARE(area->workspace().recent, saved.recent);
            QCOMPARE(area->currentSession().opened, 123);
            fixture.notify("thread/name/updated",
                           {{"threadId", v.threadId}, {"threadName", "Live rename"}});
            QTRY_COMPARE(area->workspace().views.first().title, "Live rename");
            window.close();
        }
        QVERIFY(Worker::finishPending());
        {
            Store store(data.filePath("metadata.sqlite"));
            saved = store.workspace();
            QCOMPARE(saved.views.size(), 1);
            QCOMPARE(saved.views.first().disposition, ViewDisposition::Automatic);
            QCOMPARE(saved.views.first().title, "Live rename");
        }
        {
            Window window(data.path(), false);
            auto *area = window.findChild<TerminalArea *>();
            QTRY_COMPARE(area->workspace().views.size(), 1);
            QCOMPARE(area->workspace().views.first().id, v.id);
            QCOMPARE(area->findChildren<KParts::ReadOnlyPart *>().size(), 0);
            window.close(); // Still offline; partial startup must preserve intent.
        }
        QVERIFY(Worker::finishPending());
        {
            Store store(data.filePath("metadata.sqlite"));
            QCOMPARE(store.workspace().views.first().id, v.id);
            QCOMPARE(store.workspace().views.first().disposition, ViewDisposition::Automatic);
        }
        QSettings settings(data.filePath("window.ini"), QSettings::IniFormat);
        settings.setValue("application/restoreOpenedTerminals", false);
        settings.sync();
        {
            Window window(data.path(), false);
            QTRY_VERIFY(window.findChild<Backend *>());
            QCOMPARE(window.findChild<TerminalArea *>()->workspace().views.size(), 0);
        }
        QVERIFY(Worker::finishPending());
    }
    void keyboardNavigationPaletteAndContextMenus() {
        QTemporaryDir data;
        {
            Store store(data.path() + "/metadata.sqlite");
            store.save(BackendProfile{"b", "Main"});
        }
        Window window(data.path(), false);
        window.show();
        window.activateWindow();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QTRY_VERIFY(window.findChild<Backend *>());
        auto *backend = window.findChild<Backend *>();
        for (const auto &id : {"waiting", "running"}) {
            auto s = Session::fromThread(
                "b", {{"id", id},
                      {"name", id},
                      {"cwd", "/work/" + QString(id)},
                      {"path", "/saved/" + QString(id)},
                      {"status", QJsonObject{{"type", QString(id) == "waiting" ? "idle" : "active"},
                                             {"activeFlags", QJsonArray{}}}}});
            backend->sessionChanged(s);
        }
        auto *tree = window.findChild<QTreeView *>();
        auto *query = window.findChild<QLineEdit *>("sessionSearch");
        auto *tabs = window.findChild<QTabBar *>();
        auto *terminal = window.findChild<TerminalArea *>();
        auto *commands = window.findChild<Commands *>();
        const auto copyKeys = commands->collection()->action("terminal_copy")->shortcuts();
        QVERIFY(copyKeys.contains(QKeySequence("Ctrl+C")));
        QVERIFY(copyKeys.contains(QKeySequence("Ctrl+Shift+C")));
        QVERIFY(copyKeys.contains(QKeySequence("Ctrl+Insert")));
        QSignalSpy focused(terminal, &TerminalArea::sessionFocused);
        QCOMPARE(commands->collection()->action("focus_projects")->shortcut(),
                 QKeySequence("Ctrl+Alt+P"));
        QCOMPARE(commands->collection()->action("focus_search")->shortcut(),
                 QKeySequence("Ctrl+Alt+F"));
        QCOMPARE(commands->collection()->action("command_palette")->shortcut(),
                 QKeySequence("Ctrl+Shift+P"));
        QCOMPARE(commands->collection()->action("terminal_find")->shortcut(),
                 QKeySequence("Ctrl+Shift+F"));
        for (int i = 0; i < 5; ++i) {
            query->setFocus();
            QTest::keyClick(query, Qt::Key(Qt::Key_1 + i), Qt::AltModifier);
            QCOMPARE(tabs->currentIndex(), i);
            QCOMPARE(QApplication::focusWidget(), tree);
        }
        QTest::keyClick(tree, Qt::Key_4, Qt::AltModifier);
        QTest::keyClick(tree, Qt::Key_F, Qt::ControlModifier | Qt::AltModifier);
        QCOMPARE(QApplication::focusWidget(), query);
        query->setText("waiting");
        QTest::keyClick(query, Qt::Key_P, Qt::ControlModifier | Qt::AltModifier);
        QCOMPARE(QApplication::focusWidget(), tree);
        QTest::keyClick(tree, Qt::Key_P, Qt::ControlModifier | Qt::ShiftModifier);
        QPointer<KCommandBar> palette = window.findChild<KCommandBar *>("commandPalette");
        QVERIFY(palette && palette->isVisible());
        auto *commandSearch = palette->findChild<QLineEdit *>();
        QVERIFY(commandSearch);
        commandSearch->setText("Show Running");
        const auto screenshots = qEnvironmentVariable("ARCHCODEX_TEST_SCREENSHOT_DIR");
        if (!screenshots.isEmpty()) {
            QApplication::processEvents();
            QVERIFY(window.grab().save(screenshots + "/command-palette.png"));
        }
        QTest::keyClick(commandSearch, Qt::Key_Return);
        QTRY_COMPARE(tabs->currentIndex(), 2);
        QTRY_VERIFY(!palette);
        QCOMPARE(QApplication::focusWidget(), tree);
        QTest::keyClick(tree, Qt::Key_4, Qt::AltModifier);
        QCOMPARE(query->text(), "waiting");
        auto session = nodeWithKey(tree, sessionKey("b", "waiting"));
        auto folder = session.parent();
        tree->setCurrentIndex(folder);
        QVERIFY(tree->isExpanded(folder));
        QTest::keyClick(tree, Qt::Key_Return);
        QVERIFY(!tree->isExpanded(folder));
        QTest::keyClick(tree, Qt::Key_Return);
        QVERIFY(tree->isExpanded(folder));
        QTest::keyClick(tree, Qt::Key_F10, Qt::ShiftModifier);
        QPointer<QMenu> menu = tree->findChild<QMenu *>("sessionContextMenu");
        QVERIFY(menu && menu->isVisible());
        QCOMPARE(menu->actions().size(), 2);
        QCOMPARE(menu->actions().last()->text(), "Expand All");
        QTest::keyClick(menu, Qt::Key_Escape);
        QTRY_VERIFY(!menu);
        QCOMPARE(focused.size(), 0);
        bool sawSettings = false;
        bool savedScreenshot = true;
        SettingsShown shown([&](SettingsDialog *dialog) {
            sawSettings = true;
            auto *categories = dialog->findChild<QListWidget *>("settingsCategories");
            categories->setCurrentItem(
                categories->findItems("Keyboard Shortcuts", Qt::MatchExactly).first());
            if (!screenshots.isEmpty()) {
                QApplication::processEvents();
                savedScreenshot = dialog->grab().save(screenshots + "/keyboard-settings.png");
            }
            dialog->reject();
        });
        // Offscreen has no window manager to reactivate the owner after a menu.
        if (QGuiApplication::platformName() == "offscreen")
            window.activateWindow();
        tree->setFocus();
        QTRY_COMPARE(QApplication::activeWindow(), &window);
        QCOMPARE(QApplication::activePopupWidget(), nullptr);
        QVERIFY(commands->collection()->action("configureArchcodex")->isEnabled());
        QCOMPARE(commands->collection()->action("configureArchcodex")->shortcut(),
                 QKeySequence(Qt::ControlModifier | Qt::ShiftModifier | Qt::Key_Comma));
        QTest::keyClick(tree, Qt::Key_Comma, Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_VERIFY(sawSettings);
        QVERIFY(savedScreenshot);
        window.close();
    }
    void shortcutSettingsAreDraftedValidatedAndPersisted() {
        QTemporaryDir data;
        QWidget host;
        Commands commands(&host);
        auto *projects = commands.add("focus_projects", "Projects", "Navigation",
                                      {QKeySequence("Ctrl+Alt+P")}, [] {});
        auto *terminal = commands.add("focus_terminal", "Terminal", "Navigation",
                                      {QKeySequence("Alt+0")}, [] {});
        const auto config = data.filePath("shortcutsrc");
        QVERIFY(commands.load(config).isEmpty());
        {
            SettingsDialog dialog(true, false, {}, &host, &commands);
            dialog.show();
            auto *draft = dialog.findChild<KActionCollection *>("shortcutDraft");
            QVERIFY(draft);
            draft->action("focus_projects")->setShortcuts({QKeySequence("Ctrl+Alt+J")});
            QCOMPARE(projects->shortcut(), QKeySequence("Ctrl+Alt+P"));
            dialog.reject();
        }
        QCOMPARE(projects->shortcut(), QKeySequence("Ctrl+Alt+P"));
        {
            SettingsDialog dialog(true, false, {}, &host, &commands);
            dialog.show();
            auto *draft = dialog.findChild<KActionCollection *>("shortcutDraft");
            auto *apply = dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Apply);
            draft->action("focus_projects")->setShortcuts({QKeySequence("Alt+0")});
            QSignalSpy applied(&dialog, &SettingsDialog::settingsApplied);
            apply->click();
            QCOMPARE(applied.size(), 0); // Duplicate shortcuts reject the entire Apply.
            QCOMPARE(projects->shortcut(), QKeySequence("Ctrl+Alt+P"));
            draft->action("focus_projects")->setShortcuts({QKeySequence("Ctrl+K, Ctrl+P")});
            apply->click();
            QCOMPARE(applied.size(), 0);
            draft->action("focus_projects")->setShortcuts({QKeySequence("Ctrl+Alt+J")});
            apply->click();
            QCOMPARE(applied.size(), 1);
            QCOMPARE(projects->shortcut(), QKeySequence("Ctrl+Alt+J"));
            QCOMPARE(terminal->shortcut(), QKeySequence("Alt+0"));
            dialog.findChild<KShortcutsEditor *>()->allDefault();
            QCOMPARE(projects->shortcut(), QKeySequence("Ctrl+Alt+J"));
            dialog.reject(); // Apply survives subsequent unapplied draft edits.
        }
        QWidget secondHost;
        Commands reloaded(&secondHost);
        auto *again = reloaded.add("focus_projects", "Projects", "Navigation",
                                   {QKeySequence("Ctrl+Alt+P")}, [] {});
        reloaded.add("focus_terminal", "Terminal", "Navigation", {QKeySequence("Alt+0")}, [] {});
        QVERIFY(reloaded.load(config).isEmpty());
        QCOMPARE(again->shortcut(), QKeySequence("Ctrl+Alt+J"));
        SettingsDialog reset(true, false, {}, &secondHost, &reloaded);
        reset.findChild<KShortcutsEditor *>()->allDefault();
        reset.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Apply)->click();
        QCOMPARE(again->shortcut(), QKeySequence("Ctrl+Alt+P"));
    }
    void heldShortcutValidation() {
        QWidget host;
        Commands commands(&host);
        auto *next = commands.add("next", "Next", "Navigation", {QKeySequence("Ctrl+Tab")}, [] {});
        auto *previous = commands.add("previous", "Previous", "Navigation",
                                      {QKeySequence("Ctrl+Shift+Tab")}, [] {});
        next->setProperty("cycleKind", 1);
        previous->setProperty("cycleKind", 1);
        auto values = Commands::bindings(commands.collection());
        QVERIFY(commands.validate(values).isEmpty());
        values["next"] = {QKeySequence("Ctrl+Alt+Tab")};
        QVERIFY(!commands.validate(values).isEmpty());
        values["next"] = {QKeySequence("Alt+Tab")};
        QVERIFY(!commands.validate(values).isEmpty()); // Mixed held modifiers.
        values["next"] = {QKeySequence("Ctrl+Tab"), QKeySequence("Ctrl+F8")};
        QVERIFY(!commands.validate(values).isEmpty());
        values["next"] = {QKeySequence(Qt::ControlModifier | Qt::Key_Backtab)};
        QVERIFY(!commands.validate(values).isEmpty()); // Same as Ctrl+Shift+Tab.
        values["next"] = {QKeySequence("Alt+F8")};
        values["previous"] = {QKeySequence("Alt+Shift+F8")};
        QVERIFY(commands.validate(values).isEmpty());
    }
    void existingOrderingAfterBackgroundTransition_data() {
        QTest::addColumn<bool>("separateProjects");
        QTest::newRow("sessions") << false;
        QTest::newRow("projects") << true;
    }
    void existingOrderingAfterBackgroundTransition() {
        QFETCH(bool, separateProjects);
        Fixture fixture;
        fixture.loaded = {"thread-1", "background"};
        fixture.threadFields["status"] =
            QJsonObject{{"type", "active"}, {"activeFlags", QJsonArray{}}};
        QTemporaryDir data;
        {
            Store store(data.path() + "/metadata.sqlite");
            store.save(fixture.profile());
        }
        Window window(data.path(), false);
        QTRY_VERIFY(window.findChild<Backend *>());
        auto *backend = window.findChild<Backend *>();
        backend->connectServer();
        auto *tree = window.findChild<QTreeView *>();
        QTRY_COMPARE(visibleThreads(tree).size(), 2);
        const auto foregroundKey = sessionKey(backend->profile.id, "thread-1");
        const auto backgroundKey = sessionKey(backend->profile.id, "background");
        if (separateProjects) {
            auto foreground = Session::fromThread(backend->profile.id, fixture.thread());
            foreground.cwd = "/work/foreground";
            backend->sessionChanged(foreground);
        }
        window.findChild<TerminalArea *>()->sessionFocused(foregroundKey);
        const auto opened = nodeWithKey(tree, foregroundKey).data(RecencyRole).toLongLong();
        QCOMPARE(nodeWithKey(tree, foregroundKey).row(), 0);
        // Events and visits currently use whole seconds. Cross that boundary so
        // this exercises a strictly newer transition rather than the tie breaker.
        QTRY_VERIFY(QDateTime::currentSecsSinceEpoch() > opened);
        fixture.notify("thread/status/changed",
                       {{"threadId", "background"}, {"status", QJsonObject{{"type", "idle"}}}});
        QTRY_VERIFY(nodeWithKey(tree, backgroundKey).data(WaitingRole).toBool());
        QVERIFY(nodeWithKey(tree, backgroundKey).data(RecencyRole).toLongLong() > opened);
        QCOMPARE(nodeWithKey(tree, backgroundKey).row(), 0);
        if (separateProjects)
            QCOMPARE(nodeWithKey(tree, backgroundKey).parent().row(), 0);
        window.close();
    }
    void tabFiltersAreIndependentLocalAndLive() {
        Fixture fixture;
        QTemporaryDir data;
        {
            Store store(data.path() + "/metadata.sqlite");
            store.save(fixture.profile());
            store.save(BackendProfile{"other", "Other"});
        }
        Window window(data.path(), false);
        window.show();
        QTRY_COMPARE(window.findChildren<Backend *>().size(), 2);
        Backend *backend = nullptr, *other = nullptr;
        for (auto *b : window.findChildren<Backend *>())
            (b->profile.id == "other" ? other : backend) = b;
        backend->connectServer();
        auto *tree = window.findChild<QTreeView *>();
        auto *tabs = window.findChild<QTabBar *>();
        auto *query = window.findChild<QLineEdit *>("sessionSearch");
        auto *scope = window.findChild<QComboBox *>();
        auto *feedback = window.findChild<KSqueezedTextLabel *>("searchFeedback");
        auto *terminal = window.findChild<TerminalArea *>();
        QTRY_VERIFY(visibleThreads(tree).contains("thread-1"));
        QTRY_VERIFY(backend->ready());
        QPushButton *loadMore = nullptr;
        for (auto *button : window.findChildren<QPushButton *>())
            if (button->text() == "Load more history")
                loadMore = button;
        QVERIFY(loadMore);
        backend->moreAvailable(true);
        QVERIFY(loadMore->isEnabled());
        auto put = [&](Backend *b, const QString &id, const QString &cwd, const QString &type) {
            auto fields = fixture.thread(id);
            fields["name"] = "Needle " + id;
            fields["cwd"] = cwd;
            fields["status"] = QJsonObject{{"type", type}, {"activeFlags", QJsonArray{}}};
            auto s = Session::fromThread(b->profile.id, fields);
            b->sessionChanged(s);
            return s;
        };
        auto waiting = put(backend, "waiting", "/work/project/deep", "idle");
        auto running = put(backend, "running", "/work/project/build", "active");
        auto sibling = put(backend, "sibling", "/work/project-other", "idle");
        put(backend, "history", "/history", "notLoaded");
        put(other, "remote", "/work/project/deep", "idle");
        QTest::qWait(50); // Let the initial snapshot finish before counting requests.
        const auto requests = fixture.methods;
        query->setText("all draft"); // All only submits on Enter.
        tabs->setCurrentIndex(1);
        QVERIFY(query->text().isEmpty());
        query->setText("NEEDLE");
        QCOMPARE(visibleThreads(tree), (QStringList{"remote", "running", "sibling", "waiting"}));
        QVERIFY(query->toolTip().contains("conversation bodies are not searched"));
        QVERIFY(feedback->fullText().contains("4 matching sessions"));
        QTest::keyClick(query, Qt::Key_Return);
        QVERIFY(tabs->isEnabled());
        tabs->setCurrentIndex(2);
        query->setText("running");
        QCOMPARE(visibleThreads(tree), QStringList{"running"});
        tabs->setCurrentIndex(3);
        auto waitingNode = [&] {
            return nodeWithKey(tree, sessionKey(waiting.backendId, waiting.id));
        };
        tree->setCurrentIndex(waitingNode());
        const auto folderKey = waitingNode().parent().data(KeyRole).toString();
        tree->collapse(nodeWithKey(tree, folderKey));
        const auto selectedBeforeFiltering = tree->currentIndex().data(KeyRole).toString();
        scope->setCurrentIndex(1); // Session scope means its directory and descendants.
        QCOMPARE(tree->currentIndex().data(KeyRole).toString(), selectedBeforeFiltering);
        query->setText("needle");
        QCOMPARE(visibleThreads(tree), QStringList{"waiting"});
        QCOMPARE(serverNode(tree).parent().data(KindRole).toInt(), int(NodeKind::Group));
        QCOMPARE(waitingNode().parent().data().toString(), "deep");
        QVERIFY(feedback->fullText().contains(waiting.cwd));
        query->setText("no match");
        QCOMPARE(visibleThreads(tree).size(), 0);
        query->setText("needle"); // Scope must survive an empty result tree.
        QCOMPARE(visibleThreads(tree), QStringList{"waiting"});
        tree->collapse(waitingNode().parent());
        query->setText("Needle waiting");
        QVERIFY(!tree->isExpanded(waitingNode().parent()));
        query->setText("needle");
        waiting.status = {{"type", "active"}, {"activeFlags", QJsonArray{}}};
        backend->sessionChanged(waiting);
        QCOMPARE(visibleThreads(tree).size(), 0);
        QCOMPARE(query->text(), "needle");
        waiting.status = {{"type", "idle"}};
        backend->sessionChanged(waiting);
        QCOMPARE(visibleThreads(tree), QStringList{"waiting"});
        waiting.title = "Changed title";
        backend->sessionChanged(waiting);
        QCOMPARE(visibleThreads(tree).size(), 0);
        waiting.title = "Needle waiting";
        backend->sessionChanged(waiting);
        QCOMPARE(visibleThreads(tree), QStringList{"waiting"});
        put(backend, "incremental", waiting.cwd, "idle");
        QCOMPARE(visibleThreads(tree), (QStringList{"incremental", "waiting"}));
        QVERIFY(feedback->fullText().startsWith("2 matching sessions"));
        backend->sessionRemoved("incremental");
        QVERIFY(feedback->fullText().startsWith("1 matching sessions"));
        tabs->setCurrentIndex(4);
        query->setText("waiting");
        QCOMPARE(visibleThreads(tree).size(), 0);
        waiting.terminalOpen = true;
        waiting.clientIssue = "Retained exit output";
        terminal->clientStateChanged(waiting);
        QCOMPARE(visibleThreads(tree), QStringList{"waiting"});
        waiting.terminalOpen = false;
        waiting.clientIssue.clear();
        terminal->clientStateChanged(waiting);
        QCOMPARE(visibleThreads(tree).size(), 0);
        for (const auto tab : {0, 1, 2, 3, 4}) {
            tabs->setCurrentIndex(tab);
            QCOMPARE(loadMore->isEnabled(), tab == 0);
            QCOMPARE(query->text(),
                     (QStringList{"all draft", "NEEDLE", "running", "needle", "waiting"})[tab]);
            QCOMPARE(scope->currentIndex(), tab == 3 ? 1 : 0);
        }
        tabs->setCurrentIndex(3);
        query->clear();
        QVERIFY(visibleThreads(tree).contains("remote"));
        QVERIFY(visibleThreads(tree).contains("sibling"));
        QCOMPARE(tree->currentIndex().data(KeyRole).toString(), selectedBeforeFiltering);
        QVERIFY(!tree->isExpanded(nodeWithKey(tree, folderKey)));
        // Server and group scopes are intersections with Waiting, never a global search.
        scope->setCurrentIndex(0);
        tree->setCurrentIndex(nodeWithKey(tree, "server:" + waiting.backendId));
        scope->setCurrentIndex(1);
        query->setText("needle");
        QCOMPARE(visibleThreads(tree), (QStringList{"sibling", "waiting"}));
        query->clear();
        scope->setCurrentIndex(0);
        tree->setCurrentIndex(serverNode(tree).parent());
        scope->setCurrentIndex(1);
        query->setText("needle");
        QCOMPARE(visibleThreads(tree), (QStringList{"remote", "sibling", "waiting"}));
        const auto screenshots = qEnvironmentVariable("ARCHCODEX_TEST_SCREENSHOT_DIR");
        if (!screenshots.isEmpty())
            QVERIFY(window.grab().save(screenshots + "/waiting-filter.png"));
        query->clear();
        // A directory scope must exclude a sibling with the same path prefix.
        scope->setCurrentIndex(0);
        tree->setCurrentIndex(
            nodeWithKey(tree, "folder:" + projectKey(waiting.backendId, "/work/project")));
        QVERIFY(tree->currentIndex().isValid());
        scope->setCurrentIndex(1);
        query->setText("needle");
        QCOMPARE(visibleThreads(tree), QStringList{"waiting"});
        QTest::keyClick(query, Qt::Key_Return);
        QTest::qWait(50);
        QCOMPARE(fixture.methods, requests);
        window.close();
    }
    void searchPagesPreserveBrowsingAndTerminalFocus() {
        Fixture fixture;
        fixture.delaySearch = true;
        PtyProbe child;
        QVERIFY(child.install());
        auto profile = fixture.profile();
        profile.executable = child.executable();
        QTemporaryDir data;
        {
            Store store(data.filePath("metadata.sqlite"));
            store.save(profile);
        }
        Window window(data.path(), false);
        window.resize(1300, 900);
        window.show();
        window.activateWindow();
        QTRY_VERIFY(window.findChild<Backend *>());
        auto *backend = window.findChild<Backend *>();
        backend->connectServer();
        QTRY_VERIFY(backend->ready());
        auto *area = window.findChild<TerminalArea *>();
        const auto current = Session::fromThread(profile.id, fixture.thread());
        QString error;
        QVERIFY2(area->open(profile, current, &error), qPrintable(error));
        auto *query = window.findChild<QLineEdit *>("sessionSearch");
        auto *tree = window.findChild<QTreeView *>();
        query->setText("needle");
        QTest::keyClick(query, Qt::Key_Return);
        QTRY_COMPARE(fixture.searchIds.size(), 1);
        QJsonArray first{QJsonObject{{"thread", fixture.thread()}}};
        for (int i = 0; i < 90; ++i) {
            auto t = fixture.thread(QString("match-%1").arg(i));
            t["updatedAt"] = 200 - i;
            first << QJsonObject{{"thread", t}};
        }
        fixture.reply(fixture.searchIds[0], {{"data", first}, {"nextCursor", "second"}});
        QTRY_COMPARE(fixture.searchIds.size(), 2); // First page consumed before the next request.
        const auto browsedKey = sessionKey(profile.id, "match-89");
        tree->setCurrentIndex(nodeWithKey(tree, browsedKey));
        tree->scrollTo(tree->currentIndex());
        const auto scroll = tree->verticalScrollBar()->value();
        area->focusCurrent();
        QTRY_VERIFY(area->isAncestorOf(QApplication::focusWidget()));
        auto *focus = QApplication::focusWidget();
        fixture.reply(fixture.searchIds[1],
                      {{"data", QJsonArray{QJsonObject{{"thread", fixture.thread("later")}}}},
                       {"nextCursor", "third"}});
        QTRY_COMPARE(fixture.searchIds.size(), 3);
        QCOMPARE(tree->currentIndex().data(KeyRole).toString(), browsedKey);
        QCOMPARE(tree->verticalScrollBar()->value(), scroll);
        const auto folderKey = tree->currentIndex().parent().data(KeyRole).toString();
        tree->collapse(tree->currentIndex().parent());
        const auto selected = tree->currentIndex().data(KeyRole).toString();
        auto branch = fixture.thread("new-branch");
        branch["cwd"] = "/other/path/to/worktree";
        fixture.reply(fixture.searchIds[2], {{"data", QJsonArray{QJsonObject{{"thread", branch}}}},
                                             {"nextCursor", QJsonValue::Null}});
        auto *progress = window.findChild<KSqueezedTextLabel *>("searchProgress");
        QTRY_VERIFY(progress->fullText().contains("Search complete"));
        QCOMPARE(tree->currentIndex().data(KeyRole).toString(), selected);
        QVERIFY(!tree->isExpanded(nodeWithKey(tree, folderKey)));
        QCOMPARE(QApplication::focusWidget(), focus);
        QCOMPARE(area->currentKey(), sessionKey(profile.id, current.id));
        window.close();
    }
    void searchFeedbackStaysBounded_data() {
        QTest::addColumn<int>("fontSize");
        QTest::addColumn<int>("paneWidth");
        QTest::newRow("narrow") << 10 << 240;
        QTest::newRow("large-font") << 20 << 340;
    }
    void searchFeedbackStaysBounded() {
        QFETCH(int, fontSize);
        QFETCH(int, paneWidth);
        Fixture fixture;
        fixture.delaySearch = true;
        QTemporaryDir data;
        {
            Store store(data.filePath("metadata.sqlite"));
            store.save(fixture.profile());
            for (int i = 0; i < 24; ++i) {
                auto p = fixture.profile();
                p.id = QString("offline-%1").arg(i);
                p.name = "<b>Long identical connection names</b> with host and container details";
                p.socketPath = data.filePath(p.id);
                store.save(p);
            }
        }
        Window window(data.path(), false);
        auto font = window.font();
        font.setPointSize(fontSize);
        window.setFont(font);
        window.resize(1300, 1100);
        window.show();
        window.activateWindow();
        window.findChild<QSplitter *>()->setSizes({paneWidth, 1300 - paneWidth});
        QTRY_COMPARE(window.findChildren<Backend *>().size(), 25);
        Backend *online = nullptr, *offline = nullptr;
        for (auto *b : window.findChildren<Backend *>())
            if (b->profile.id == fixture.profile().id)
                online = b;
            else
                offline = b;
        QVERIFY(online && offline);
        online->connectServer();
        QTRY_VERIFY(online->ready());
        auto *query = window.findChild<QLineEdit *>("sessionSearch");
        auto *tree = window.findChild<QTreeView *>();
        auto *summary = window.findChild<KSqueezedTextLabel *>("searchFeedback");
        auto *progress = window.findChild<KSqueezedTextLabel *>("searchProgress");
        auto *area = window.findChild<QWidget *>("searchFeedbackArea");
        auto *button = window.findChild<QToolButton *>("searchDetailsButton");
        query->setFocus();
        query->setText("needle");
        QTest::keyClick(query, Qt::Key_Return);
        QTRY_COMPARE(fixture.searchIds.size(), 1);
        QTRY_VERIFY(progress->fullText().contains("24 incomplete"));
        QVERIFY(progress->fullText().contains("Searching 1"));
        QCOMPARE(summary->fullText(), "0 results");
        QCOMPARE(QApplication::focusWidget(), query);
        QVERIFY(tree->viewport()->height() > 350);
        QVERIFY(area->height() <= 4 * summary->fontMetrics().height() + 20);
        const auto height = tree->viewport()->height();
        button->setFocus();
        QTest::keyClick(button, Qt::Key_Space);
        QTRY_VERIFY(window.findChild<QDialog *>("searchDetails"));
        QPointer<QDialog> dialog = window.findChild<QDialog *>("searchDetails");
        QVERIFY(!dialog->isModal());
        auto *report = dialog->findChild<QPlainTextEdit *>();
        QVERIFY(report->toPlainText().contains("<b>Long identical connection names</b>"));
        for (int i = 0; i < 24; ++i)
            QVERIFY(report->toPlainText().contains(QString("[offline-%1]").arg(i)));
        QTRY_VERIFY(dialog->isActiveWindow());
        report->setFocus();
        auto cursor = report->textCursor();
        cursor.setPosition(0);
        cursor.setPosition(5, QTextCursor::KeepAnchor);
        report->setTextCursor(cursor);
        const auto text = report->toPlainText();
        offline->searchResults(1, {}, "Disconnected — search coverage incomplete.", true);
        QCOMPARE(report->toPlainText(), text); // Repeated diagnostics are one entry per backend.
        auto visible = Session::fromThread(online->profile.id, fixture.thread("visible"));
        auto hidden = visible;
        hidden.id = "hidden";
        hidden.ephemeral = true;
        online->searchResults(1, {visible, hidden}, {}, false);
        QCOMPARE(summary->fullText(), "1 results · 1 hidden");
        QCOMPARE(tree->viewport()->height(), height);
        QCOMPARE(QApplication::focusWidget(), report);
        QCOMPARE(report->textCursor().selectedText(), "Query");
        fixture.reply(fixture.searchIds.first(),
                      {{"data", QJsonArray{}}, {"nextCursor", QJsonValue::Null}});
        QTRY_VERIFY(progress->fullText().contains("Search complete"));
        QVERIFY(progress->fullText().contains("24 incomplete"));
        window.findChild<QTabBar *>()->setCurrentIndex(3);
        QVERIFY(!dialog || !dialog->isVisible());
        QVERIFY(!area->isVisible());
        window.findChild<QTabBar *>()->setCurrentIndex(0);
        query->clear();
        QVERIFY(!area->isVisible());
        query->setText("fresh");
        QTest::keyClick(query, Qt::Key_Return);
        button->click();
        dialog = window.findChild<QDialog *>("searchDetails");
        // A closed dialog is deleteLater'd; the visible dialog belongs to this query.
        for (auto *d : window.findChildren<QDialog *>("searchDetails"))
            if (d->isVisible())
                dialog = d;
        QVERIFY(dialog && dialog->isVisible());
        offline->searchResults(1, {}, "obsolete diagnostic", true);
        QVERIFY(!dialog->findChild<QPlainTextEdit *>()->toPlainText().contains("obsolete"));
        QVERIFY(dialog->findChild<QPlainTextEdit *>()->toPlainText().startsWith("Query: fresh"));
        window.close();
    }
    void allSearchFinishesInBackgroundAndRejectsStaleResults() {
        Fixture fixture;
        fixture.delaySearch = true;
        QTemporaryDir data;
        {
            Store store(data.path() + "/metadata.sqlite");
            store.save(fixture.profile());
        }
        Window window(data.path(), false);
        window.show();
        QTRY_VERIFY(window.findChild<Backend *>());
        auto *backend = window.findChild<Backend *>();
        backend->connectServer();
        auto *tree = window.findChild<QTreeView *>();
        auto *tabs = window.findChild<QTabBar *>();
        auto *query = window.findChild<QLineEdit *>("sessionSearch");
        auto *feedback = window.findChild<KSqueezedTextLabel *>("searchFeedback");
        QTRY_VERIFY(visibleThreads(tree).contains("thread-1"));
        query->setText("conversation text");
        QTest::keyClick(query, Qt::Key_Return);
        QTRY_COMPARE(fixture.searchIds.size(), 1);
        QVERIFY(tabs->isEnabled());
        tabs->setCurrentIndex(3);
        query->setText("example");
        QCOMPARE(visibleThreads(tree), QStringList{"thread-1"});
        fixture.reply(fixture.searchIds[0],
                      {{"data", QJsonArray{QJsonObject{{"thread", fixture.thread("found")}}}},
                       {"nextCursor", QJsonValue::Null}});
        QTest::qWait(50);
        QCOMPARE(query->text(), "example");
        QCOMPARE(visibleThreads(tree), QStringList{"thread-1"});
        QVERIFY(feedback->fullText().contains("cached titles"));
        tabs->setCurrentIndex(0);
        QCOMPARE(query->text(), "conversation text");
        QTRY_COMPARE(visibleThreads(tree), QStringList{"found"});
        QVERIFY(!feedback->fullText().contains("searching"));
        query->clear();
        QCOMPARE(visibleThreads(tree), QStringList{"thread-1"});
        query->setText("old request");
        QTest::keyClick(query, Qt::Key_Return);
        QTRY_COMPARE(fixture.searchIds.size(), 2);
        query->setText("new request");
        QTest::keyClick(query, Qt::Key_Return);
        QTRY_COMPARE(fixture.searchIds.size(), 3);
        fixture.reply(fixture.searchIds[1],
                      {{"data", QJsonArray{QJsonObject{{"thread", fixture.thread("stale")}}}},
                       {"nextCursor", QJsonValue::Null}});
        fixture.reply(fixture.searchIds[2],
                      {{"data", QJsonArray{QJsonObject{{"thread", fixture.thread("fresh")}}}},
                       {"nextCursor", QJsonValue::Null}});
        QTRY_COMPARE(visibleThreads(tree), QStringList{"fresh"});
        backend->searchResults(
            1, {Session::fromThread(backend->profile.id, fixture.thread("stale-signal"))},
            "old error", true);
        QCOMPARE(visibleThreads(tree), QStringList{"fresh"});
        QVERIFY(!feedback->fullText().contains("old error"));
        query->setText("interrupted search");
        QTest::keyClick(query, Qt::Key_Return);
        QTRY_COMPARE(fixture.searchIds.size(), 4);
        tabs->setCurrentIndex(3);
        fixture.peer->close();
        QTRY_VERIFY(!backend->ready());
        QCOMPARE(query->text(), "example");
        QVERIFY(!feedback->fullText().contains("interrupted"));
        tabs->setCurrentIndex(0);
        auto *progress = window.findChild<KSqueezedTextLabel *>("searchProgress");
        QVERIFY(progress->fullText().contains("1 incomplete"));
        window.findChild<QToolButton *>("searchDetailsButton")->click();
        auto *details = window.findChild<QDialog *>("searchDetails");
        QVERIFY(details);
        QVERIFY(details->findChild<QPlainTextEdit *>()->toPlainText().contains("interrupted"));
        query->clear();
        window.close();
    }
    void contextMenusUseClickedRowsAndExpandOnlyTheirSubtree() {
        Fixture first, second;
        QTemporaryDir data;
        {
            Store store(data.path() + "/metadata.sqlite");
            auto a = first.profile(), b = second.profile();
            a.id = "first";
            b.id = "second";
            store.save(a);
            store.save(b);
        }
        Window window(data.path(), false);
        window.show();
        QTRY_COMPARE(window.findChildren<Backend *>().size(), 2);
        for (auto *backend : window.findChildren<Backend *>())
            backend->connectServer();
        auto *tree = window.findChild<QTreeView *>();
        auto *tabs = window.findChild<QTabBar *>();
        QTRY_COMPARE(serverNodes(tree).size(), 2);
        auto server = [&](const QString &id) {
            for (const auto &item : serverNodes(tree))
                if (item.data(BackendRole).toString() == id)
                    return item;
            return QModelIndex{};
        };
        auto folder = [&](const QString &id) { return tree->model()->index(0, 0, server(id)); };
        auto session = [&](const QString &id) { return tree->model()->index(0, 0, folder(id)); };
        QTRY_COMPARE(session("first").data(ThreadRole).toString(), "thread-1");
        QTRY_COMPARE(session("second").data(ThreadRole).toString(), "thread-1");
        auto menuFor = [&](const QModelIndex &item) {
            tree->scrollTo(item);
            tree->customContextMenuRequested(tree->visualRect(item).center());
            return QPointer<QMenu>(tree->findChild<QMenu *>("sessionContextMenu"));
        };
        auto labels = [](QMenu *menu) {
            QStringList names;
            for (auto *action : menu->actions())
                names << action->text();
            return names;
        };
        auto dispose = [](QMenu *menu) {
            if (!menu)
                return; // Triggering a popup action can already have closed/deleted it.
            menu->hide();
            delete menu;
        };
        for (const auto tab : {0, 2, 3}) {
            if (tab == 2)
                for (auto *backend : window.findChildren<Backend *>())
                    backend->sessionChanged(Session::fromThread(
                        backend->profile.id,
                        {{"id", "thread-1"},
                         {"cwd", "/work/project"},
                         {"status",
                          QJsonObject{{"type", "active"}, {"activeFlags", QJsonArray{}}}}}));
            if (tab == 3)
                for (auto *backend : window.findChildren<Backend *>())
                    backend->sessionChanged(
                        Session::fromThread(backend->profile.id, first.thread()));
            tabs->setCurrentIndex(tab);
            tree->expandAll();
            QCOMPARE(server("first").parent().data(KindRole).toInt(), int(NodeKind::Group));
            auto menu = menuFor(session("first"));
            QVERIFY(menu);
            QCOMPARE(labels(menu), (QStringList{"Fork", "New", "Close"}));
            QVERIFY(menu->actions()[0]->isEnabled());
            QVERIFY(menu->actions()[1]->isEnabled());
            QVERIFY(!menu->actions()[2]->isEnabled());
            dispose(menu);
        }
        tree->collapse(folder("first"));
        tree->collapse(folder("second"));
        auto menu = menuFor(folder("first"));
        QCOMPARE(labels(menu), (QStringList{"New", "Expand All"}));
        menu->actions().last()->trigger();
        QVERIFY(tree->isExpanded(folder("first")));
        QVERIFY(!tree->isExpanded(folder("second")));
        dispose(menu);
        for (const auto &parent : {server("first"), server("first").parent()}) {
            menu = menuFor(parent);
            QCOMPARE(labels(menu), QStringList{"Expand All"});
            dispose(menu);
        }
        // The selected row differs from the right-clicked row. A preflight must
        // target only the clicked server, even if its row moves while the menu is open.
        tree->expandAll();
        tree->setCurrentIndex(session("second"));
        first.delayRead = true;
        const auto reads = first.readIds.size();
        const auto otherReads = second.readIds.size();
        menu = menuFor(session("first"));
        menu->actions().first()->trigger();
        QTRY_COMPARE(first.readIds.size(), reads + 1);
        QCOMPARE(second.readIds.size(), otherReads);
        QVERIFY(!first.methods.contains("thread/fork"));
        dispose(menu);
        window.close();
    }
    void thisComputerCannotBeDisabledOrRemoved() {
        TestHome home;
        QTemporaryDir data;
        Window window(data.path(), false);
        QTRY_VERIFY(window.workspaceLoaded && window.cachePending.isEmpty());
        const auto id = window.hosts.cbegin().key();
        QVERIFY(window.hosts[id].isLocal());
        window.setHostEnabled(id, false);
        window.removeHost(id);
        QVERIFY(window.hosts.contains(id));
        QVERIFY(window.hosts[id].enabled);
        QVERIFY(window.hosts[id].servers);
        QTimer::singleShot(0, &window, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            auto close = qScopeGuard([&] { dialog->reject(); });
            QVERIFY(!dialog->findChild<QPushButton *>("disableHost")->isEnabled());
            QVERIFY(!dialog->findChild<QPushButton *>("removeHost")->isEnabled());
            QVERIFY(dialog->findChild<QCheckBox *>("discoverHostServers")->isChecked());
            QVERIFY(!dialog->findChild<QCheckBox *>("discoverHostServers")->isEnabled());
        });
        window.hostsDialog();
        window.saveHosts();
        window.close();
        Window restored(data.path(), false);
        QVERIFY(restored.hosts[id].enabled);
        restored.close();
    }
    void remoteNativeInventoryIsScopedAndLocalRefreshCannotRetireIt() {
        TestHome home;
        QTemporaryDir data;
        Window window(data.path(), false);
        QTRY_VERIFY(window.workspaceLoaded && window.cachePending.isEmpty());
        QJsonObject endpoint{{"path", "/tmp/same.sock"},
                             {"localSocket", "/tmp/fixture-relay"},
                             {"user", "alice"},
                             {"host", "remote"},
                             {"ownerHome", "/home/alice"},
                             {"hostEnvironment", true},
                             {"default", true}};
        QJsonObject inventory{{"endpoints", QJsonArray{endpoint}},
                              {"issues", QJsonArray{}},
                              {"complete", true},
                              {"revision", "fresh"}};
        QStringList ids;
        for (const auto &host : {QString("first"), QString("second")}) {
            window.addHost({host, host + "-ssh"}, false);
            const auto id =
                "ssh-" + QString::fromLatin1(QCryptographicHash::hash(
                                                 (host + QChar(0x1f) + "/tmp/same.sock").toUtf8(),
                                                 QCryptographicHash::Sha256)
                                                 .toHex());
            ids << id;
            window.paused.insert(id); // Prevent external connections in this routing test.
            QVERIFY(window.applyRemoteServers(host, inventory).isEmpty());
            QCOMPARE(window.profiles[id].discoveryHost, host);
            QVERIFY(window.profiles[id].usesRelay());
        }
        QCOMPARE(window.profiles.size(), 2);
        QVERIFY(window.groupKey(window.profiles[ids[0]]) !=
                window.groupKey(window.profiles[ids[1]]));
        for (const auto &id : ids)
            window.paused.remove(id);
        window.refreshRevision = "fresh";
        window.refreshScopes = {"local", "first", "second"};
        window.applyDiscovery({}, {}, "fresh");
        QVERIFY(window.retirements.isEmpty());
        QCOMPARE(window.profiles.size(), 2);
        inventory["endpoints"] = QJsonArray{};
        inventory["complete"] = false;
        window.applyRemoteServers("first", inventory);
        QVERIFY(window.retirements.isEmpty());
        inventory["complete"] = true;
        inventory["revision"] = "stale";
        window.applyRemoteServers("first", inventory);
        QVERIFY(window.retirements.isEmpty());
        inventory["revision"] = "fresh";
        window.applyRemoteServers("first", inventory);
        QTRY_COMPARE(window.profiles.size(), 1);
        QVERIFY(window.profiles.contains(ids[1]));
        window.close();
    }
    void ordinaryAndContainerServersShareTheirHostGroups() {
        QTemporaryDir data;
        {
            Store store(data.path() + "/metadata.sqlite");
            BackendProfile local{"host", "Local"};
            local.discovered = true;
            local.autoConnect = false;
            local.identity = {{"user", "alice"}, {"host", "workstation"}, {"default", true}};
            store.save(local);
            for (const auto &id : {"local-container", "remote-container"}) {
                BackendProfile p{id, "Container"};
                p.kind = "mycodex";
                p.discovered = true;
                p.autoConnect = false;
                p.containerId = QString(64, 'a');
                p.containerEpoch = "epoch";
                p.runtimeHome = "/home/alice";
                p.runtimeUser = "alice";
                p.dockerContext = "default";
                p.discoveryHost = id;
                p.socketPath = "/tmp/codex.sock";
                p.sshHost = QString(id) == "remote-container" ? "ssh-alias" : "";
                p.identity = {
                    {"containerName", "project"},
                    {"containerWorkdir", "/work"},
                    {"dockerHost", p.sshHost.isEmpty() ? "workstation" : "remote-machine"},
                    {"stateMode", "Shared"}};
                p.name = serverLabel(p);
                store.save(p);
            }
        }
        Window window(data.path(), false);
        window.show();
        auto *tree = window.findChild<QTreeView *>();
        QTRY_COMPARE(tree->model()->rowCount(), 2);
        QCOMPARE(tree->model()->index(0, 0).data().toString(), "This Computer · alice@workstation");
        QSet<QString> labels;
        for (int i = 0; i < tree->model()->rowCount(); ++i) {
            const auto group = tree->model()->index(i, 0);
            QCOMPARE(group.data(KindRole).toInt(), int(NodeKind::Group));
            labels.insert(group.data(Qt::DisplayRole).toString());
            QCOMPARE(tree->model()->rowCount(group), i == 0 ? 2 : 1);
            for (int j = 0; j < tree->model()->rowCount(group); ++j) {
                const auto server = tree->model()->index(j, 0, group);
                QCOMPARE(server.data(Qt::DisplayRole).toString(),
                         server.data(BackendRole) == "host" ? QString("Main")
                                                            : QString("myCodex · project"));
            }
        }
        QCOMPARE(labels, (QSet<QString>{"This Computer · alice@workstation",
                                        "ssh-alias · remote-machine"}));
        const auto screenshots = qEnvironmentVariable("ARCHCODEX_TEST_SCREENSHOT_DIR");
        for (const auto tab : {0, 3, 2}) {
            for (auto *backend : window.findChildren<Backend *>())
                backend->sessionChanged(Session::fromThread(
                    backend->profile.id,
                    {{"id", "example"},
                     {"name", "Example session"},
                     {"cwd", "/work"},
                     {"path", "/saved/example"},
                     {"status", QJsonObject{{"type", tab == 2 ? "active" : "idle"},
                                            {"activeFlags", QJsonArray{}}}}}));
            window.findChild<QTabBar *>()->setCurrentIndex(tab);
            QCOMPARE(tree->model()->rowCount(), 2);
            for (int i = 0; i < 2; ++i) {
                const auto group = tree->model()->index(i, 0);
                QCOMPARE(group.data(KindRole).toInt(), int(NodeKind::Group));
                QVERIFY(labels.contains(group.data().toString()));
                QCOMPARE(tree->model()->index(0, 0, group).data(KindRole).toInt(),
                         int(NodeKind::Server));
            }
            tree->expandAll();
            if (!screenshots.isEmpty()) {
                QApplication::processEvents();
                QVERIFY(window.grab().save(screenshots + QString("/host-groups-%1.png").arg(tab)));
            }
        }
        window.close();
    }
    void cleanup() {
        QVERIFY(Worker::finishPending());
    }
    void containersHaveSeparateRootsAndSharedHistoryDoesNotBecomeLive() {
        QTemporaryDir data;
        const auto digest = [](const QString &value) {
            return QString::fromLatin1(
                QCryptographicHash::hash(value.toUtf8(), QCryptographicHash::Sha256).toHex());
        };
        QJsonArray containers;
        QStringList ids;
        {
            Store store(data.path() + "/metadata.sqlite");
            for (const auto letter : {'a', 'b'}) {
                const QString cid(64, letter);
                const auto key =
                    "local-mycodex" + QString(QChar(0x1f)) + "engine" + QChar(0x1f) + cid;
                BackendProfile p;
                p.id = "mc-" + digest(key + QChar(0x1f) + "/tmp/physical.sock");
                p.name = QString("myCodex %1").arg(letter);
                p.kind = "mycodex";
                p.autoConnect = false; // This UI fixture must never contact a real Docker engine.
                p.discovered = true;
                p.containerId = cid;
                p.containerEpoch = "epoch";
                p.runtimeHome = "/home/alice";
                p.runtimeUser = "alice";
                p.dockerContext = "default";
                p.discoveryHost = "local-mycodex";
                p.socketPath = "/tmp/physical.sock";
                p.environmentId = digest(key + "mounts");
                p.identity["containerWorkdir"] = "/workspace";
                store.save(p);
                ids << p.id;
                containers << QJsonObject{
                    {"containerId", cid},
                    {"containerEpoch", "epoch"},
                    {"engine", "engine"},
                    {"containerName", QString("project-%1").arg(letter)},
                    {"runtimeHome", p.runtimeHome},
                    {"runtimeUser", p.runtimeUser},
                    {"dockerContext", "default"},
                    {"environment", "mounts"},
                    {"stateMode", "Shared"},
                    {"stateVolume", "codex_state"},
                    {"codexHome", "/home/alice/.codex"},
                    {"cwd", "/workspace"},
                    {"issues", QJsonArray{}},
                    {"endpoints",
                     QJsonArray{QJsonObject{{"path", p.socketPath}, {"executable", "codex"}}}}};
            }
        }
        Window window(data.path(), false);
        window.show();
        QTRY_COMPARE(window.findChildren<Backend *>().size(), 2);
        auto *discovery = window.findChild<SharedHostDiscovery *>();
        discovery->snapshot(
            {{"containers", containers}, {"issues", QJsonArray{}}, {"complete", true}});
        auto *tree = window.findChild<QTreeView *>();
        QCOMPARE(serverNodes(tree).size(), 2);
        for (auto *b : window.findChildren<Backend *>()) {
            QVERIFY(!b->ready());
            auto s = Session::fromThread(
                b->profile.id,
                {{"id", "same-shared-thread"},
                 {"cwd", "/workspace"},
                 {"name", "Shared conversation"},
                 {"path", "/history.jsonl"},
                 {"status",
                  QJsonObject{{"type", b->profile.id == ids[0] ? "idle" : "notLoaded"}}}});
            s.executionObserved = b->profile.id == ids[0];
            s.executionEnvironment = b->profile.environmentId;
            b->sessionsChanged({s});
        }
        auto *tabs = window.findChild<QTabBar *>();
        tabs->setCurrentIndex(1);
        QCOMPARE(serverNodes(tree).size(), 1);
        QCOMPARE(serverNode(tree).data(BackendRole).toString(), ids[0]);
        tabs->setCurrentIndex(0);
        auto empty = containers[0].toObject();
        empty["containerId"] = QString(64, 'c');
        empty["containerName"] = "empty-container";
        empty["endpoints"] = QJsonArray{};
        containers << empty;
        discovery->snapshot(
            {{"containers", containers}, {"issues", QJsonArray{}}, {"complete", true}});
        QCOMPARE(serverNodes(tree).size(), 3);
        bool found = false;
        for (int i = 0; i < serverNodes(tree).size(); ++i) {
            const auto root = serverNode(tree, i);
            if (root.data(Qt::DisplayRole).toString().contains("empty-container")) {
                found = true;
                QCOMPARE(root.data(CountRole).toInt(), 0);
            }
        }
        QVERIFY(found);
        const auto screenshots = qEnvironmentVariable("ARCHCODEX_TEST_SCREENSHOT_DIR");
        if (!screenshots.isEmpty()) {
            tree->expandAll();
            QApplication::processEvents();
            QVERIFY(window.grab().save(screenshots + "/containers.png"));
        }
        window.close();
    }
    void myCodexWorkdirDefaultsAndSettingApplyToCachedLocalAndRemoteServers() {
        QTemporaryDir data;
        {
            Store store(data.path() + "/metadata.sqlite");
            for (const auto &id : {"local", "remote"}) {
                BackendProfile p{id, id};
                p.kind = "mycodex";
                p.autoConnect = false;
                p.containerId = QString(64, QString(id) == "local" ? 'a' : 'b');
                p.containerEpoch = "epoch";
                p.dockerContext = "default";
                p.runtimeHome = "/home/alice";
                p.runtimeUser = "alice";
                p.discoveryHost = QString(id) == "local" ? "local-mycodex" : "remote-mycodex";
                p.socketPath = "/tmp/codex.sock";
                p.identity["containerWorkdir"] = QString(id) == "local" ? "/work/a" : "/work/b";
                p.sshHost = QString(id) == "remote" ? "example-host" : "";
                QVERIFY2(p.validate().isEmpty(), qPrintable(p.validate()));
                store.save(p);
                for (const auto &path : {"/work/a", "/work/a/sub", "/work/b"})
                    store.save(
                        Session::fromThread(id, {{"id", path},
                                                 {"cwd", path},
                                                 {"name", "Shared history"},
                                                 {"path", "/saved/file"},
                                                 {"status", QJsonObject{{"type", "idle"}}}}));
            }
        }
        const auto count = [](QTreeView *tree, const QString &backend) {
            for (const auto &index : serverNodes(tree)) {
                if (index.data(BackendRole) == backend)
                    return index.data(CountRole).toInt();
            }
            return -1;
        };
        {
            Window window(data.path(), false);
            auto *tree = window.findChild<QTreeView *>();
            QTRY_COMPARE(count(tree, "local"), 2);
            QTRY_COMPARE(count(tree, "remote"), 1);
            QTimer::singleShot(0, &window, [&] {
                auto *dialog = window.findChild<SettingsDialog *>();
                QVERIFY(dialog);
                dialog->findChild<QCheckBox *>("showAllMyCodexSessions")->setChecked(true);
                QTest::mouseClick(
                    dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok),
                    Qt::LeftButton);
            });
            window.findChild<QAction *>("configureArchcodex")->trigger();
            QCOMPARE(count(tree, "local"), 3);
            QCOMPARE(count(tree, "remote"), 3);
            window.close();
        }
        Window reopened(data.path(), false);
        auto *tree = reopened.findChild<QTreeView *>();
        QTRY_COMPARE(count(tree, "local"), 3);
        QTRY_COMPARE(count(tree, "remote"), 3);
        reopened.close();
    }
    void settingsDialogAppliesExplicitlyAndPersists() {
        QTemporaryDir data;
        Window window(data.path(), false);
        window.show();
        QVERIFY(window.findChild<QAction *>("check_updates"));
        QCOMPARE(window.findChild<Updater *>()->state(), Updater::State::Idle);
        auto *action = window.findChild<QAction *>("configureArchcodex");
        auto *quickSetting = window.findChild<QAction *>("hideUnsavedSessions");
        QVERIFY(action && quickSetting);
        // Cancel must not change either setting.
        QTimer::singleShot(0, &window, [&] {
            auto *dialog = window.findChild<SettingsDialog *>();
            QVERIFY(dialog);
            dialog->findChild<QCheckBox *>("hideUnsavedSetting")->setChecked(false);
            dialog->findChild<QCheckBox *>("showAllMyCodexSessions")->setChecked(true);
            dialog->findChild<QLineEdit *>("localCodexExecutable")->setText("/cancelled/codex");
            dialog->findChild<QCheckBox *>("checkUpdatesOnStartup")->setChecked(false);
            dialog->reject();
        });
        action->trigger();
        QVERIFY(quickSetting->isChecked());
        QSettings settings(data.path() + "/window.ini", QSettings::IniFormat);
        QVERIFY(!settings.value("backends/myCodex/showAllSessions", false).toBool());
        QVERIFY(settings.value("application/localCodexExecutable").toString().isEmpty());
        QVERIFY(settings.value("updates/checkOnStartup", true).toBool());
        QTimer::singleShot(0, &window, [&] {
            auto *dialog = window.findChild<SettingsDialog *>();
            QVERIFY(dialog);
            QVERIFY(dialog->findChild<QCheckBox *>("hideUnsavedSetting")->isChecked());
            QVERIFY(!dialog->findChild<QCheckBox *>("showAllMyCodexSessions")->isChecked());
            QCOMPARE(dialog->findChild<QListWidget *>("settingsCategories")->count(), 4);
            QVERIFY(dialog->findChild<QCheckBox *>("notificationsEnabled")->isChecked());
            QVERIFY(dialog->findChild<QCheckBox *>("checkUpdatesOnStartup")->isChecked());
            dialog->findChild<QCheckBox *>("checkUpdatesOnStartup")->setChecked(false);
            dialog->findChild<QCheckBox *>("notificationsEnabled")->setChecked(false);
            dialog->findChild<QCheckBox *>("hideUnsavedSetting")->setChecked(false);
            dialog->findChild<QCheckBox *>("showAllMyCodexSessions")->setChecked(true);
            auto *buttons = dialog->findChild<QDialogButtonBox *>();
            auto *localCodex = dialog->findChild<QLineEdit *>("localCodexExecutable");
            localCodex->setText("relative/codex");
            QTest::mouseClick(buttons->button(QDialogButtonBox::Apply), Qt::LeftButton);
            QVERIFY(quickSetting->isChecked()); // Invalid paths must not apply any settings.
            localCodex->setText("/local Codex/bin/codex");
            QTest::mouseClick(buttons->button(QDialogButtonBox::Apply), Qt::LeftButton);
            QVERIFY(dialog->isVisible());
            QVERIFY(!quickSetting->isChecked());
            const auto screenshots = qEnvironmentVariable("ARCHCODEX_TEST_SCREENSHOT_DIR");
            if (!screenshots.isEmpty()) {
                dialog->findChild<QListWidget *>("settingsCategories")->setCurrentRow(0);
                QApplication::processEvents();
                QVERIFY(dialog->grab().save(screenshots + "/settings-application.png"));
                auto *categories = dialog->findChild<QListWidget *>("settingsCategories");
                categories->setCurrentItem(
                    categories->findItems("Backends", Qt::MatchExactly).first());
                QApplication::processEvents();
                QVERIFY(dialog->grab().save(screenshots + "/settings.png"));
            }
            dialog->reject(); // Explicit Apply survives a later Cancel.
        });
        action->trigger();
        settings.sync();
        QVERIFY(settings.value("backends/myCodex/showAllSessions").toBool());
        QVERIFY(!settings.value("hideUnsavedSessions").toBool());
        QVERIFY(!settings.value("notifications/enabled", true).toBool());
        QVERIFY(!settings.value("updates/checkOnStartup", true).toBool());
        QCOMPARE(settings.value("application/localCodexExecutable").toString(),
                 "/local Codex/bin/codex");
        window.close();
        Window reopened(data.path(), false);
        QVERIFY(!reopened.findChild<QAction *>("hideUnsavedSessions")->isChecked());
        QTimer::singleShot(0, &reopened, [&] {
            auto *dialog = reopened.findChild<SettingsDialog *>();
            QVERIFY(dialog);
            QVERIFY(dialog->findChild<QCheckBox *>("showAllMyCodexSessions")->isChecked());
            QVERIFY(!dialog->findChild<QCheckBox *>("checkUpdatesOnStartup")->isChecked());
            QCOMPARE(dialog->findChild<QLineEdit *>("localCodexExecutable")->text(),
                     "/local Codex/bin/codex");
            dialog->reject();
        });
        reopened.findChild<QAction *>("configureArchcodex")->trigger();
        reopened.close();
    }
    void refreshRetiresPersistedConnectionsAndPreservesReferencedViews() {
        TestHome home;
        QTemporaryDir data;
        const auto path = data.filePath("metadata.sqlite");
        {
            Store store(path);
            for (int i = 0; i < 28; ++i) {
                BackendProfile p{QString::number(i), "Automatic"};
                p.socketPath = data.filePath("missing-" + p.id);
                p.discovered = i != 23;
                p.autoConnect = i != 24;
                store.save(p);
                auto session = Session::fromThread(p.id, {{"id", "history"},
                                                          {"cwd", "/outside/workdir"},
                                                          {"path", "/real/codex/rollout"},
                                                          {"name", "Saved"}});
                store.save(session);
                if (i == 25) {
                    WorkspaceView view;
                    view.id = "remembered";
                    view.backendId = p.id;
                    view.threadId = session.id;
                    view.title = session.title;
                    view.cwd = session.cwd;
                    view.sourceIdentity = workspaceSourceIdentity(p);
                    store.save(WorkspaceSnapshot{1, {view}, {view.id}, view.id});
                }
            }
        }
        QSettings settings(data.filePath("window.ini"), QSettings::IniFormat);
        settings.setValue("application/restoreOpenedTerminals", false);
        settings.sync();
        {
            Window window(data.path(), false);
            window.show();
            QTRY_VERIFY(window.workspaceLoaded && window.cachePending.isEmpty());
            QCOMPARE(window.profiles.size(), 28);
            auto view = window.terminal->prepare(
                window.profiles["26"], window.sessions[sessionKey("26", "history")], true);
            QVERIFY(!view.id.isEmpty()); // Retained/pending terminal protects its source.
            window.pendingStarts[100] = window.backends["27"];
            // Capture saved-workspace protection before the new view's async save replaces it.
            window.refreshRevision = "refresh-one";
            window.refreshScopes = {"local", "slow-remote"};
            window.applyDiscovery({}, {}, "older-refresh");
            QCOMPARE(window.profiles.size(), 28);
            window.applyDiscovery({}, {QJsonObject{{"summary", "Permission denied"}}},
                                  "refresh-one");
            QVERIFY(window.retirements.isEmpty());
            window.refreshScopes.insert("local");
            window.applyDiscovery({QJsonObject{{"path", "relative-invalid"}}}, {}, "refresh-one");
            QVERIFY(window.retirements.isEmpty());
            window.refreshScopes.insert("local");
            window.discoveryReport.refreshIssues = {};
            window.applyDiscovery({}, {}, "refresh-one");
            QTRY_COMPARE(window.profiles.size(), 5);
            QVERIFY(window.profiles.contains("23"));
            QVERIFY(window.profiles.contains("24"));
            QVERIFY(window.profiles.contains("25"));
            QVERIFY(window.profiles.contains("26"));
            QVERIFY(window.profiles.contains("27"));
            QCOMPARE(window.terminal->workspace().selected, view.id);
            QCOMPARE(window.refreshRemoved, 23);
            QVERIFY(window.refreshScopes.contains("slow-remote"));
            QVERIFY(window.ignoredSockets.isEmpty());
            QTRY_COMPARE(Store(path).backends().size(), 5);
            QCOMPARE(Store(path).sessions().size(), 5);
            window.refreshScopeFinished("slow-remote", {"SSH timed out"});
            QVERIFY(window.refreshRevision.isEmpty());
            QVERIFY(window.discoveryReport.refreshSummary.contains("23 removed"));
            QCOMPARE(window.discoveryReport.refreshIssues.size(), 1);
            const auto *button = window.findChild<QPushButton *>("refreshConnections");
            QCOMPARE(button->text(), "Refresh");
            QVERIFY(button->toolTip().contains("local project/history cache"));
            QCOMPARE(window.findChild<Commands *>()->collection()->action("discover")->text(),
                     "Refresh connections");
            window.close();
        }
        QVERIFY(Worker::finishPending());
        Window reopened(data.path(), false);
        QTRY_VERIFY(reopened.workspaceLoaded && reopened.cachePending.isEmpty());
        QCOMPARE(reopened.profiles.size(), 5);
        // A returning local endpoint is a normal discovery, with no ignore marker.
        Fixture fixture;
        reopened.applyDiscovery({QJsonObject{{"path", fixture.path()}}}, {});
        QCOMPARE(reopened.profiles.size(), 6);
        reopened.close();
    }
    void containerRefreshRequiresFreshCompleteEndpointScopes() {
        TestHome home;
        QTemporaryDir data;
        Window window(data.path(), false);
        QTRY_VERIFY(window.workspaceLoaded && window.cachePending.isEmpty());
        const auto make = [&](const QString &id, QChar cid) {
            BackendProfile p{id, id};
            p.kind = "mycodex";
            p.discovered = true;
            p.containerId = QString(64, cid);
            p.containerEpoch = "epoch";
            p.dockerContext = "fixture";
            p.runtimeUser = "user";
            p.runtimeHome = "/home/user";
            p.socketPath = "/tmp/" + id;
            p.discoveryHost = "local-mycodex";
            p.environmentId = "env";
            p.identity = {{"dockerEngine", "engine"}, {"containerWorkdir", "/work"}};
            window.profiles[id] = p;
            window.store->save(p);
            window.attach(p, false);
            window.model->setContainerWorkdir(id, "/work");
            window.results->setContainerWorkdir(id, "/work");
            window.applySessions({Session::fromThread(
                id,
                {{"id", "hidden-cache"}, {"cwd", "/outside/workdir"}, {"path", "/real/rollout"}})});
            return p;
        };
        for (int i = 0; i < 23; ++i)
            make(i ? "healthy-" + QString::number(i) : QString("healthy"), 'a');
        make("failed", 'b');
        make("paused", 'c');
        make("initial", 'd');
        make("gone", 'e');
        make("other-engine", 'f');
        window.profiles["other-engine"].identity["dockerEngine"] = "unavailable-engine";
        window.store->save(window.profiles["other-engine"]);
        QTRY_COMPARE(Store(data.filePath("metadata.sqlite")).sessions().size(), 28);
        QVERIFY(visibleThreads(window.tree).isEmpty());
        window.refreshRevision = "fresh";
        window.refreshScopes = {"local-mycodex"};
        QJsonArray containers, ids;
        for (const auto letter : {'a', 'b', 'c', 'd'}) {
            ids.append(QString(64, letter));
            containers.append(QJsonObject{{"containerId", QString(64, letter)},
                                          {"engine", "engine"},
                                          {"containerName", QString(letter)},
                                          {"containerEpoch", "epoch"},
                                          {"endpoints", QJsonArray{}},
                                          {"issues", QJsonArray{}},
                                          {"endpointRevision", "fresh"},
                                          {"endpointComplete", true}});
        }
        auto failed = containers[1].toObject();
        failed["issues"] = QJsonArray{QJsonObject{{"summary", "Permission denied"}}};
        containers[1] = failed;
        auto paused = containers[2].toObject();
        paused["paused"] = true;
        containers[2] = paused;
        auto initial = containers[3].toObject();
        initial.remove("endpointComplete");
        containers[3] = initial;
        QJsonObject report{
            {"revision", "fresh"},
            {"complete", true},
            {"containers", containers},
            {"issues", QJsonArray{}},
            {"engines",
             QJsonObject{{"engine", QJsonObject{{"revision", "old"}, {"containerIds", ids}}}}}};
        window.applyContainers("local-mycodex", report);
        QVERIFY(window.retirements.isEmpty()); // Fresh host envelope, stale Docker scan.
        report["engines"] =
            QJsonObject{{"engine", QJsonObject{{"revision", "fresh"}, {"containerIds", ids}}}};
        auto truncatedContainers = containers;
        auto truncated = truncatedContainers[0].toObject();
        QJsonArray excessive;
        for (int i = 0; i < 4097; ++i)
            excessive.append(QJsonObject{});
        truncated["endpoints"] = excessive;
        truncatedContainers[0] = truncated;
        report["containers"] = truncatedContainers;
        window.applyContainers("local-mycodex", report);
        QTRY_COMPARE(window.profiles.size(), 27); // Only the confirmed gone container.
        QVERIFY(window.profiles.contains("healthy"));
        report["containers"] = containers;
        window.applyContainers("local-mycodex", report);
        QTRY_COMPARE(window.profiles.size(), 4);
        QVERIFY(window.profiles.contains("failed"));
        QVERIFY(window.profiles.contains("paused"));
        QVERIFY(window.profiles.contains("initial"));
        QVERIFY(window.profiles.contains("other-engine"));
        QCOMPARE(window.refreshRemoved, 24);
        QCOMPARE(Store(data.filePath("metadata.sqlite")).sessions().size(), 4);
        window.refreshScopeFinished("local-mycodex", {"Some container scopes unavailable"});
        window.close();
    }
    void returningContainerEndpointIsPersistedWithTheSameIdentity() {
        TestHome home;
        QTemporaryDir data;
        Fixture fixture;
        Window window(data.path(), false);
        QTRY_VERIFY(window.workspaceLoaded && window.cachePending.isEmpty());
        const QString cid(64, 'a');
        QJsonObject container{{"containerId", cid},
                              {"engine", "engine"},
                              {"containerName", "fixture"},
                              {"containerEpoch", "epoch"},
                              {"dockerContext", "fixture"},
                              {"runtimeUser", "user"},
                              {"runtimeHome", "/home/user"},
                              {"environment", "mounts"},
                              {"cwd", "/workspace"},
                              {"endpointRevision", "fresh"},
                              {"endpointComplete", true},
                              {"issues", QJsonArray{}},
                              {"endpoints", QJsonArray{QJsonObject{{"path", fixture.path()},
                                                                   {"localSocket", fixture.path()},
                                                                   {"executable", "codex"}}}}};
        QJsonObject inventory{
            {"revision", "fresh"},
            {"containers", QJsonArray{container}},
            {"complete", true},
            {"issues", QJsonArray{}},
            {"engines", QJsonObject{{"engine", QJsonObject{{"revision", "fresh"},
                                                           {"containerIds", QJsonArray{cid}}}}}}};
        window.applyContainers("local-mycodex", inventory);
        QCOMPARE(window.profiles.size(), 1);
        const auto id = window.profiles.cbegin()->id;
        window.refreshRevision = "fresh";
        window.refreshScopes = {"local-mycodex"};
        auto absent = container;
        absent["endpoints"] = QJsonArray{};
        inventory["containers"] = QJsonArray{absent};
        window.applyContainers("local-mycodex", inventory);
        QTRY_VERIFY(window.profiles.isEmpty());
        QVERIFY(Store(data.filePath("metadata.sqlite")).backends().isEmpty());
        QVERIFY(window.ignoredSockets.isEmpty());
        inventory["containers"] = QJsonArray{container};
        window.applyContainers("local-mycodex", inventory);
        QCOMPARE(window.profiles.size(), 1);
        QCOMPARE(window.profiles.cbegin()->id, id);
        QTRY_COMPARE(Store(data.filePath("metadata.sqlite")).backends().size(), 1);
        window.refreshScopeFinished("local-mycodex");
        window.close();
    }
    void refreshKeepsTerminalInputWorkingWhileRemoteScopeIsPending() {
        PtyProbe probe;
        QVERIFY(probe.install());
        Fixture fixture;
        QTemporaryDir data;
        auto profile = fixture.profile();
        profile.executable = probe.executable();
        {
            Store store(data.filePath("metadata.sqlite"));
            store.save(profile);
        }
        Window window(data.path(), false);
        window.show();
        window.activateWindow();
        QTRY_VERIFY(window.workspaceLoaded && window.cachePending.isEmpty());
        auto *backend = window.backends[profile.id];
        backend->connectServer();
        QTRY_VERIFY(backend->ready());
        window.openSession(profile.id, Session::fromThread(profile.id, fixture.thread()));
        QTRY_VERIFY(!probe.records("thread-1").isEmpty());
        const auto key = window.terminal->currentKey();
        const auto before = probe.records("thread-1").size();
        QVERIFY(window.terminal->focusSession(key));
        auto *focus = QApplication::focusWidget();
        QVERIFY(focus && window.terminal->isAncestorOf(focus));
        window.refreshRevision = "fresh";
        window.refreshScopes = {"local", "remote"};
        window.applyDiscovery({}, {}, "fresh");
        QVERIFY(window.refreshScopes.contains("remote"));
        QTest::keyClicks(focus, "s");
        QTRY_VERIFY_WITH_TIMEOUT(probe.records("thread-1").size() > before, 1000);
        QCOMPARE(QApplication::focusWidget(), focus);
        QCOMPARE(window.terminal->currentKey(), key);
        QVERIFY(backend->ready());
        window.refreshScopeFinished("remote", {"SSH unavailable"});
        window.close();
    }
    void retirementFailureIsVisibleAndDoesNotRemoveRows() {
        TestHome home;
        QTemporaryDir data;
        Window window(data.path(), false);
        QTRY_VERIFY(window.workspaceLoaded && window.cachePending.isEmpty());
        BackendProfile p{"automatic", "Automatic"};
        p.socketPath = data.filePath("absent");
        p.discovered = true;
        window.profiles[p.id] = p;
        window.attach(p, false);
        window.store->save(p);
        QTRY_COMPARE(Store(data.filePath("metadata.sqlite")).backends().size(), 1);
        const auto name = QUuid::createUuid().toString();
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", name);
            db.setDatabaseName(data.filePath("metadata.sqlite"));
            QVERIFY(db.open());
            QSqlQuery query(db);
            QVERIFY(query.exec("CREATE TRIGGER deny_delete BEFORE DELETE ON backends BEGIN "
                               "SELECT RAISE(FAIL, 'fixture deletion denied'); END"));
        }
        QSqlDatabase::removeDatabase(name);
        window.refreshRevision = "fresh";
        window.refreshScopes = {"local"};
        window.applyDiscovery({}, {}, "fresh");
        QTRY_VERIFY(window.retirements.isEmpty());
        QCOMPARE(window.profiles.size(), 1);
        QCOMPARE(Store(data.filePath("metadata.sqlite")).backends().size(), 1);
        QCOMPARE(window.refreshRemoved, 0);
        QCOMPARE(window.discoveryReport.refreshIssues.size(), 1);
        QVERIFY(window.discoveryReport.refreshIssues.first()
                    .toObject()["details"]
                    .toArray()
                    .first()
                    .toString()
                    .contains("fixture deletion denied"));
        QVERIFY(!QApplication::activeModalWidget());
        window.close();
    }
    void retirementCancellationAndFailureKeepProfiles() {
        TestHome home;
        QTemporaryDir data;
        Window window(data.path(), false);
        QTRY_VERIFY(window.workspaceLoaded && window.cachePending.isEmpty());
        BackendProfile p{"retired", "retired"};
        p.discovered = true;
        p.socketPath = data.filePath("missing");
        window.profiles[p.id] = p;
        window.attach(p, false);
        window.store->save(p);
        auto session = Session::fromThread(
            p.id, {{"id", "saved"}, {"cwd", "/work"}, {"path", "/real/rollout"}});
        window.applySessions({session});
        window.refreshRevision = "first";
        window.refreshScopes = {"remote"};
        window.retireConnection(p.id);
        QVERIFY(window.retirements.contains(p.id));
        window.refreshScopeFinished("remote");
        QVERIFY(!window.refreshRevision.isEmpty());
        window.openSession(p.id, session); // Cancels even if the database already deleted it.
        QVERIFY(window.retirements.isEmpty());
        QTRY_COMPARE(Store(data.filePath("metadata.sqlite")).sessions().size(), 1);
        QTRY_VERIFY(window.refreshRevision.isEmpty());
        QVERIFY(window.profiles.contains(p.id));
        QVERIFY(!window.terminal->view(sessionKey(p.id, session.id)).id.isEmpty());
        window.refreshRevision = "second";
        window.refreshScopes = {"remote"};
        window.discover();
        window.discover();
        QVERIFY(window.refreshQueued); // Repeated requests coalesce, without starting helpers.
        window.refreshQueued = false;
        window.refreshScopeFinished("remote", {"Unavailable"});
        window.close();
    }
    void discoveryFeedbackIsActionableAndDoesNotInterrupt() {
        Fixture fixture;
        QTemporaryDir data;
        Window window(data.path(), false);
        window.show();
        auto *discovery = window.findChild<HostDiscovery *>();
        auto *feedback = window.findChild<KSqueezedTextLabel *>("searchFeedback");
        auto *status = window.findChild<QToolButton *>("discoveryStatus");
        QVERIFY(feedback && feedback->isHidden());
        QVERIFY(status && status->isHidden());
        const QJsonArray endpoints{QJsonObject{{"path", fixture.path()},
                                               {"default", true},
                                               {"user", "alice"},
                                               {"host", "workstation"}}};
        const QJsonArray issues{QJsonObject{
            {"code", "unidentified-access"},
            {"summary", "2 processes could not be identified"},
            {"explanation",
             "Linux denied access to executable links. Archcodex cannot tell whether they are "
             "Codex; this does not prove any sessions are missing."},
            {"action", "If all expected servers are listed, no action is needed. Otherwise, add "
                       "the missing server's Unix socket in Connections. "
                       "Retry after resolving access to that process."},
            {"details",
             QJsonArray{"/proc/4201/exe — read executable link: Permission denied (EACCES)",
                        "/proc/4202/exe — read executable link: Permission denied (EACCES)"}}}};
        discovery->snapshot(endpoints, issues);
        QVERIFY(feedback->isHidden());
        QVERIFY(status->isVisible());
        QCOMPARE(status->text(), "Discovery limited");
        QVERIFY(!QApplication::activeModalWidget());
        QVERIFY(!window.findChild<DiscoveryDialog *>());
        QTest::mouseClick(status, Qt::LeftButton);
        QPointer<DiscoveryDialog> dialog = window.findChild<DiscoveryDialog *>();
        QVERIFY(dialog && dialog->isVisible() && !dialog->isModal());
        QVERIFY(!QApplication::activeModalWidget());
        auto *technical = dialog->findChild<QPlainTextEdit *>();
        QVERIFY(technical && technical->isHidden());
        QTest::mouseClick(dialog->findChild<QToolButton *>(), Qt::LeftButton);
        QVERIFY(technical->isVisible());
        QVERIFY(technical->toPlainText().contains("/proc/4201/exe"));
        QTest::mouseClick(dialog->findChild<QPushButton *>("copyDiscoveryReport"), Qt::LeftButton);
        QVERIFY(QApplication::clipboard()->text().contains("EACCES"));
        QVERIFY(
            QApplication::clipboard()->text().contains("does not prove any sessions are missing"));
        discovery->snapshot(endpoints, issues);
        QCOMPARE(window.findChildren<DiscoveryDialog *>().size(), 1);
        QCOMPARE(dialog->findChild<QPlainTextEdit *>(), technical);
        QVERIFY(technical->isVisible());
        const auto screenshots = qEnvironmentVariable("ARCHCODEX_TEST_SCREENSHOT_DIR");
        if (!screenshots.isEmpty()) {
            QApplication::processEvents();
            QVERIFY(dialog->grab().save(screenshots + "/discovery-details.png"));
            for (auto *backend : window.findChildren<Backend *>())
                backend->sessionChanged(Session::fromThread(backend->profile.id, fixture.thread()));
            window.findChild<QTreeView *>()->expandAll();
            QVERIFY(window.grab().save(screenshots + "/discovery-status.png"));
        }
        discovery->snapshot(endpoints, {});
        QVERIFY(status->isHidden());
        QVERIFY(feedback->isHidden());
        QVERIFY(!dialog->findChild<QPlainTextEdit *>());
        QTest::keyClick(dialog, Qt::Key_Escape);
        QTRY_VERIFY(!dialog);
        discovery->failed("Cannot start host discovery: No such file or directory");
        QCOMPARE(status->text(), "Discovery failed");
        QVERIFY(status->isVisible());
        QVERIFY(!window.findChild<DiscoveryDialog *>());
        QVERIFY(feedback->isHidden());
        window.close();
    }
    void discoveryActionsAndProgress() {
        DiscoveryDialog dialog;
        QSignalSpy retry(&dialog, &DiscoveryDialog::retryRequested);
        QSignalSpy add(&dialog, &DiscoveryDialog::addConnectionRequested);
        DiscoveryReport report;
        report.checked = QDateTime::currentDateTime();
        report.failure = "Example helper failure";
        dialog.setReport(report);
        dialog.show();
        auto *retryButton = dialog.findChild<QPushButton *>("retryDiscovery");
        QTest::mouseClick(retryButton, Qt::LeftButton);
        QCOMPARE(retry.size(), 1);
        report.inProgress = true;
        dialog.setReport(report);
        QVERIFY(!retryButton->isEnabled());
        for (auto *button : dialog.findChildren<QPushButton *>())
            if (button->text() == "Add connection…")
                QTest::mouseClick(button, Qt::LeftButton);
        QCOMPARE(add.size(), 1);
        report.inProgress = false;
        report.failure.clear();
        report.endpoints = 2;
        dialog.setReport(report);
        QVERIFY(retryButton->isEnabled());
        bool healthy = false;
        for (auto *label : dialog.findChildren<QLabel *>())
            healthy |= label->text().contains("No discovery access problems");
        QVERIFY(healthy);
    }
    void viewsFollowEventsAndPreserveNavigation() {
        Fixture fixture;
        QTemporaryDir data;
        Window window(data.path(), false);
        window.show();
        window.findChild<HostDiscovery *>()->snapshot(
            {QJsonObject{{"path", fixture.path()}, {"hostEnvironment", true}}}, {});
        auto *tree = window.findChild<QTreeView *>();
        auto *tabs = window.findChild<QTabBar *>();
        QCOMPARE(tabs->count(), 5);
        QTRY_COMPARE(serverNodes(tree).size(), 1);
        auto server = [&] { return serverNode(tree); };
        auto project = [&] { return tree->model()->index(0, 0, server()); };
        QTRY_VERIFY(project().isValid());
        QTRY_COMPARE(tree->model()->index(0, 0, project()).data(ThreadRole).toString(), "thread-1");
        tree->expand(project());
        tree->setCurrentIndex(tree->model()->index(0, 0, project()));
        tabs->setCurrentIndex(1);
        QCOMPARE(serverNodes(tree).size(), 1);
        QVERIFY(tree->isExpanded(project()));
        QCOMPARE(tree->currentIndex().data(ThreadRole).toString(), "thread-1");
        tabs->setCurrentIndex(2);
        QCOMPARE(serverNodes(tree).size(), 0);
        tabs->setCurrentIndex(3);
        QCOMPARE(serverNodes(tree).size(), 1);
        fixture.notify(
            "thread/status/changed",
            {{"threadId", "thread-1"},
             {"status", QJsonObject{{"type", "active"}, {"activeFlags", QJsonArray{}}}}});
        QTRY_COMPARE(serverNodes(tree).size(), 0);
        tabs->setCurrentIndex(2);
        QCOMPARE(serverNodes(tree).size(), 1);
        tabs->setCurrentIndex(1);
        // Tooltips keep a stable snapshot while events arrive under the pointer.
        const auto pos = tree->visualRect(project()).center();
        QHelpEvent help(QEvent::ToolTip, pos, tree->viewport()->mapToGlobal(pos));
        QApplication::sendEvent(tree->viewport(), &help);
        QVERIFY(QToolTip::isVisible());
        const auto tip = QToolTip::text();
        fixture.notify("thread/status/changed",
                       {{"threadId", "thread-1"}, {"status", QJsonObject{{"type", "idle"}}}});
        tabs->setCurrentIndex(1);
        QTRY_VERIFY(tree->model()->index(0, 0, project()).data(WaitingRole).toBool());
        QApplication::sendEvent(tree->viewport(), &help);
        QCOMPARE(QToolTip::text(), tip);
        QToolTip::hideText();
        fixture.notify("thread/closed", {{"threadId", "thread-1"}});
        QTRY_COMPARE(serverNodes(tree).size(), 0);
        tabs->setCurrentIndex(0);
        QCOMPARE(serverNodes(tree).size(), 1);
        window.close();
    }
    void activityViewsExpandWithoutOverridingManualCollapse() {
        QTemporaryDir data;
        QVERIFY(data.isValid());
        {
            Store store(data.path() + "/metadata.sqlite");
            store.save(BackendProfile{"b", "Main"});
        }
        Window window(data.path(), false);
        window.show();
        QTRY_VERIFY(window.findChild<Backend *>());
        auto *backend = window.findChild<Backend *>();
        auto *tree = window.findChild<QTreeView *>();
        auto *tabs = window.findChild<QTabBar *>();
        QVERIFY(backend);
        QCOMPARE(tabs->tabText(3), "Waiting");
        auto put = [&](const QString &id, const QString &cwd, const QString &type) {
            auto s = Session::fromThread(
                "b", {{"id", id},
                      {"cwd", cwd},
                      {"status", QJsonObject{{"type", type}, {"activeFlags", QJsonArray{}}}}});
            s.title = id + " session";
            backend->sessionChanged(s);
            return s;
        };
        put("old-home", data.filePath("home"), "notLoaded");
        put("old-user", data.filePath("home/user"), "notLoaded");
        put("old-documents", data.filePath("home/user/Documents"), "notLoaded");
        auto waiting = put("waiting", data.filePath("home/user/Documents/Chats"), "idle");
        put("running", data.filePath("srv/work/build"), "active");
        auto root = [&] { return serverNode(tree); };
        auto folder = [&] { return tree->model()->index(0, 0, root()); };
        for (const auto tab : {2, 3}) {
            tabs->setCurrentIndex(tab);
            QCOMPARE(root().parent().data(KindRole).toInt(), int(NodeKind::Group));
            QVERIFY(!root().parent().parent().isValid());
            QVERIFY(tree->isExpanded(root().parent()));
            QVERIFY(tree->isExpanded(root()));
            QVERIFY(tree->isExpanded(folder()));
            QCOMPARE(folder().data(Qt::DisplayRole).toString(), tab == 2 ? "build" : "Chats");
            const auto screenshots = qEnvironmentVariable("ARCHCODEX_TEST_SCREENSHOT_DIR");
            if (!screenshots.isEmpty()) {
                QApplication::processEvents();
                QVERIFY(
                    window.grab().save(screenshots + (tab == 2 ? "/running.png" : "/waiting.png")));
            }
        }
        tree->collapse(folder());
        QVERIFY(!tree->isExpanded(folder()));
        tabs->setCurrentIndex(0);
        tabs->setCurrentIndex(3);
        QVERIFY(!tree->isExpanded(folder()));
        waiting.title = "Updated title";
        backend->sessionChanged(waiting);
        QVERIFY(!tree->isExpanded(folder()));
        auto fresh = put("fresh", data.filePath("home/user/Documents/other/nested"), "idle");
        bool sawNewBranch = false;
        for (int i = 0; i < tree->model()->rowCount(root()); ++i) {
            auto branch = tree->model()->index(i, 0, root());
            if (branch.data(PathRole) == fresh.cwd) {
                QVERIFY(tree->isExpanded(branch));
                sawNewBranch = true;
            } else if (branch.data(PathRole) == waiting.cwd)
                QVERIFY(!tree->isExpanded(branch));
        }
        QVERIFY(sawNewBranch);
        // Leaving and rejoining Waiting must keep the explicit collapse for its stable path.
        fresh.status = {{"type", "notLoaded"}};
        backend->sessionChanged(fresh);
        QCOMPARE(folder().data(PathRole).toString(), waiting.cwd);
        QVERIFY(!tree->isExpanded(folder()));
        tree->expand(folder());
        tree->setCurrentIndex(tree->model()->index(0, 0, folder()));
        tabs->setCurrentIndex(1);
        QCOMPARE(tree->currentIndex().data(ThreadRole).toString(), "waiting");
        tabs->setCurrentIndex(3);
        QCOMPARE(tree->currentIndex().data(ThreadRole).toString(), "waiting");
        QVERIFY(tree->isExpanded(folder()));
        // A local query must restore the tab tree, selection and expansion when cleared.
        auto *query = window.findChild<QLineEdit *>("sessionSearch");
        query->setText("needle");
        QTest::keyClick(query, Qt::Key_Return);
        query->clear();
        QCOMPARE(folder().data(Qt::DisplayRole).toString(), "Chats");
        QVERIFY(tree->isExpanded(root()) && tree->isExpanded(folder()));
        QCOMPARE(tree->currentIndex().data(ThreadRole).toString(), "waiting");
        window.close();
    }
    void hideUnsavedSettingAndClientError() {
        Fixture fixture;
        fixture.threadFields = {{"path", QJsonValue::Null}, {"ephemeral", true}};
        QTemporaryDir data;
        {
            Window window(data.path(), false);
            window.show();
            window.findChild<HostDiscovery *>()->snapshot({QJsonObject{{"path", fixture.path()}}},
                                                          {});
            auto *tree = window.findChild<QTreeView *>();
            auto *tabs = window.findChild<QTabBar *>();
            auto *setting = window.findChild<QAction *>("hideUnsavedSessions");
            QVERIFY(setting && setting->isChecked());
            QTRY_VERIFY(fixture.methods.contains("thread/read"));
            QTRY_COMPARE(serverNodes(tree).size(), 1);
            QCOMPARE(serverNode(tree).data(CountRole).toInt(), 0);
            setting->setChecked(false);
            QTRY_COMPARE(serverNode(tree).data(CountRole).toInt(), 1);
            auto session = Session::fromThread(serverNode(tree).data(BackendRole).toString(),
                                               fixture.thread());
            session.terminalOpen = true;
            session.clientIssue = "Terminal client exited with code 1";
            window.findChild<TerminalArea *>()->clientStateChanged(session);
            setting->setChecked(true);
            tabs->setCurrentIndex(1);
            QCOMPARE(serverNodes(tree).size(), 1);
            tabs->setCurrentIndex(2);
            QCOMPARE(serverNodes(tree).size(), 0);
            tabs->setCurrentIndex(3);
            QCOMPARE(serverNodes(tree).size(), 0);
            tabs->setCurrentIndex(int(SessionView::Opened));
            QCOMPARE(serverNodes(tree).size(), 1);
            session.terminalOpen = false;
            session.clientIssue.clear();
            window.findChild<TerminalArea *>()->clientStateChanged(session);
            QCOMPARE(serverNodes(tree).size(), 0);
            tabs->setCurrentIndex(1);
            QCOMPARE(serverNodes(tree).size(), 0);
            setting->setChecked(false);
            window.close();
        }
        Window reopened(data.path(), false);
        QVERIFY(!reopened.findChild<QAction *>("hideUnsavedSessions")->isChecked());
        reopened.close();
    }
    void activationChecksOnlyTheSelectedServer() {
        Fixture first, second;
        QTemporaryDir data;
        Window window(data.path(), false);
        window.show();
        window.findChild<HostDiscovery *>()->snapshot(
            {QJsonObject{{"path", first.path()}}, QJsonObject{{"path", second.path()}}}, {});
        auto *tree = window.findChild<QTreeView *>();
        QTRY_COMPARE(first.readIds.size(), 1);
        QTRY_COMPARE(second.readIds.size(), 1);
        QTRY_COMPARE(serverNodes(tree).size(), 2);
        Store store(data.path() + "/metadata.sqlite");
        QString firstId;
        for (const auto &p : store.backends())
            if (p.socketPath == first.path())
                firstId = p.id;
        QVERIFY(!firstId.isEmpty());
        QModelIndex server;
        for (int i = 0; i < serverNodes(tree).size(); ++i)
            if (serverNode(tree, i).data(BackendRole) == firstId)
                server = serverNode(tree, i);
        QVERIFY(server.isValid());
        auto session = tree->model()->index(0, 0, tree->model()->index(0, 0, server));
        QCOMPARE(session.data(ThreadRole).toString(), "thread-1");
        // A rejected preflight must not launch a terminal or fall back to the other server.
        first.readErrors["thread-1"] = "thread not loaded: thread-1";
        QSignalSpy terminalStates(window.findChild<TerminalArea *>(),
                                  &TerminalArea::clientStateChanged);
        auto *area = window.findChild<TerminalArea *>();
        tree->setCurrentIndex(session);
        tree->setFocus();
        QTest::keyClick(tree, Qt::Key_Return);
        QTRY_VERIFY(area->currentSession().clientIssue.contains("thread not loaded: thread-1"));
        QCOMPARE(first.readIds.size(), 2);
        QCOMPARE(second.readIds.size(), 1);
        QVERIFY(!terminalStates.isEmpty()); // Failure remains visible in a placeholder.
        QCOMPARE(area->findChildren<KParts::ReadOnlyPart *>().size(), 0);
        QVERIFY(window.findChildren<QMessageBox *>().isEmpty());
        QCOMPARE(area->workspace().views.first().disposition, ViewDisposition::Disconnected);
        // Retry uses the same exact selected-server preflight and same view.
        const auto stableId = area->workspace().views.first().id;
        const auto client = Session::fromThread(firstId, first.thread());
        area->restartRequested(client);
        QTRY_COMPARE(first.readIds.size(), 3);
        QTRY_VERIFY(area->currentSession().clientIssue.contains("thread not loaded: thread-1"));
        QCOMPARE(second.readIds.size(), 1);
        QCOMPARE(area->workspace().views.first().id, stableId);
        QCOMPARE(area->findChildren<KParts::ReadOnlyPart *>().size(), 0);
        QVERIFY(window.findChildren<QMessageBox *>().isEmpty());
        window.close();
    }
    void discoveryOpensExistingSessionsWithoutProjectSetup() {
        Fixture first, second;
        first.loaded = {"older-loaded-session"};
        QTemporaryDir data;
        Window window(data.path(), false);
        window.show();
        auto *discovery = window.findChild<HostDiscovery *>();
        auto *tree = window.findChild<QTreeView *>();
        QVERIFY(discovery && tree);
        const auto endpoint = [](const Fixture &fixture) {
            return QJsonObject{{"path", fixture.path()},
                               {"aliases", QJsonArray{}},
                               {"executable", "codex"},
                               {"hostEnvironment", true}};
        };
        const QJsonArray endpoints{endpoint(first), endpoint(second)};
        discovery->snapshot(endpoints, {});
        QTRY_COMPARE_WITH_TIMEOUT(serverNodes(tree).size(), 2, 5000);
        auto project = [&] { return serverNode(tree); };
        QTRY_COMPARE_WITH_TIMEOUT(serverNode(tree).data(CountRole).toInt() +
                                      serverNode(tree, 1).data(CountRole).toInt(),
                                  3, 5000);
        QVERIFY(first.methods.contains("thread/loaded/list"));
        QVERIFY(first.methods.contains("thread/read"));
        QVERIFY(!first.methods.contains("thread/start"));
        QVERIFY(!first.methods.contains("thread/resume"));
        // The same discovery result does not reconnect or duplicate sessions.
        const auto requests = first.methods.size();
        discovery->snapshot(endpoints, {});
        QTest::qWait(100);
        QCOMPARE(first.methods.size(), requests);
        QCOMPARE(serverNodes(tree).size(), 2);
        // A selected server searches only that server, even on a shared host.
        tree->setCurrentIndex(project());
        window.findChild<QComboBox *>()->setCurrentIndex(1);
        auto *query = window.findChild<QLineEdit *>("sessionSearch");
        query->setText("needle");
        QTest::keyClick(query, Qt::Key_Return);
        QTRY_COMPARE(first.methods.count("thread/search") + second.methods.count("thread/search"),
                     2);
        QVERIFY(first.methods.count("thread/search") == 0 ||
                second.methods.count("thread/search") == 0);
        QTRY_COMPARE(serverNodes(tree).size(), 1);
        QTRY_COMPARE(serverNode(tree).data(CountRole).toInt(), 2);
        // The parent group searches every server it contains, without losing
        // the backend identities retained on its children.
        query->clear();
        tree->setCurrentIndex(tree->model()->index(0, 0));
        query->setText("group search");
        QTest::keyClick(query, Qt::Key_Return);
        QTRY_COMPARE(first.methods.count("thread/search") + second.methods.count("thread/search"),
                     6);
        QTRY_COMPARE(serverNodes(tree).size(), 2);
        window.close();
    }
};
QTEST_MAIN(WindowTests)
#include "test_window.moc"
