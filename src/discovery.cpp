// SPDX-License-Identifier: GPL-3.0-or-later
#include "discovery.h"
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

// Force the resource into executables which link the static core library.
static void initDiscoveryResource() {
    Q_INIT_RESOURCE(discovery);
}

namespace Archcodex {
HostDiscovery::HostDiscovery(QObject *parent) : QObject(parent) {
    initDiscoveryResource();
    startup.setSingleShot(true);
    connect(&startup, &QTimer::timeout, this, [this] {
        stop();
        emit failed(failureDetails("Host discovery did not respond within 15 seconds. Check host "
                                   "Python 3 and Flatpak host-command access."));
    });
    connect(&process, &QProcess::started, this, &HostDiscovery::sendRequest);
    connect(&process, &QProcess::readyReadStandardError, this,
            [this] { stderrTail = (stderrTail + process.readAllStandardError()).right(4096); });
    connect(&process, &QProcess::errorOccurred, this, [this](auto error) {
        if (!stopping && error == QProcess::FailedToStart) {
            startup.stop();
            emit failed(
                failureDetails("Cannot start host discovery: " + process.errorString() +
                               "\nHost Python 3 and Flatpak host-command access are required."));
        }
    });
    connect(&process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](int code, QProcess::ExitStatus status) {
                startup.stop();
                if (!stopping)
                    emit failed(failureDetails(
                        QString("Host discovery %1 (exit code %2). Retry discovery or add a "
                                "connection manually.")
                            .arg(status == QProcess::CrashExit ? "crashed" : "stopped")
                            .arg(code)));
            });
    connect(&process, &QProcess::readyReadStandardOutput, this, [this] {
        buffer += process.readAllStandardOutput();
        if (buffer.size() > 16 * 1024 * 1024) {
            stop();
            emit failed(failureDetails("Host discovery exceeded its 16 MiB response limit."));
            return;
        }
        while (buffer.contains('\n')) {
            const auto end = buffer.indexOf('\n');
            QJsonParseError error;
            const auto message = QJsonDocument::fromJson(buffer.left(end), &error).object();
            buffer.remove(0, end + 1);
            if (error.error != QJsonParseError::NoError || !message["endpoints"].isArray() ||
                !message["issues"].isArray()) {
                stop();
                emit failed(failureDetails("Host discovery failed: " +
                                           message["error"].toString("invalid response")));
                return;
            }
            startup.stop();
            emit snapshot(message["endpoints"].toArray(), message["issues"].toArray());
        }
    });
}
QString HostDiscovery::failureDetails(const QString &summary) {
    if (process.isOpen())
        stderrTail = (stderrTail + process.readAllStandardError()).right(4096);
    const auto diagnostic = QString::fromUtf8(stderrTail).trimmed();
    return summary +
           (diagnostic.isEmpty() ? QString() : "\n\nHelper stderr (last 4 KiB):\n" + diagnostic);
}
HostDiscovery::~HostDiscovery() {
    stop();
}
void HostDiscovery::stop() {
    stopping = true;
    startup.stop();
    process.closeWriteChannel();
    if (process.state() != QProcess::NotRunning && !process.waitForFinished(500)) {
        process.terminate();
        if (!process.waitForFinished(500)) {
            process.kill();
            process.waitForFinished(500);
        }
    }
}
void HostDiscovery::sendRequest() {
    startup.start(15000); // One response deadline per explicit request, not a scan timer.
    process.write(QJsonDocument(QJsonObject{{"paths", QJsonArray::fromStringList(paths)}})
                      .toJson(QJsonDocument::Compact) +
                  '\n');
}
void HostDiscovery::refresh(const QStringList &requestedPaths) {
    paths = requestedPaths;
    if (process.state() == QProcess::Running) {
        sendRequest();
        return;
    }
    if (process.state() == QProcess::Starting)
        return;
    QFile script(":/archcodex/discover.py");
    if (!script.open(QIODevice::ReadOnly)) {
        emit failed("The packaged host discovery script is missing.");
        return;
    }
    QStringList args = {"-I", "-u", "-c", QString::fromUtf8(script.readAll())};
    QString program = "/usr/bin/python3";
    if (QFile::exists("/.flatpak-info")) {
        args.prepend(program);
        args.prepend("--host");
        args.prepend("--watch-bus");
        program = "/usr/bin/flatpak-spawn";
    }
    buffer.clear();
    stderrTail.clear();
    stopping = false;
    startup.start(15000);
    process.start(program, args);
}
} // namespace Archcodex
