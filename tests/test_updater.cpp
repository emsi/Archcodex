// SPDX-License-Identifier: GPL-3.0-or-later
#include "updatedialog.h"
#include "updater.h"
#include <QFile>
#include <QPushButton>
#include <QTemporaryDir>
#include <QtTest>
using namespace Archcodex;

class UpdaterTests : public QObject {
    Q_OBJECT
    QTemporaryDir scripts;
    QStringList helper(const QByteArray &body) {
        const auto path = scripts.filePath("helper.py");
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly))
            qFatal("Cannot write test helper");
        file.write(
            "import json,sys,time\nrequest=json.loads(sys.stdin.readline())\n"
            "def emit(event,**data): print(json.dumps(dict(event=event,**data)),flush=True)\n");
        file.write(body);
        return {"/usr/bin/python3", "-u", path};
    }
  private slots:
    void explicitInstallAndRestart() {
        const auto command = helper(
            "if request['operation']=='check':\n"
            " emit('available',release={'version':'0.10.0','notes':'Example'},install_reason='')\n"
            "elif request['operation']=='install':\n"
            " emit('downloading',percent=50)\n"
            " emit('ready_to_install')\n"
            " assert sys.stdin.readline()=='install\\n'\n"
            " emit('installed',version='0.10.0')\n"
            "else: emit('restart_ready')\n");
        Updater updater("0.9.0", scripts.path(), nullptr, command);
        auto *dialog = new UpdateDialog(&updater, "0.9.0");
        dialog->show();
        QVERIFY(!dialog->isModal());
        updater.install(); // no release and no consent: no process
        QVERIFY(!updater.busy());
        updater.check();
        QTRY_COMPARE(updater.state(), Updater::State::Available);
        QVERIFY(!updater.busy());
        auto *install = dialog->findChild<QPushButton *>("updateInstall");
        QVERIFY(install->isVisible() && install->isEnabled());
        const auto screenshots = qEnvironmentVariable("ARCHCODEX_TEST_SCREENSHOT_DIR");
        if (!screenshots.isEmpty())
            QVERIFY(dialog->grab().save(screenshots + "/updates-available.png"));
        install->click();
        QTRY_COMPARE(updater.state(), Updater::State::Installed);
        QCOMPARE(updater.installedVersion(), "0.10.0");
        if (!screenshots.isEmpty())
            QVERIFY(dialog->grab().save(screenshots + "/updates-installed.png"));
        QSignalSpy restarting(&updater, &Updater::restartReady);
        dialog->findChild<QPushButton *>("updateRestart")->click();
        QTRY_COMPARE(restarting.size(), 1);
        QCOMPARE(updater.state(), Updater::State::RestartReady);
        dialog->close();
    }
    void cancellationBeforeInstallAcknowledgement() {
        const auto command = helper(
            "if request['operation']=='check': emit('available',release={'version':'0.10.0'})\n"
            "else:\n"
            " emit('ready_to_install')\n"
            " time.sleep(10)\n");
        Updater updater("0.9.0", scripts.path(), nullptr, command);
        updater.check();
        QTRY_COMPARE(updater.state(), Updater::State::Available);
        updater.install();
        updater.cancel();
        QTRY_VERIFY(!updater.busy());
        QCOMPARE(updater.state(), Updater::State::Idle);
    }
    void installCannotBeCancelledOnceAcknowledged() {
        const auto command = helper(
            "if request['operation']=='check': emit('available',release={'version':'0.10.0'})\n"
            "else:\n"
            " emit('ready_to_install')\n"
            " assert sys.stdin.readline()=='install\\n'\n"
            " time.sleep(0.15)\n"
            " emit('installed',version='0.10.0')\n");
        Updater updater("0.9.0", scripts.path(), nullptr, command);
        updater.check();
        QTRY_COMPARE(updater.state(), Updater::State::Available);
        updater.install();
        QTRY_COMPARE(updater.state(), Updater::State::Installing);
        updater.cancel();
        QTRY_COMPARE(updater.state(), Updater::State::Installed);
    }
    void unsupportedInstallIsDisabled() {
        Updater updater("0.9.0", scripts.path(), nullptr,
                        helper("emit('available',release={'version':'0.10.0'},install_reason='"
                               "System installation')\n"));
        updater.check();
        QTRY_COMPARE(updater.state(), Updater::State::Available);
        updater.install();
        QVERIFY(!updater.busy());
        auto *dialog = new UpdateDialog(&updater, "0.9.0");
        QVERIFY(!dialog->findChild<QPushButton *>("updateInstall")->isEnabled());
        dialog->close();
    }
    void failures_data() {
        QTest::addColumn<QByteArray>("code");
        QTest::newRow("incomplete-result") << QByteArray("emit('current')\n");
        QTest::newRow("missing-result") << QByteArray("pass\n");
        QTest::newRow("truncated") << QByteArray("print('{',end='')\n");
        QTest::newRow("invalid-order") << QByteArray("emit('ready_to_install')\n");
        QTest::newRow("nonzero-success")
            << QByteArray("emit('current',release={'version':'0.9.0'})\nsys.exit(1)\n");
        QTest::newRow("duplicate-result") << QByteArray("emit('current')\nemit('current')\n");
        QTest::newRow("reported-error") << QByteArray("emit('error',message='offline')\n");
    }
    void failures() {
        QFETCH(QByteArray, code);
        Updater updater("0.9.0", scripts.path(), nullptr, helper(code));
        updater.check();
        QTRY_COMPARE(updater.state(), Updater::State::Error);
        QVERIFY(!updater.error().isEmpty());
        QVERIFY(!updater.busy());
    }
    void failedStartAndCancellationStayResponsive() {
        Updater missing("0.9.0", scripts.path(), nullptr, {"/missing/archcodex-test-updater"});
        missing.check();
        QTRY_COMPARE(missing.state(), Updater::State::Error);
        Updater updater("0.9.0", scripts.path(), nullptr, helper("time.sleep(10)\n"));
        updater.check();
        QSignalSpy changed(&updater, &Updater::changed);
        updater.check(); // no parallel helper
        QCOMPARE(changed.size(), 0);
        bool responsive = false;
        QTimer::singleShot(0, &updater, [&] {
            responsive = true;
            updater.cancel();
        });
        QTRY_VERIFY(responsive && !updater.busy());
        QCOMPARE(updater.state(), Updater::State::Idle);
    }
};
QTEST_MAIN(UpdaterTests)
#include "test_updater.moc"
