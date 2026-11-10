// SPDX-License-Identifier: GPL-3.0-or-later
#include "commands.h"
#include "discovery.h"
#include "discoverydialog.h"
#include "fixture.h"
#include "sessionmodel.h"
#include "settingsdialog.h"
#include "terminal.h"
#include "window.h"
#include "worker.h"
#include <KCommandBar>
#include <KShortcutsEditor>
#include <QAction>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QHelpEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QTabBar>
#include <QToolButton>
#include <QToolTip>
#include <QTreeView>
#include <QtTest>

namespace {
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

class WindowTests : public QObject {
    Q_OBJECT
  private slots:
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
            auto *tabs = window.findChild<QTabBar *>();
            tabs->setCurrentIndex(int(SessionView::Opened));
            auto *tree = window.findChild<QTreeView *>();
            QTRY_COMPARE(visibleThreads(tree), QStringList{v.threadId});
            const auto screenshots = qEnvironmentVariable("ARCHCODEX_TEST_SCREENSHOT_DIR");
            if (!screenshots.isEmpty())
                QVERIFY(window.grab().save(screenshots + "/workspace-placeholder.png"));
            auto *backend = window.findChild<Backend *>();
            QVERIFY(backend);
            backend->connectServer();
            QTRY_VERIFY(area->hasClient(v.key()));
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
        auto *query = window.findChild<QLineEdit *>();
        auto *tabs = window.findChild<QTabBar *>();
        auto *terminal = window.findChild<TerminalArea *>();
        auto *commands = window.findChild<Commands *>();
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
            dialog->findChild<QListWidget *>("settingsCategories")->setCurrentRow(2);
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
        auto *query = window.findChild<QLineEdit *>();
        auto *scope = window.findChild<QComboBox *>();
        auto *feedback = window.findChild<QLabel *>("searchFeedback");
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
        QVERIFY(feedback->text().contains("4 matching sessions"));
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
        QVERIFY(feedback->text().contains(waiting.cwd));
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
        auto *query = window.findChild<QLineEdit *>();
        auto *feedback = window.findChild<QLabel *>("searchFeedback");
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
        QVERIFY(feedback->text().contains("cached titles"));
        tabs->setCurrentIndex(0);
        QCOMPARE(query->text(), "conversation text");
        QTRY_COMPARE(visibleThreads(tree), QStringList{"found"});
        QVERIFY(!feedback->text().contains("searching"));
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
        QVERIFY(!feedback->text().contains("old error"));
        query->setText("interrupted search");
        QTest::keyClick(query, Qt::Key_Return);
        QTRY_COMPARE(fixture.searchIds.size(), 4);
        tabs->setCurrentIndex(3);
        fixture.peer->close();
        QTRY_VERIFY(!backend->ready());
        QCOMPARE(query->text(), "example");
        QVERIFY(!feedback->text().contains("interrupted"));
        tabs->setCurrentIndex(0);
        QVERIFY(feedback->text().contains("interrupted", Qt::CaseInsensitive));
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
    void localHostAndContainerHostsHaveSeparateGroupsWithShortLabels() {
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
        QTRY_COMPARE(tree->model()->rowCount(), 3);
        QCOMPARE(tree->model()->index(0, 0).data().toString(), "alice @ workstation");
        QCOMPARE(tree->model()->index(1, 0).data().toString(), "myCodex @ workstation");
        QSet<QString> labels;
        for (int i = 0; i < tree->model()->rowCount(); ++i) {
            const auto group = tree->model()->index(i, 0);
            QCOMPARE(group.data(KindRole).toInt(), int(NodeKind::Group));
            labels.insert(group.data(Qt::DisplayRole).toString());
            QCOMPARE(tree->model()->rowCount(group), 1);
            const auto server = tree->model()->index(0, 0, group);
            QCOMPARE(server.data(Qt::DisplayRole).toString(),
                     server.data(BackendRole) == "host" ? QString("Main") : QString("project"));
        }
        QCOMPARE(labels, (QSet<QString>{"alice @ workstation", "myCodex @ workstation",
                                        "myCodex @ remote-machine"}));
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
            QCOMPARE(tree->model()->rowCount(), 3);
            for (int i = 0; i < 3; ++i) {
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
        auto *discovery = window.findChild<ContainerDiscovery *>();
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
            dialog->reject();
        });
        action->trigger();
        QVERIFY(quickSetting->isChecked());
        QSettings settings(data.path() + "/window.ini", QSettings::IniFormat);
        QVERIFY(!settings.value("backends/myCodex/showAllSessions", false).toBool());
        QVERIFY(settings.value("application/localCodexExecutable").toString().isEmpty());
        QTimer::singleShot(0, &window, [&] {
            auto *dialog = window.findChild<SettingsDialog *>();
            QVERIFY(dialog);
            QVERIFY(dialog->findChild<QCheckBox *>("hideUnsavedSetting")->isChecked());
            QVERIFY(!dialog->findChild<QCheckBox *>("showAllMyCodexSessions")->isChecked());
            QCOMPARE(dialog->findChild<QListWidget *>("settingsCategories")->count(), 3);
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
                dialog->findChild<QListWidget *>("settingsCategories")->setCurrentRow(1);
                QApplication::processEvents();
                QVERIFY(dialog->grab().save(screenshots + "/settings.png"));
            }
            dialog->reject(); // Explicit Apply survives a later Cancel.
        });
        action->trigger();
        settings.sync();
        QVERIFY(settings.value("backends/myCodex/showAllSessions").toBool());
        QVERIFY(!settings.value("hideUnsavedSessions").toBool());
        QCOMPARE(settings.value("application/localCodexExecutable").toString(),
                 "/local Codex/bin/codex");
        window.close();
        Window reopened(data.path(), false);
        QVERIFY(!reopened.findChild<QAction *>("hideUnsavedSessions")->isChecked());
        QTimer::singleShot(0, &reopened, [&] {
            auto *dialog = reopened.findChild<SettingsDialog *>();
            QVERIFY(dialog);
            QVERIFY(dialog->findChild<QCheckBox *>("showAllMyCodexSessions")->isChecked());
            QCOMPARE(dialog->findChild<QLineEdit *>("localCodexExecutable")->text(),
                     "/local Codex/bin/codex");
            dialog->reject();
        });
        reopened.findChild<QAction *>("configureArchcodex")->trigger();
        reopened.close();
    }
    void discoveryFeedbackIsActionableAndDoesNotInterrupt() {
        Fixture fixture;
        QTemporaryDir data;
        Window window(data.path(), false);
        window.show();
        auto *discovery = window.findChild<HostDiscovery *>();
        auto *feedback = window.findChild<QLabel *>("searchFeedback");
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
        auto *query = window.findChild<QLineEdit *>();
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
        tree->activated(session);
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
        auto *query = window.findChild<QLineEdit *>();
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
