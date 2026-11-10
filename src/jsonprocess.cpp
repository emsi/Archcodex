// SPDX-License-Identifier: GPL-3.0-or-later
#include "jsonprocess.h"
#include <QJsonDocument>
namespace Archcodex {
JsonProcess::JsonProcess(QObject *owner) : reaper(owner) {
    deadline.setParent(this);
    deadline.setSingleShot(true);
    connect(&deadline, &QTimer::timeout, this,
            [this] { fail("Discovery did not respond within 30 seconds."); });
}
JsonProcess::~JsonProcess() {
    stop();
}
void JsonProcess::stop() {
    ++generation;
    deadline.stop();
    latest = {};
    initialRequest = {};
    if (!process)
        return;
    auto *p = process;
    process = nullptr;
    p->disconnect(this);
    p->setParent(reaper);
    if (p->state() == QProcess::NotRunning)
        p->deleteLater();
    else {
        connect(p, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), p,
                &QObject::deleteLater);
        p->closeWriteChannel();
        QTimer::singleShot(1500, p, [p] {
            if (p->state() != QProcess::NotRunning)
                p->terminate();
            QTimer::singleShot(1000, p, [p] {
                if (p->state() != QProcess::NotRunning)
                    p->kill();
            });
        });
    }
}
void JsonProcess::fail(const QString &reason) {
    const auto diagnostic = QString::fromUtf8(tail).trimmed();
    stop();
    emit failed(reason + (diagnostic.isEmpty() ? QString() : "\n" + diagnostic));
}
void JsonProcess::request(const QJsonObject &data) {
    if (!process)
        return;
    const auto bytes = QJsonDocument(data).toJson(QJsonDocument::Compact) + '\n';
    if (bytes.size() > 256 * 1024 || process->bytesToWrite() > 256 * 1024) {
        fail("Discovery request exceeded its limit.");
        return;
    }
    // QProcess buffers writes made while Starting. Sending those before the
    // captured startup request could restore an obsolete host configuration.
    if (process->state() == QProcess::Starting) {
        initialRequest = data;
        return;
    }
    deadline.start(30000);
    process->write(bytes);
}
void JsonProcess::start(const QStringList &command, const QJsonObject &data) {
    stop();
    buffer.clear();
    tail.clear();
    if (command.isEmpty()) {
        emit failed("Missing discovery command.");
        return;
    }
    process = new QProcess(this);
    initialRequest = data;
    const auto epoch = generation;
    connect(process, &QProcess::readyReadStandardError, this, [this] {
        if (process)
            tail = (tail + process->readAllStandardError()).right(4096);
    });
    connect(process, &QProcess::readyReadStandardOutput, this, &JsonProcess::receive);
    connect(process, &QProcess::started, this, [this] {
        const auto current = initialRequest;
        initialRequest = {};
        request(current);
    });
    connect(process, &QProcess::errorOccurred, this, [this, epoch](auto error) {
        if (epoch == generation && error == QProcess::FailedToStart)
            fail("Cannot start discovery: " + process->errorString());
    });
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this, epoch](int code, auto) {
                if (epoch == generation)
                    fail(QString("Discovery stopped (exit %1).").arg(code));
            });
    deadline.start(30000);
    process->start(command.first(), command.mid(1));
}
void JsonProcess::receive() {
    if (!process)
        return;
    buffer += process->readAllStandardOutput();
    if (buffer.size() > 16 * 1024 * 1024) {
        fail("Discovery output exceeded its limit.");
        return;
    }
    int lines = 0;
    while (buffer.contains('\n')) {
        if (++lines > 32) {
            const auto epoch = generation;
            QTimer::singleShot(0, this, [this, epoch] {
                if (epoch == generation)
                    receive();
            });
            break;
        }
        const auto end = buffer.indexOf('\n');
        QJsonParseError error;
        const auto doc = QJsonDocument::fromJson(buffer.left(end), &error);
        buffer.remove(0, end + 1);
        if (error.error != QJsonParseError::NoError || !doc.isObject() ||
            doc.object().contains("error")) {
            fail("Invalid discovery response: " + doc.object()["error"].toString());
            return;
        }
        deadline.stop();
        latest = doc.object();
    }
    if (!inFlight)
        acknowledge();
}
void JsonProcess::acknowledge() {
    inFlight = false;
    if (!latest.isEmpty()) {
        const auto data = latest;
        latest = {};
        inFlight = true;
        emit message(data);
    }
}
} // namespace Archcodex
