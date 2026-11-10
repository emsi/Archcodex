// SPDX-License-Identifier: GPL-3.0-or-later
#include "commands.h"
#include "terminal.h"
#include <KXMLGUIClient>
#include <QAccessible>
#include <QDialog>
#include <QFile>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QScrollBar>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QtTest>
using namespace Archcodex;

namespace {
void addCycles(Commands &commands, TerminalArea &area) {
    for (bool projects : {false, true})
        for (bool backwards : {false, true}) {
            auto *action = commands.add(
                QString("cycle_%1_%2")
                    .arg(projects ? "projects" : "terminals", backwards ? "previous" : "next"),
                "Cycle", "Navigation",
                {QKeySequence(Qt::ControlModifier |
                              (backwards ? Qt::ShiftModifier : Qt::NoModifier) |
                              (projects ? Qt::Key_QuoteLeft : Qt::Key_Tab))},
                [&area, projects, backwards] { area.switchRecent(backwards, projects); });
            action->setProperty("cycleKind", projects ? 2 : 1);
            action->setProperty("cycleBackwards", backwards);
        }
    area.setCommands(&commands);
}
void cycleKey(int key, Qt::KeyboardModifiers modifiers) {
    auto *input = QApplication::focusWidget();
    QKeyEvent shortcut(QEvent::ShortcutOverride, key, modifiers);
    QApplication::sendEvent(input, &shortcut);
    QKeyEvent press(QEvent::KeyPress, key, modifiers);
    QApplication::sendEvent(input, &press);
    QKeyEvent release(QEvent::KeyRelease, key, modifiers);
    QApplication::sendEvent(QApplication::focusWidget(), &release);
}
void releaseControl() {
    QKeyEvent release(QEvent::KeyRelease, Qt::Key_Control, Qt::NoModifier);
    QApplication::sendEvent(QApplication::focusWidget(), &release);
}
QString candidate(TerminalArea &area) {
    auto *list = area.window()->findChild<QListWidget *>("recentSwitcherList");
    return list && list->currentItem() ? list->currentItem()->data(Qt::UserRole).toString()
                                       : QString();
}
} // namespace

