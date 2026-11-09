// SPDX-License-Identifier: GPL-3.0-or-later
#include "window.h"
#include "worker.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QMessageBox>
#include <QStandardPaths>
#include <QTimer>
#include <fcntl.h>
#include <stdexcept>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

class InstanceLock {
    int fd = -1;

  public:
    explicit InstanceLock(const QString &directory) {
        if (!QDir().mkpath(directory))
            throw std::runtime_error("Cannot create application data directory.");
        const auto path = QFile::encodeName(directory + "/instance.lock");
        fd = ::open(path.constData(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd < 0)
            throw std::runtime_error("Cannot open the application lock file.");
        if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
            ::close(fd);
            fd = -1;
            throw std::runtime_error("Archcodex is already using this data directory.");
        }
    }
    ~InstanceLock() {
        if (fd >= 0)
            ::close(fd);
    }
};

int main(int argc, char **argv) {
    ::umask(0077);
    QApplication app(argc, argv);
    app.setApplicationName("archcodex");
    app.setOrganizationDomain("emsi.github.io");
    app.setApplicationVersion(ARCHCODEX_VERSION);
    app.setDesktopFileName("io.github.emsi.Archcodex");
    QCommandLineParser parser;
    parser.setApplicationDescription("Native Codex project and session workspace");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption({"data-dir", "Use a separate application data directory.", "path"});
    parser.addOption(
        {"smoke-test",
         "Load the window and Konsole component without connecting to saved backends."});
    parser.addOption({"screenshot", "Save a smoke-test screenshot.", "path"});
    parser.process(app);
    const auto dir = parser.isSet("data-dir")
                         ? parser.value("data-dir")
                         : QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    try {
        InstanceLock lock(dir);
        struct FinishWorkers {
            ~FinishWorkers() {
                if (!Archcodex::Worker::finishPending())
                    qWarning()
                        << "Worker shutdown exceeded 10 seconds; metadata cache may be incomplete.";
            }
        } finishWorkers;
        Archcodex::Window window(dir, !parser.isSet("smoke-test"));
        window.show();
        if (parser.isSet("smoke-test")) {
            QString error;
            if (!window.smokeTest(&error)) {
                qCritical().noquote() << error;
                return 1;
            }
            QTimer::singleShot(700, &app, [&] {
                if (parser.isSet("screenshot"))
                    window.grab().save(parser.value("screenshot"));
                qInfo() << "Window and Konsole component smoke test passed.";
                app.quit();
            });
        }
        return app.exec();
    } catch (const std::exception &e) {
        qCritical() << e.what();
        if (!parser.isSet("smoke-test")) {
            QMessageBox box(QMessageBox::Critical, "Archcodex startup failed", {}, QMessageBox::Ok);
            box.setTextFormat(Qt::PlainText);
            box.setText(QString::fromUtf8(e.what()));
            box.exec();
        }
        return 1;
    }
}
