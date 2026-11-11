// SPDX-License-Identifier: GPL-3.0-or-later
#include "launch.h"
#include "testenvironment.h"
#include <KSandbox>
#include <QFile>
#include <QJsonDocument>
#include <QtTest>

using namespace Archcodex;

class ClientLaunchTests : public QObject {
    Q_OBJECT
    TestHome home;

    static void write(const QString &path, const QByteArray &contents) {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(file.write(contents), contents.size());
        QVERIFY(file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    }

  private slots:
    void editorsReachBothClientPaths_data() {
        QTest::addColumn<bool>("relay");
        QTest::addColumn<QByteArray>("editor");
        QTest::addColumn<QByteArray>("visual");
        QTest::addColumn<bool>("hostValues");
        QTest::addColumn<QByteArray>("startup");
        QTest::addColumn<QJsonObject>("expected");
        const auto values = [](QJsonValue e = QJsonValue::Null, QJsonValue v = QJsonValue::Null) {
            return QJsonObject{{"EDITOR", e}, {"VISUAL", v}};
        };
        for (bool relay : {false, true}) {
            const auto row = [&](const char *name, QByteArray e, QByteArray v, bool host,
                                 QByteArray rc, QJsonObject expected) {
                QTest::newRow((QByteArray(relay ? "relay-" : "direct-") + name).constData())
                    << relay << e << v << host << rc << expected;
            };
            row("neither", {}, {}, false, {}, values());
            row("editor", "vim -f", {}, false, {}, values("vim -f"));
            row("visual", {}, "kate --block", false, {}, values(QJsonValue::Null, "kate --block"));
            row("both", "vim -f", "kate --block", false, {}, values("vim -f", "kate --block"));
            row("empty-editor", "", {}, false, {}, values(""));
            row("empty-visual", "vim", "", true, {}, values("vim", ""));
            row("host-inheritance", {}, {}, true, {}, values("host-editor", "host-visual"));
            row("override-stale-host", "new-editor", "new-visual", true, {},
                values("new-editor", "new-visual"));
            row("interactive-rc", {}, {}, false,
                "export EDITOR='rc editor --wait' VISUAL='rc visual'\n",
                values("rc editor --wait", "rc visual"));
            row("rc-overrides-forwarding", "inherited-editor", "inherited-visual", false,
                "export EDITOR=override\nunset VISUAL\n", values("override"));
            const auto literal = QString::fromUtf8(
                "'quoted editor' \"arg\" $HOME ${USER} `false` $(false); \\ path\nżółw");
            row("literal", literal.toUtf8(), literal.toUtf8(), true, {}, values(literal, literal));
        }
    }
    void editorsReachBothClientPaths() {
        QFETCH(bool, relay);
        QFETCH(QByteArray, editor);
        QFETCH(QByteArray, visual);
        QFETCH(bool, hostValues);
        QFETCH(QByteArray, startup);
        QFETCH(QJsonObject, expected);
        QTemporaryDir dir;
        TestEnvironment inherited;
        inherited.set("EDITOR", editor);
        inherited.set("VISUAL", visual);
        write(dir.filePath(".bashrc"),
              "case $- in *i*) ;; *) return ;; esac\n"
              "export ENTRY_EDITOR=${EDITOR-unset} ENTRY_VISUAL=${VISUAL-unset}\n" +
                  startup);
        const auto executable = dir.filePath("codex 'literal' $HOME");
        write(executable, "#!/usr/bin/python3\nimport json,os,sys\n"
                          "if '--version' in sys.argv: print('codex-cli 0.166.0')\n"
                          "else: print(json.dumps(dict(argv=sys.argv[1:],pid=os.getpid(),"
                          "environment={k:os.environ.get(k) for k in "
                          "('EDITOR','VISUAL','ENTRY_EDITOR','ENTRY_VISUAL')})))\n");
        BackendProfile profile{"b",        "Fixture", "local-unix", dir.filePath("socket $HOME"),
                               executable, false};
        if (relay) {
            profile.kind = "mycodex";
            profile.relaySocket = profile.socketPath;
            profile.executable = "/container-only/codex";
            profile.sshHost = "must-not-be-contacted";
        }
        // Both new-client and resume commands keep all argv entries literal.
        for (const auto &thread : {QString(), QString("thread ' ; $(false) $HOME")}) {
            const auto command = clientArguments(profile, thread, "/remote dir", executable);
            auto host = QProcessEnvironment::systemEnvironment();
            host.insert("HOME", dir.path());
            host.remove("EDITOR");
            host.remove("VISUAL");
            if (hostValues) {
                host.insert("EDITOR", "host-editor");
                host.insert("VISUAL", "host-visual");
            }
            QProcess client;
            client.setProcessEnvironment(host);
            client.start(command.first(), command.mid(1));
            QVERIFY(client.waitForStarted());
            const auto pid = client.processId();
            QVERIFY(client.waitForFinished(10000));
            QCOMPARE(client.exitCode(), 0);
            const auto output = QJsonDocument::fromJson(client.readAllStandardOutput()).object();
            QCOMPARE(output["pid"].toInteger(), pid); // env, Bash and transport all exec.
            const auto environment = output["environment"].toObject();
            QCOMPARE(environment["EDITOR"], expected["EDITOR"]);
            QCOMPARE(environment["VISUAL"], expected["VISUAL"]);
            QCOMPARE(environment["ENTRY_EDITOR"].toString(),
                     editor.isNull() ? host.value("EDITOR", "unset") : QString::fromUtf8(editor));
            QCOMPARE(environment["ENTRY_VISUAL"].toString(),
                     visual.isNull() ? host.value("VISUAL", "unset") : QString::fromUtf8(visual));
            QJsonArray argv{"--remote", profile.endpoint()};
            const QStringList tail = thread.isEmpty() ? QStringList{"--cd", "/remote dir"}
                                                      : QStringList{"resume", "--", thread};
            for (const auto &arg : tail)
                argv.append(arg);
            QCOMPARE(output["argv"].toArray(), argv);
        }
    }
    void kdeIdenticalValueFilteringDoesNotDropEditorArguments() {
        if (!KSandbox::isFlatpak())
            QSKIP("Run in Flatpak or the isolated SDK host-context fixture");
        TestEnvironment inherited;
        inherited.set("EDITOR", "editor $HOME 'quoted' --wait");
        inherited.set("VISUAL", "");
        const auto command =
            terminalCommand({"/usr/bin/python3", "-c",
                             "import json,os;print(json.dumps([os.getenv('EDITOR'),"
                             "os.getenv('VISUAL')]))"});
        QProcess requested;
        requested.setProcessEnvironment(QProcessEnvironment::systemEnvironment());
        requested.setProgram(command.first());
        requested.setArguments(command.mid(1));
        const auto context = KSandbox::makeHostContext(requested);
        QCOMPARE(context.program, "/usr/bin/flatpak-spawn");
        QVERIFY(!context.arguments.contains("--env=EDITOR=" + qEnvironmentVariable("EDITOR")));
        QVERIFY(!context.arguments.contains("--env=VISUAL="));
        // Use the real KDE-generated command with an absent/stale helper environment.
        // This isolates its filtering contract; it is not a desktop Flatpak smoke test.
        const auto boundary = context.arguments.indexOf(command.first());
        QVERIFY(boundary >= 0);
        QCOMPARE(context.arguments.mid(boundary), command);
        for (bool stale : {false, true}) {
            auto host = QProcessEnvironment::systemEnvironment();
            host.remove("EDITOR");
            host.remove("VISUAL");
            if (stale) {
                host.insert("EDITOR", "stale");
                host.insert("VISUAL", "stale");
            }
            QProcess client;
            client.setProcessEnvironment(host);
            client.start(command.first(), context.arguments.mid(boundary + 1));
            QVERIFY(client.waitForFinished(10000));
            QCOMPARE(client.exitCode(), 0);
            QCOMPARE(QJsonDocument::fromJson(client.readAllStandardOutput()).array(),
                     (QJsonArray{qEnvironmentVariable("EDITOR"), ""}));
        }
    }
};
QTEST_GUILESS_MAIN(ClientLaunchTests)
#include "test_clientlaunch.moc"