class TerminalTests : public QObject {
    Q_OBJECT
  private slots:
    void restoredPlaceholdersPreserveNavigationAndExitIntent() {
        QTemporaryDir dir;
        QFile script(dir.filePath("client"));
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write("#!/usr/bin/python3\nimport time\ntime.sleep(30)\n");
        script.close();
        QVERIFY(script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        BackendProfile p{"b", "Fixture", "local-unix", "/unused", script.fileName(), false};
        p.identity["codexHome"] = "/home/test/.codex";
        WorkspaceSnapshot saved;
        for (const auto &id : {QString("a"), QString("b")}) {
            WorkspaceView v;
            v.id = "view-" + id;
            v.backendId = p.id;
            v.threadId = id;
            v.cwd = dir.path();
            v.title = id;
            v.sourceIdentity = workspaceSourceIdentity(p);
            saved.views << v;
            saved.recent << v.id;
        }
        saved.selected = "view-b";
        QWidget window;
        auto *layout = new QVBoxLayout(&window);
        auto *outside = new QLineEdit;
        auto *area = new TerminalArea;
        layout->addWidget(outside);
        layout->addWidget(area);
        window.resize(800, 500);
        window.show();
        window.activateWindow();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QSignalSpy focused(area, &TerminalArea::sessionFocused);
        area->restoreWorkspace(saved, {{p.id, p}});
        QCOMPARE(area->findChildren<KParts::ReadOnlyPart *>().size(), 0);
        QCOMPARE(area->workspace().json(), saved.json());
        outside->setFocus();
        for (const auto &v : saved.views) {
            auto session =
                Session::fromThread(p.id, {{"id", v.threadId}, {"name", v.title}, {"cwd", v.cwd}});
            QString error;
            QVERIFY2(area->startPrepared(v.id, p, session, &error), qPrintable(error));
            QCOMPARE(area->currentKey(), sessionKey(p.id, "b"));
            QCOMPARE(area->workspace().recent, saved.recent);
            QCOMPARE(QApplication::focusWidget(), outside);
        }
        QCOMPARE(focused.size(), 0);
        QVERIFY(area->findChildren<QLabel *>("terminalPlaceholder").isEmpty());
        area->freezeWorkspace();
        const auto intent = area->workspace().json();
        for (auto *part : area->findChildren<KParts::ReadOnlyPart *>())
            QVERIFY(QMetaObject::invokeMethod(part, "terminalProcessExited", Q_ARG(int, 1),
                                              Q_ARG(bool, false)));
        QCOMPARE(area->workspace().json(), intent);
    }
    void unresolvedAndExitedViewsNeverCreateClientsOnRestore() {
        TerminalArea original;
        BackendProfile p{"b", "Fixture"};
        auto s =
            Session::fromThread(p.id, {{"id", "saved"}, {"cwd", "/work"}, {"name", "Original"}});
        const auto v = original.prepare(p, s, true);
        original.openingState(v.id, "Missing session", true);
        s.id = "new-fresh";
        original.prepare(p, s, true);
        auto snapshot = original.workspace();
        QVERIFY(snapshot.validate().isEmpty());
        TerminalArea restored;
        restored.restoreWorkspace(snapshot, {{p.id, p}});
        QCOMPARE(restored.workspace().json(), snapshot.json());
        QCOMPARE(restored.findChildren<KParts::ReadOnlyPart *>().size(), 0);
        QCOMPARE(restored.findChildren<QLabel *>("terminalPlaceholder").size(), 2);
        auto renamed = s;
        renamed.id = "saved";
        renamed.title = "Renamed through app-server";
        restored.updateSession(renamed, p.name);
        QCOMPARE(restored.view(sessionKey(p.id, "saved")).title, renamed.title);
        QSignalSpy closed(&restored, &TerminalArea::viewClosed);
        restored.closeSession(sessionKey(p.id, "saved"));
        QCOMPARE(closed.size(), 1);
        QCOMPARE(closed.first()[0].toString(), v.id);
        QCOMPARE(restored.workspace().views.size(), 1);
    }
    void scrollbackShortcutsStayLocal_data() {
        QTest::addColumn<bool>("enhancedKeyboard");
        QTest::newRow("legacy") << false;
        QTest::newRow("enhanced-keyboard-request") << true;
    }
    void scrollbackShortcutsStayLocal() {
        QFETCH(bool, enhancedKeyboard);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QFile script(dir.filePath("client"));
        QVERIFY(script.open(QIODevice::WriteOnly));
        // The cursor-position reply confirms that both the output and the
        // keyboard-protocol request have been processed before sending keys.
        script.write("#!/usr/bin/python3\nimport os, pathlib, select, sys, time, tty\n"
                     "out = pathlib.Path(__file__).parent\n"
                     "tty.setraw(0)\n"
                     "os.write(1, b''.join(f'LINE {i:04d}\\r\\n'.encode() for i in range(500)))\n"
                     // Request the same enhanced keyboard flags as Codex, on the normal
                     // screen with real scrollback. Older terminals may ignore this.
                     "if sys.argv[-1] == 'kitty': os.write(1, b'\\x1b[>7u')\n"
                     "os.write(1, b'\\x1b[6n')\nresponse = b''\n"
                     "while not response.endswith(b'R'):\n"
                     " assert select.select([0], [], [], 5)[0], 'No cursor-position reply'\n"
                     " response += os.read(0, 1024)\n"
                     "(out / 'ready').touch()\n"
                     "received = b''\nend = time.monotonic() + 20\n"
                     "while time.monotonic() < end:\n"
                     " if not select.select([0], [], [], 0.1)[0]: continue\n"
                     " chunk = os.read(0, 1024)\n"
                     " if not chunk: break\n"
                     " received += chunk\n"
                     " if b'\\r' in received: break\n"
                     "(out / 'received').write_bytes(received.split(b'\\r')[0])\n");
        script.close();
        QVERIFY(script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        TerminalArea area;
        area.resize(900, 500);
        area.show();
        area.activateWindow();
        QVERIFY(QTest::qWaitForWindowExposed(&area));
        BackendProfile profile{"b", "Fixture", "local-unix", "/unused", script.fileName(), false};
        auto session = Session::fromThread(
            "b", {{"id", enhancedKeyboard ? "kitty" : "legacy"}, {"cwd", dir.path()}});
        QString error;
        QVERIFY2(area.open(profile, session, &error), qPrintable(error));
        QTRY_VERIFY(QApplication::focusWidget() && area.isAncestorOf(QApplication::focusWidget()));
        auto *input = QApplication::focusWidget();
        QTRY_VERIFY(QFile::exists(dir.filePath("ready")));
        auto *part = area.findChild<KParts::ReadOnlyPart *>();
        QVERIFY(part);
        auto *scrollbar = part->widget()->findChild<QScrollBar *>();
        QVERIFY(scrollbar);
        QVERIFY(scrollbar->maximum() > 100);
        const int bottom = scrollbar->maximum();
        QCOMPARE(scrollbar->value(), bottom);
        QTest::keyClick(input, Qt::Key_PageUp, Qt::ShiftModifier);
        QTRY_VERIFY(scrollbar->value() < bottom);
        QTest::keyClick(input, Qt::Key_PageDown, Qt::ShiftModifier);
        QTRY_COMPARE(scrollbar->value(), bottom);
        QTest::keyClick(input, Qt::Key_Home, Qt::ShiftModifier);
        QTRY_COMPARE(scrollbar->value(), scrollbar->minimum());
        QTest::keyClick(input, Qt::Key_End, Qt::ShiftModifier);
        QTRY_COMPARE(scrollbar->value(), bottom);
        QSignalSpy exits(&area, &TerminalArea::clientExited);
        QTest::keyClick(input, Qt::Key_Return);
        QTRY_COMPARE(exits.size(), 1);
        QCOMPARE(exits.first()[1].toInt(), 0);
        QFile received(dir.filePath("received"));
        QVERIFY(received.open(QIODevice::ReadOnly));
        // Only the final Enter should reach the child, without any scroll keys.
        QCOMPARE(received.readAll(), QByteArray());
        area.closeCurrent();
    }
    void windowCommandsOverrideKonsoleWithoutLeakingIntoThePty() {
        QTemporaryDir dir;
        QFile script(dir.filePath("client"));
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write(
            "#!/bin/sh\nprintf 'READY\n'\nIFS= read -r answer\nprintf '%s' \"$answer\" > '" +
            dir.filePath("received").toUtf8() + "'\n");
        script.close();
        QVERIFY(script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        QWidget host;
        auto *layout = new QHBoxLayout(&host);
        auto *navigator = new QLineEdit;
        auto *area = new TerminalArea;
        layout->addWidget(navigator);
        layout->addWidget(area);
        Commands commands(&host);
        int palette = 0, waiting = 0, searches = 0;
        commands.add("focus_projects", "Projects", "Navigation", {QKeySequence("Ctrl+Alt+P")},
                     [&] { navigator->setFocus(); });
        commands.add("focus_search", "Filter", "Navigation", {QKeySequence("Ctrl+Alt+F")},
                     [&] { navigator->setFocus(); });
        commands.add("focus_terminal", "Terminal", "Navigation", {QKeySequence("Alt+0")},
                     [&] { area->focusCurrent(); });
        commands.add("command_palette", "Commands", "Navigation", {QKeySequence("Ctrl+Shift+P")},
                     [&] { ++palette; });
        commands.add("terminal_find", "Find", "Terminal", {QKeySequence("Ctrl+Shift+F")}, [&] {
            if (area->triggerAction("edit_find"))
                ++searches;
        });
        commands.add("view_waiting", "Waiting", "Projects", {QKeySequence("Alt+4")}, [&] {
            ++waiting;
            navigator->setFocus();
        });
        area->setCommands(&commands);
        host.resize(900, 500);
        host.show();
        host.activateWindow();
        QVERIFY(QTest::qWaitForWindowExposed(&host));
        BackendProfile profile{"b", "Fixture", "local-unix", "/unused", script.fileName(), false};
        QString error;
        auto session = Session::fromThread("b", {{"id", "test"}, {"cwd", dir.path()}});
        QVERIFY2(area->open(profile, session, &error), qPrintable(error));
        QTRY_VERIFY(area->isAncestorOf(QApplication::focusWidget()));
        auto *input = QApplication::focusWidget();
        auto *accessible = QAccessible::queryAccessibleInterface(input);
        QVERIFY(accessible && accessible->textInterface());
        QTRY_VERIFY(accessible->textInterface()
                        ->text(0, accessible->textInterface()->characterCount())
                        .contains("READY"));
        QTest::keyClicks(input, "before");
        QTest::keyClick(input, Qt::Key_P, Qt::ControlModifier | Qt::AltModifier);
        QCOMPARE(QApplication::focusWidget(), navigator);
        QTest::keyClick(navigator, Qt::Key_0, Qt::AltModifier);
        QTRY_VERIFY(area->isAncestorOf(QApplication::focusWidget()));
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_P,
                        Qt::ControlModifier | Qt::ShiftModifier);
        QCOMPARE(palette, 1); // Konsole's Print Screen cannot claim the same key.
        QVERIFY(!QApplication::activeModalWidget());
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_F,
                        Qt::ControlModifier | Qt::AltModifier);
        QCOMPARE(QApplication::focusWidget(), navigator);
        QTest::keyClick(navigator, Qt::Key_0, Qt::AltModifier);
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_F,
                        Qt::ControlModifier | Qt::ShiftModifier);
        QCOMPARE(searches, 1);
        QVERIFY(area->isAncestorOf(QApplication::focusWidget()));
        QVERIFY(QApplication::focusWidget() != input); // Native scrollback search field.
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_Escape);
        area->focusCurrent();
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_4, Qt::AltModifier);
        QCOMPARE(waiting, 1); // Single-modifier shortcuts use the KPart override hook.
        QCOMPARE(QApplication::focusWidget(), navigator);
        QTest::keyClick(navigator, Qt::Key_0, Qt::AltModifier);
        auto *part = area->findChild<KParts::ReadOnlyPart *>();
        const auto findAction = [](auto &&self, KXMLGUIClient *client,
                                   const QString &name) -> QAction * {
            if (auto *action = client->actionCollection()->action(name))
                return action;
            for (auto *child : client->childClients())
                if (auto *action = self(self, child, name))
                    return action;
            return nullptr;
        };
        auto *print = findAction(findAction, part, "file_print");
        QVERIFY(print);
        QVERIFY(!print->shortcuts().contains(QKeySequence("Ctrl+Shift+P")));
        auto keys = Commands::bindings(commands.collection());
        keys["command_palette"] = {QKeySequence("Ctrl+Alt+J")};
        QVERIFY2(commands.apply(keys, &error), qPrintable(error));
        QVERIFY(print->shortcuts().contains(QKeySequence("Ctrl+Shift+P")));
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_J,
                        Qt::ControlModifier | Qt::AltModifier);
        QCOMPARE(palette, 2);
        QTest::keyClicks(QApplication::focusWidget(), " after");
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_Return);
        QTRY_VERIFY(QFile::exists(dir.filePath("received")));
        QFile received(dir.filePath("received"));
        QVERIFY(received.open(QIODevice::ReadOnly));
        QCOMPARE(received.readAll(), QByteArray("before after"));
        area->closeCurrent();
    }
    void containerTerminalUsesSelectedLocalExecutableAndExplicitRelay() {
        QTemporaryDir dir;
        QFile script(dir.path() + "/local codex ; literal");
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write(
            "#!/usr/bin/python3\nimport json,sys\n"
            "if '--version' in sys.argv: print('codex-cli 0.166.0')\n"
            "else:\n with open(__file__+'.args','w') as out: json.dump(sys.argv[1:],out)\n");
        script.close();
        QVERIFY(script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        TerminalArea area;
        area.setLocalCodexExecutable(script.fileName());
        area.show();
        BackendProfile p{"container", "Selected remote container", "mycodex",
                         "/remote/app-server.sock"};
        p.executable = "/remote-only/codex";
        p.relaySocket = "/local/explicit-relay.sock";
        p.sshHost = "must-not-be-contacted";
        p.containerId = QString(64, 'a');
        p.containerEpoch = "epoch";
        p.environmentId = "execution-environment";
        p.dockerContext = "default";
        p.runtimeUser = "alice";
        p.runtimeHome = "/home/alice";
        p.discoveryHost = "remote-host";
        auto s = Session::fromThread(p.id, {{"id", "thread-1"}, {"cwd", "/remote-only/workdir"}});
        s.executionObserved = true;
        s.executionEnvironment = p.environmentId;
        QSignalSpy exits(&area, &TerminalArea::clientExited);
        QString error;
        QVERIFY2(area.open(p, s, &error), qPrintable(error));
        QTRY_COMPARE(exits.size(), 1);
        QCOMPARE(exits.first()[1].toInt(), 0);
        QFile args(script.fileName() + ".args");
        QVERIFY(args.open(QIODevice::ReadOnly));
        QCOMPARE(QJsonDocument::fromJson(args.readAll()).array(),
                 QJsonArray({"--remote", "unix:///local/explicit-relay.sock", "resume", "--",
                             "thread-1"}));
        // The test executable was invoked only as a version probe and explicit
        // remote client; neither SSH nor an app-server launch is in this path.
        area.closeCurrent();
    }
    void controlTabCyclesFrozenRecentOrderAndNeverReachesTheTerminal() {
        QTemporaryDir dir;
        QFile script(dir.path() + "/client");
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write("#!/bin/sh\nfor id; do :; done\nprintf 'READY\\n'\nIFS= read -r "
                     "answer\nprintf '%s' \"$answer\" > '" +
                     dir.path().toUtf8() + "/'\"$id\"\n");
        script.close();
        QVERIFY(script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        TerminalArea area;
        Commands commands(&area);
        addCycles(commands, area);
        area.resize(800, 500);
        area.show();
        area.activateWindow();
        QVERIFY(QTest::qWaitForWindowExposed(&area));
        QSignalSpy exits(&area, &TerminalArea::clientExited);
        QSignalSpy titles(&area, &TerminalArea::currentChanged);
        BackendProfile p{
            "b", "Full server · host · storage", "local-unix", "/unused", script.fileName(), false};
        QString error;
        for (const auto &id : {"a", "b", "c"}) {
            auto s = Session::fromThread("b", {{"id", id}, {"name", id}, {"cwd", dir.path()}});
            QVERIFY2(area.open(p, s, &error), qPrintable(error));
        }
        QVERIFY(titles.last()[0].toString().contains(p.name));
        QTRY_VERIFY(QApplication::focusWidget() && area.isAncestorOf(QApplication::focusWidget()));
        QSignalSpy focuses(&area, &TerminalArea::sessionFocused);
        const auto tab = [&](bool backwards = false) {
            cycleKey(backwards ? Qt::Key_Backtab : Qt::Key_Tab,
                     Qt::ControlModifier | (backwards ? Qt::ShiftModifier : Qt::NoModifier));
        };
        QCOMPARE(area.currentKey(), sessionKey("b", "c"));
        tab();
        QCOMPARE(candidate(area), sessionKey("b", "b"));
        tab();
        QCOMPARE(candidate(area), sessionKey("b", "a"));
        tab(true);
        QCOMPARE(candidate(area), sessionKey("b", "b"));
        QCOMPARE(area.currentKey(), sessionKey("b", "c"));
        QCOMPARE(focuses.size(), 0);
        releaseControl();
        QCOMPARE(area.currentKey(), sessionKey("b", "b"));
        QCOMPARE(focuses.size(), 1);
        tab();
        QCOMPARE(candidate(area), sessionKey("b", "c"));
        releaseControl();
        tab(true);
        QCOMPARE(candidate(area), sessionKey("b", "a"));
        tab(true);
        QCOMPARE(candidate(area), sessionKey("b", "b"));
        releaseControl();
        QCOMPARE(focuses.size(), 3);
        QTRY_VERIFY(QApplication::focusWidget() && area.isAncestorOf(QApplication::focusWidget()));
        auto *input = QApplication::focusWidget();
        auto *accessible = QAccessible::queryAccessibleInterface(input);
        QVERIFY(accessible && accessible->textInterface());
        auto *text = accessible->textInterface();
        QTRY_VERIFY(text->text(0, text->characterCount()).contains("READY"));
        QTest::keyClicks(input, "ordinary input");
        QTest::keyClick(input, Qt::Key_Return);
        QTRY_COMPARE(exits.size(), 1);
        QFile received(dir.path() + "/b");
        QVERIFY(received.open(QIODevice::ReadOnly));
        QCOMPARE(received.readAll(), QByteArray("ordinary input"));
        // Retained exit output participates, while closed views do not.
        area.switchRecent();
        QCOMPARE(area.currentKey(), sessionKey("b", "c"));
        area.switchRecent();
        QCOMPARE(area.currentKey(), sessionKey("b", "b"));
        area.closeCurrent();
        area.switchRecent();
        QCOMPARE(area.currentKey(), sessionKey("b", "c"));
        area.closeSession(sessionKey("b", "a")); // Closing a different row preserves the view.
        QCOMPARE(area.currentKey(), sessionKey("b", "c"));
        QVERIFY(!area.focusSession(sessionKey("b", "a")));
        area.closeBackend("b");
        area.switchRecent();
        QVERIFY(area.currentKey().isEmpty());
    }
    void projectCyclesCancelRebindAndHandleClosedCandidates() {
        QTemporaryDir dir;
        QFile script(dir.filePath("client"));
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write("#!/bin/sh\nprintf 'READY\n'\nIFS= read -r line\n");
        script.close();
        QVERIFY(script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        TerminalArea area;
        Commands commands(&area);
        addCycles(commands, area);
        area.resize(900, 600);
        area.show();
        area.activateWindow();
        QVERIFY(QTest::qWaitForWindowExposed(&area));
        BackendProfile profile{"b",       "Host / container", "local-unix",
                               "/unused", script.fileName(),  false};
        QString error;
        for (const auto &id : {"a1", "b1", "a2", "c1"}) {
            QVERIFY(QDir().mkpath(dir.path() + "/" + QString(id).left(1)));
            auto session = Session::fromThread(
                "b", {{"id", id}, {"name", id}, {"cwd", dir.path() + "/" + QString(id).left(1)}});
            QVERIFY2(area.open(profile, session, &error), qPrintable(error));
        }
        QSignalSpy focused(&area, &TerminalArea::sessionFocused);
        cycleKey(Qt::Key_QuoteLeft, Qt::ControlModifier);
        QCOMPARE(candidate(area), sessionKey("b", "a2"));
        QCOMPARE(area.findChild<QListWidget *>("recentSwitcherList")->count(), 3);
        const auto screenshots = qEnvironmentVariable("ARCHCODEX_TEST_SCREENSHOT_DIR");
        if (!screenshots.isEmpty())
            QVERIFY(area.grab().save(screenshots + "/project-switcher.png"));
        QCOMPARE(area.currentKey(), sessionKey("b", "c1"));
        cycleKey(Qt::Key_Escape, Qt::ControlModifier);
        releaseControl();
        QVERIFY(!area.findChild<QFrame *>("recentSwitcher")->isVisible());
        QCOMPARE(focused.size(), 0);
        cycleKey(Qt::Key_QuoteLeft, Qt::ControlModifier);
        releaseControl();
        QCOMPARE(area.currentKey(), sessionKey("b", "a2"));
        QCOMPARE(focused.size(), 1);
        cycleKey(Qt::Key_QuoteLeft, Qt::ControlModifier);
        QCOMPARE(candidate(area), sessionKey("b", "c1"));
        QEvent deactivated(QEvent::WindowDeactivate);
        QApplication::sendEvent(&area, &deactivated);
        QVERIFY(!area.findChild<QFrame *>("recentSwitcher")->isVisible());
        QCOMPARE(area.currentKey(), sessionKey("b", "a2"));
        QCOMPARE(focused.size(), 1);
        // A changed binding is shared by routing, validation and the switcher.
        auto keys = Commands::bindings(commands.collection());
        keys["cycle_terminals_next"] = {QKeySequence("Ctrl+F8")};
        keys["cycle_terminals_previous"] = {QKeySequence("Ctrl+Shift+F8")};
        QVERIFY2(commands.apply(keys, &error), qPrintable(error));
        cycleKey(Qt::Key_F8, Qt::ControlModifier);
        QCOMPARE(candidate(area), sessionKey("b", "c1"));
        area.closeSession(sessionKey("b", "c1"));
        QVERIFY(candidate(area) != sessionKey("b", "c1"));
        releaseControl();
        QVERIFY(area.currentKey() != sessionKey("b", "c1"));
        QCOMPARE(focused.size(), 2);
        // Settings and other dialogs must receive their own navigation keys.
        QDialog dialog(&area);
        auto *layout = new QHBoxLayout(&dialog);
        auto *edit = new QLineEdit;
        layout->addWidget(edit);
        dialog.setModal(true);
        dialog.show();
        edit->setFocus();
        QTest::keyClick(edit, Qt::Key_F8, Qt::ControlModifier);
        QVERIFY(!area.findChild<QFrame *>("recentSwitcher")->isVisible());
        dialog.close();
        area.closeBackend("b");
    }
    void projectSwitcherKeepsGroupsWhenRepresentativeCloses() {
        QTemporaryDir dir;
        QFile script(dir.filePath("client"));
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write("#!/bin/sh\nIFS= read -r line\n");
        script.close();
        QVERIFY(script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        TerminalArea area;
        Commands commands(&area);
        addCycles(commands, area);
        area.show();
        area.activateWindow();
        QVERIFY(QTest::qWaitForWindowExposed(&area));
        BackendProfile profile{"b", "Fixture", "local-unix", "/unused", script.fileName(), false};
        QString error;
        for (const auto &id : {"a1", "b1", "a2", "c1"}) {
            const auto path = dir.path() + "/" + QString(id).left(1);
            QVERIFY(QDir().mkpath(path));
            auto session = Session::fromThread("b", {{"id", id}, {"cwd", path}});
            QVERIFY2(area.open(profile, session, &error), qPrintable(error));
        }
        cycleKey(Qt::Key_QuoteLeft, Qt::ControlModifier);
        QCOMPARE(candidate(area), sessionKey("b", "a2"));
        area.closeSession(sessionKey("b", "a2"));
        QCOMPARE(candidate(area), sessionKey("b", "a1"));
        QCOMPARE(area.findChild<QListWidget *>("recentSwitcherList")->count(), 3);
        cycleKey(Qt::Key_QuoteLeft, Qt::ControlModifier);
        QCOMPARE(candidate(area), sessionKey("b", "b1"));
        releaseControl();
        QCOMPARE(area.currentKey(), sessionKey("b", "b1"));
        profile.id = "other-server";
        auto other = Session::fromThread(profile.id, {{"id", "a1"}, {"cwd", dir.path() + "/a"}});
        QVERIFY2(area.open(profile, other, &error), qPrintable(error));
        cycleKey(Qt::Key_QuoteLeft, Qt::ControlModifier);
        QCOMPARE(area.findChild<QListWidget *>("recentSwitcherList")->count(), 4);
        cycleKey(Qt::Key_Escape, Qt::ControlModifier);
        releaseControl();
        area.closeBackend("b");
        area.closeBackend(profile.id);
    }
    void containerHistoryAndChangedEnvironmentCannotLaunch() {
        TerminalArea area;
        BackendProfile p{"container", "Container"};
        p.kind = "mycodex";
        p.relaySocket = "/unused/fixture-relay";
        p.environmentId = "current-environment";
        Session s;
        s.backendId = p.id;
        s.id = "shared-thread";
        s.contextRequired = true;
        QString error;
        QVERIFY(!area.open(p, s, &error));
        QVERIFY(!error.isEmpty());
        s.executionObserved = true;
        s.executionEnvironment = "old-environment";
        QVERIFY(!area.open(p, s, &error));
        p.relaySocket.clear();
        s.executionEnvironment = p.environmentId;
        QVERIFY(!area.open(p, s, &error));
        QVERIFY(error.contains("no live desktop transport"));
    }
    void failedClientDoesNotSwitchBack_data() {
        QTest::addColumn<QByteArray>("command");
        QTest::addColumn<int>("exitCode");
        QTest::addColumn<bool>("crashed");
        QTest::newRow("error") << QByteArray("exit 7") << 7 << false;
        QTest::newRow("clean-exit") << QByteArray("exit 0") << 0 << false;
        QTest::newRow("signal") << QByteArray("kill -TERM $$") << 15 << true;
        QTest::newRow("missing-program")
            << QByteArray("exec /nonexistent/archcodex-test") << 127 << false;
    }
    void failedClientDoesNotSwitchBack() {
        QFETCH(QByteArray, command);
        QFETCH(int, exitCode);
        QFETCH(bool, crashed);
        QTest::failOnWarning(QRegularExpression(".*(Unexpected nullptr|No event config).*"));
        QTemporaryDir dir;
        QFile script(dir.path() + "/client");
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write("#!/bin/sh\nprintf 'CLIENT OUTPUT\\n'\ncase \"$*\" in *failure*) " + command +
                     ";; esac\nread answer\n");
        script.close();
        QVERIFY(script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        TerminalArea area;
        QSignalSpy exits(&area, &TerminalArea::clientExited);
        QSignalSpy states(&area, &TerminalArea::clientStateChanged);
        QSignalSpy restarts(&area, &TerminalArea::restartRequested);
        area.resize(800, 500);
        area.show();
        BackendProfile p{"b", "Local", "local-unix", "/unused", script.fileName(), false};
        auto session = Session::fromThread("b", {{"id", "first"}, {"cwd", dir.path()}});
        QString error;
        QVERIFY2(area.open(p, session, &error), qPrintable(error));
        auto *stack = area.findChild<QStackedWidget *>();
        auto *first = stack->currentWidget();
        session.id = "failure";
        QVERIFY2(area.open(p, session, &error), qPrintable(error));
        QPointer<QWidget> failed = stack->currentWidget();
        QVERIFY(first != failed);
        QTRY_COMPARE(exits.size(), 1);
        QCOMPARE(exits[0][1].toInt(), exitCode);
        QCOMPARE(exits[0][2].toBool(), crashed);
        QVERIFY(failed);
        auto client = qvariant_cast<Session>(states.last()[0]);
        QVERIFY(client.terminalOpen && client.isActive());
        QVERIFY(!client.clientIssue.isEmpty());
        QCOMPARE(area.workspace().views.first().disposition, ViewDisposition::Disconnected);
        QVERIFY(area.workspace().views.first().summary.contains("not saved across restarts"));
        QVERIFY(!client.isRunning() && !client.isWaiting());
        QVERIFY(area.focusSession(sessionKey("b", "first")));
        session.id = "first";
        QVERIFY(!area.restart(p, session, &error)); // Never replace a running client.
        session.id = "failure";
        QVERIFY(area.focusSession(sessionKey("b", "failure")));
        QCOMPARE(stack->currentWidget(), failed.data());
        bool retainedOutput = false;
        for (auto *widget : failed->findChildren<QWidget *>()) {
            auto *accessible = QAccessible::queryAccessibleInterface(widget);
            if (accessible && accessible->textInterface()) {
                auto *text = accessible->textInterface();
                retainedOutput |= text->text(0, text->characterCount()).contains("CLIENT OUTPUT");
            }
        }
        QVERIFY(retainedOutput);
        auto *retry = area.findChild<QPushButton *>("reconnectTerminal");
        QVERIFY(retry && retry->isVisible());
        QTest::mouseClick(retry, Qt::LeftButton);
        QCOMPARE(restarts.size(), 1);
        QCOMPARE(qvariant_cast<Session>(restarts[0][0]).id, session.id);
        QCOMPARE(stack->currentWidget(), failed.data()); // Keep output until validation succeeds.
        auto changed = p;
        changed.kind = "mycodex";
        changed.environmentId = "different-environment";
        QVERIFY(!area.restart(changed, session, &error));
        QCOMPARE(stack->currentWidget(), failed.data());
        // The controller supplies a refreshed profile, not the old command.
        QFile refreshed(dir.path() + "/refreshed-client");
        QVERIFY(refreshed.open(QIODevice::WriteOnly));
        refreshed.write("#!/bin/sh\nprintf 'FRESH PROFILE\\n'\nexit 9\n");
        refreshed.close();
        QVERIFY(refreshed.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        p.executable = refreshed.fileName();
        QVERIFY2(area.restart(p, session, &error), qPrintable(error));
        QTRY_COMPARE(exits.size(), 2);
        QCOMPARE(exits[1][1].toInt(), 9);
        QVERIFY(!failed);
        QVERIFY(stack->currentWidget() != first);
        area.closeCurrent();
        QVERIFY(!qvariant_cast<Session>(states.last()[0]).terminalOpen);
        QVERIFY(!area.restart(p, session, &error));
        session.id = "first";
        QVERIFY2(area.open(p, session, &error), qPrintable(error));
        QCOMPARE(stack->currentWidget(), first);
    }
    void legacyUnresolvedNewSessionCannotReplayCreation() {
        TerminalArea area;
        BackendProfile p{"b", "Local", "local-unix", "/unused", "/bin/echo", false};
        Session s;
        s.backendId = p.id;
        s.id = "new-legacy-view";
        s.cwd = "/work/project";
        s.title = "New session terminal";
        const auto view = area.prepare(p, s, true);
        QVERIFY(view.threadId.isEmpty());
        QCOMPARE(view.disposition, ViewDisposition::Unresolved);
        QString error;
        QVERIFY(!area.startPrepared(view.id, p, s, &error));
        QVERIFY(error.contains("no confirmed conversation ID"));
        QCOMPARE(area.findChildren<KParts::ReadOnlyPart *>().size(), 0);
        QVERIFY(!area.restart(p, s, &error));
        QVERIFY(error.contains("no confirmed conversation ID"));
        area.closeCurrent();
    }
};
QTEST_MAIN(TerminalTests)
#include "test_terminal.moc"
