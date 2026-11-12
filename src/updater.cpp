// SPDX-License-Identifier: GPL-3.0-or-later
#include "updater.h"
#include "launch.h"
#include <QFile>
#include <QJsonDocument>
#include <QSettings>

namespace Archcodex {
Updater::Updater(const QString &version, const QString &directory, QObject *parent,
                 const QStringList &command)
    : QObject(parent), helper(command) {
    context = {{"version", version}, {"data_dir", directory}};
    if (QFile::exists("/.flatpak-info")) {
        QSettings info("/.flatpak-info", QSettings::IniFormat);
        context["app_path"] = info.value("Instance/app-path").toString();
        context["instance_id"] = info.value("Instance/instance-id").toString();
    }
    if (helper.isEmpty())
        helper = hostCommand({"/usr/bin/python3", "-I", "-u", "-c", resourceScript("updates.py")});
    deadline.setSingleShot(true);
    connect(&deadline, &QTimer::timeout, this, [this] {
        fail("The update operation timed out. Check the host installation before retrying.");
    });
}
Updater::~Updater() {
    if (process) {
        process->disconnect(this);
        process->terminate();
        // QProcess owns the host-command wrapper, not a persistent updater service.
        process->kill();
    }
}
bool Updater::busy() const {
    return process != nullptr;
}
bool Updater::finishing() const {
    return status == State::Installing || status == State::Restarting;
}
void Updater::setState(State value) {
    status = value;
    emit changed();
}
void Updater::check() {
    if (!busy())
        start("check");
}
void Updater::install() {
    if (!busy() && status == State::Available && reason.isEmpty())
        start("install");
}
void Updater::restart() {
    if (!busy() && status == State::Installed)
        start("restart");
}
void Updater::cancel() {
    if (!process || finishing())
        return;
    cancelled = true;
    deadline.stop();
    process->terminate();
    auto *pending = process;
    QTimer::singleShot(2000, pending, [pending] {
        if (pending->state() != QProcess::NotRunning)
            pending->kill();
    });
}
void Updater::fail(const QString &message) {
    if (failure.isEmpty())
        failure = message;
    deadline.stop();
    if (process)
        process->kill();
}
void Updater::start(const QString &action) {
    operation = action;
    completion = {};
    buffer.clear();
    diagnostics.clear();
    failure.clear();
    cancelled = false;
    percent = 0;
    process = new QProcess(this);
    auto *pending = process;
    connect(pending, &QProcess::readyReadStandardOutput, this, &Updater::receive);
    connect(pending, &QProcess::readyReadStandardError, this, [this, pending] {
        diagnostics = (diagnostics + pending->readAllStandardError()).right(4096);
    });
    connect(pending, &QProcess::started, this, [this, pending] {
        pending->write(
            QJsonDocument(
                QJsonObject{{"operation", operation}, {"context", context}, {"release", selected}})
                .toJson(QJsonDocument::Compact) +
            '\n');
        if (operation != "install")
            pending->closeWriteChannel();
    });
    connect(pending, &QProcess::errorOccurred, this, [this, pending](auto error) {
        if (error == QProcess::FailedToStart) {
            deadline.stop();
            failure = "Cannot start the host updater: " + pending->errorString();
            process = nullptr;
            pending->deleteLater();
            setState(State::Error);
        }
    });
    connect(pending, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this, pending](int code, auto exit) {
                receive();
                deadline.stop();
                process = nullptr;
                pending->deleteLater();
                if (cancelled) {
                    setState(State::Idle);
                    return;
                }
                if (code != 0 || exit != QProcess::NormalExit || completion.isEmpty() ||
                    !buffer.isEmpty() || !failure.isEmpty()) {
                    if (failure.isEmpty())
                        failure = "The host updater stopped without completing the operation. " +
                                  QString::fromUtf8(diagnostics).trimmed();
                    setState(State::Error);
                    return;
                }
                const auto event = completion["event"].toString();
                if (((event == "available" || event == "current") &&
                     completion["release"].toObject()["version"].toString().isEmpty()) ||
                    (event == "installed" && completion["version"].toString().isEmpty())) {
                    failure = "The host updater returned an incomplete result.";
                    setState(State::Error);
                    return;
                }
                if (event == "available" || event == "current") {
                    selected = completion["release"].toObject();
                    reason = completion["install_reason"].toString();
                    setState(event == "available" ? State::Available : State::Current);
                } else if (event == "installed") {
                    installed = completion["version"].toString();
                    setState(State::Installed);
                } else {
                    setState(State::RestartReady);
                    emit restartReady();
                }
            });
    setState(action == "check"     ? State::Checking
             : action == "install" ? State::Downloading
                                   : State::Restarting);
    deadline.start(action == "install" ? 25 * 60 * 1000 : 90000);
    pending->start(helper.first(), helper.mid(1));
}
void Updater::receive() {
    if (!process)
        return;
    buffer += process->readAllStandardOutput();
    if (buffer.size() > 1024 * 1024) {
        fail("The host updater exceeded its response limit.");
        return;
    }
    while (buffer.contains('\n')) {
        const auto end = buffer.indexOf('\n');
        QJsonParseError parse;
        const auto document = QJsonDocument::fromJson(buffer.left(end), &parse);
        buffer.remove(0, end + 1);
        if (parse.error != QJsonParseError::NoError || !document.isObject() ||
            !completion.isEmpty()) {
            fail("Invalid response from the host updater.");
            return;
        }
        const auto object = document.object();
        const auto event = object["event"].toString();
        if (event == "error") {
            fail(object["message"].toString("The host updater reported an error."));
            return;
        }
        if (operation == "install" && event == "downloading" && status == State::Downloading) {
            percent = qBound(0, object["percent"].toInt(), 100);
            emit changed();
        } else if (operation == "install" && event == "ready_to_install" &&
                   status == State::Downloading) {
            if (!cancelled) {
                setState(State::Installing);
                process->write("install\n");
                process->closeWriteChannel();
            }
        } else if ((operation == "check" &&
                    (event == "available" || event == "current" || event == "installed")) ||
                   (operation == "install" && event == "installed" &&
                    status == State::Installing) ||
                   (operation == "restart" && event == "restart_ready")) {
            completion = object;
        } else {
            fail("Unexpected response from the host updater.");
            return;
        }
    }
}
} // namespace Archcodex
