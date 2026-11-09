// SPDX-License-Identifier: GPL-3.0-or-later
#include "discovery.h"
#include "discoverydialog.h"
#include "fixture.h"
#include "sessionmodel.h"
#include "settingsdialog.h"
#include "terminal.h"
#include "window.h"
#include <QAction>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDialogButtonBox>
#include <QHelpEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QTabBar>
#include <QToolButton>
#include <QToolTip>
#include <QTreeView>
#include <QtTest>

class WindowTests : public QObject {
    Q_OBJECT
  private slots:
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
        QCOMPARE(tree->model()->rowCount(), 2);
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
        QCOMPARE(tree->model()->rowCount(), 1);
        QCOMPARE(tree->model()->index(0, 0).data(BackendRole).toString(), ids[0]);
        tabs->setCurrentIndex(0);
        auto empty = containers[0].toObject();
        empty["containerId"] = QString(64, 'c');
        empty["containerName"] = "empty-container";
        empty["endpoints"] = QJsonArray{};
        containers << empty;
        discovery->snapshot(
            {{"containers", containers}, {"issues", QJsonArray{}}, {"complete", true}});
        QCOMPARE(tree->model()->rowCount(), 3);
        bool found = false;
        for (int i = 0; i < tree->model()->rowCount(); ++i) {
            const auto root = tree->model()->index(i, 0);
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
            auto *model = tree->model();
            for (int i = 0; i < model->rowCount(); ++i) {
                const auto index = model->index(i, 0);
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
            dialog->reject();
        });
        action->trigger();
        QVERIFY(quickSetting->isChecked());
        QSettings settings(data.path() + "/window.ini", QSettings::IniFormat);
        QVERIFY(!settings.value("backends/myCodex/showAllSessions", false).toBool());
        QTimer::singleShot(0, &window, [&] {
            auto *dialog = window.findChild<SettingsDialog *>();
            QVERIFY(dialog);
            QVERIFY(dialog->findChild<QCheckBox *>("hideUnsavedSetting")->isChecked());
            QVERIFY(!dialog->findChild<QCheckBox *>("showAllMyCodexSessions")->isChecked());
            QCOMPARE(dialog->findChild<QListWidget *>("settingsCategories")->count(), 2);
            dialog->findChild<QCheckBox *>("hideUnsavedSetting")->setChecked(false);
            dialog->findChild<QCheckBox *>("showAllMyCodexSessions")->setChecked(true);
            auto *buttons = dialog->findChild<QDialogButtonBox *>();
            QTest::mouseClick(buttons->button(QDialogButtonBox::Apply), Qt::LeftButton);
            QVERIFY(dialog->isVisible());
            QVERIFY(!quickSetting->isChecked());
            const auto screenshots = qEnvironmentVariable("ARCHCODEX_TEST_SCREENSHOT_DIR");
            if (!screenshots.isEmpty()) {
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
        window.close();
        Window reopened(data.path(), false);
        QVERIFY(!reopened.findChild<QAction *>("hideUnsavedSessions")->isChecked());
        QTimer::singleShot(0, &reopened, [&] {
            auto *dialog = reopened.findChild<SettingsDialog *>();
            QVERIFY(dialog);
            QVERIFY(dialog->findChild<QCheckBox *>("showAllMyCodexSessions")->isChecked());
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
        QCOMPARE(tabs->count(), 4);
        QTRY_COMPARE(tree->model()->rowCount(), 1);
        auto server = [&] { return tree->model()->index(0, 0); };
        auto project = [&] { return tree->model()->index(0, 0, server()); };
        QTRY_VERIFY(project().isValid());
        QTRY_COMPARE(tree->model()->index(0, 0, project()).data(ThreadRole).toString(), "thread-1");
        tree->expand(project());
        tree->setCurrentIndex(tree->model()->index(0, 0, project()));
        tabs->setCurrentIndex(1);
        QCOMPARE(tree->model()->rowCount(), 1);
        QVERIFY(tree->isExpanded(project()));
        QCOMPARE(tree->currentIndex().data(ThreadRole).toString(), "thread-1");
        tabs->setCurrentIndex(2);
        QCOMPARE(tree->model()->rowCount(), 0);
        tabs->setCurrentIndex(3);
        QCOMPARE(tree->model()->rowCount(), 1);
        fixture.notify(
            "thread/status/changed",
            {{"threadId", "thread-1"},
             {"status", QJsonObject{{"type", "active"}, {"activeFlags", QJsonArray{}}}}});
        QTRY_COMPARE(tree->model()->rowCount(), 0);
        tabs->setCurrentIndex(2);
        QCOMPARE(tree->model()->rowCount(), 1);
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
        QTRY_COMPARE(tree->model()->rowCount(), 0);
        tabs->setCurrentIndex(0);
        QCOMPARE(tree->model()->rowCount(), 1);
        window.close();
    }
    void activityViewsExpandWithoutOverridingManualCollapse() {
        QTemporaryDir data;
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
        put("old-home", "/home", "notLoaded");
        put("old-user", "/home/emsi", "notLoaded");
        put("old-documents", "/home/emsi/Documents", "notLoaded");
        auto waiting = put("waiting", "/home/emsi/Documents/Chats", "idle");
        put("running", "/srv/work/build", "active");
        auto root = [&] { return tree->model()->index(0, 0); };
        auto folder = [&] { return tree->model()->index(0, 0, root()); };
        for (const auto tab : {2, 3}) {
            tabs->setCurrentIndex(tab);
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
        auto fresh = put("fresh", "/home/emsi/Documents/other/nested", "idle");
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
        // Search may show history but must restore the filtered tree and its defaults.
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
            QTRY_COMPARE(tree->model()->rowCount(), 1);
            QCOMPARE(tree->model()->index(0, 0).data(CountRole).toInt(), 0);
            setting->setChecked(false);
            QTRY_COMPARE(tree->model()->index(0, 0).data(CountRole).toInt(), 1);
            auto session = Session::fromThread(
                tree->model()->index(0, 0).data(BackendRole).toString(), fixture.thread());
            session.terminalOpen = true;
            session.clientIssue = "Terminal client exited with code 1";
            window.findChild<TerminalArea *>()->clientStateChanged(session);
            setting->setChecked(true);
            tabs->setCurrentIndex(1);
            QCOMPARE(tree->model()->rowCount(), 1);
            tabs->setCurrentIndex(2);
            QCOMPARE(tree->model()->rowCount(), 0);
            tabs->setCurrentIndex(3);
            QCOMPARE(tree->model()->rowCount(), 0);
            session.terminalOpen = false;
            session.clientIssue.clear();
            window.findChild<TerminalArea *>()->clientStateChanged(session);
            tabs->setCurrentIndex(1);
            QCOMPARE(tree->model()->rowCount(), 0);
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
        QTRY_COMPARE(tree->model()->rowCount(), 2);
        Store store(data.path() + "/metadata.sqlite");
        QString firstId;
        for (const auto &p : store.backends())
            if (p.socketPath == first.path())
                firstId = p.id;
        QVERIFY(!firstId.isEmpty());
        QModelIndex server;
        for (int i = 0; i < tree->model()->rowCount(); ++i)
            if (tree->model()->index(i, 0).data(BackendRole) == firstId)
                server = tree->model()->index(i, 0);
        QVERIFY(server.isValid());
        auto session = tree->model()->index(0, 0, tree->model()->index(0, 0, server));
        QCOMPARE(session.data(ThreadRole).toString(), "thread-1");
        // A rejected preflight must not launch a terminal or fall back to the other server.
        first.readErrors["thread-1"] = "thread not loaded: thread-1";
        QSignalSpy terminalStates(window.findChild<TerminalArea *>(),
                                  &TerminalArea::clientStateChanged);
        bool sawError = false;
        for (auto *backend : window.findChildren<Backend *>())
            connect(backend, &Backend::sessionChanged, &window, [&](const Session &s) {
                if (!s.unavailable)
                    return;
                QTimer::singleShot(0, &window, [&] {
                    for (auto *box : window.findChildren<QMessageBox *>()) {
                        sawError = box->text().contains("thread not loaded: thread-1");
                        box->accept();
                    }
                });
            });
        tree->activated(session);
        QTRY_VERIFY(sawError);
        QCOMPARE(first.readIds.size(), 2);
        QCOMPARE(second.readIds.size(), 1);
        QCOMPARE(terminalStates.size(), 0);
        // Restart uses the same selected-server preflight instead of bypassing
        // verification with the terminal's previously captured arguments.
        sawError = false;
        const auto client = Session::fromThread(firstId, first.thread());
        window.findChild<TerminalArea *>()->restartRequested(client);
        QTRY_VERIFY(sawError);
        QCOMPARE(first.readIds.size(), 3);
        QCOMPARE(second.readIds.size(), 1);
        QCOMPARE(terminalStates.size(), 0);
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
        QTRY_COMPARE_WITH_TIMEOUT(tree->model()->rowCount(), 2, 5000);
        auto project = [&] { return tree->model()->index(0, 0); };
        QTRY_COMPARE_WITH_TIMEOUT(tree->model()->index(0, 0).data(CountRole).toInt() +
                                      tree->model()->index(1, 0).data(CountRole).toInt(),
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
        QCOMPARE(tree->model()->rowCount(), 2);
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
        QTRY_COMPARE(tree->model()->rowCount(), 1);
        QTRY_COMPARE(tree->model()->index(0, 0).data(CountRole).toInt(), 2);
        window.close();
    }
};
QTEST_MAIN(WindowTests)
#include "test_window.moc"
