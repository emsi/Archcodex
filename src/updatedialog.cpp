// SPDX-License-Identifier: GPL-3.0-or-later
#include "updatedialog.h"
#include "updater.h"
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>

namespace Archcodex {
UpdateDialog::UpdateDialog(Updater *updater, const QString &version, QWidget *parent)
    : QDialog(parent) {
    setWindowTitle("Archcodex Updates");
    setObjectName("updateDialog");
    setAttribute(Qt::WA_DeleteOnClose);
    auto *layout = new QVBoxLayout(this);
    auto *message = new QLabel;
    message->setObjectName("updateMessage");
    message->setWordWrap(true);
    message->setTextFormat(Qt::PlainText);
    layout->addWidget(message);
    auto *progress = new QProgressBar;
    layout->addWidget(progress);
    auto *notes = new QPlainTextEdit;
    notes->setReadOnly(true);
    notes->setPlaceholderText("Release notes appear here when a release is available.");
    layout->addWidget(notes);
    auto *buttons = new QHBoxLayout;
    const auto button = [&](const QString &label, const QString &name) {
        auto *b = new QPushButton(label);
        b->setObjectName(name);
        buttons->addWidget(b);
        return b;
    };
    auto *check = button("Check Again", "updateCheck");
    auto *page = button("Release Page", "updateReleasePage");
    auto *install = button("Download and Install", "updateInstall");
    auto *skip = button("Skip This Version", "updateSkip");
    auto *restart = button("Restart Now", "updateRestart");
    auto *cancel = button("Cancel Download", "updateCancel");
    auto *close = button("Close", "updateClose");
    layout->addLayout(buttons);
    connect(check, &QPushButton::clicked, updater, &Updater::check);
    connect(install, &QPushButton::clicked, updater, &Updater::install);
    connect(restart, &QPushButton::clicked, updater, &Updater::restart);
    connect(cancel, &QPushButton::clicked, updater, &Updater::cancel);
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    connect(page, &QPushButton::clicked, this, [updater] {
        QDesktopServices::openUrl(QUrl(updater->release()["page"].toString(
            "https://github.com/emsi/Archcodex/releases/latest")));
    });
    connect(skip, &QPushButton::clicked, this, [this, updater] {
        emit skipVersion(updater->release()["version"].toString());
        this->close();
    });
    const auto refresh = [=] {
        using State = Updater::State;
        const auto state = updater->state();
        const auto available = updater->release()["version"].toString();
        QString text = "Running version: " + version + "\n\n";
        switch (state) {
        case State::Idle:
            text += "Check GitHub for a newer stable release.";
            break;
        case State::Checking:
            text += "Checking GitHub…";
            break;
        case State::Current:
            text += "You are up to date. Latest public release: " + available;
            break;
        case State::Available:
            text +=
                "Version " + available + " is available.\n\n" +
                (updater->installReason().isEmpty()
                     ? QString(
                           "Download and Install updates your per-user Flatpak on this computer. "
                           "You can keep working and restart when ready. Flatpak may also download "
                           "required runtimes.")
                     : updater->installReason());
            break;
        case State::Downloading:
            text += "Downloading and verifying version " + available + "…";
            break;
        case State::Installing:
            text += "Installing version " + available +
                    "… You can keep working. Wait for installation to finish before quitting.";
            break;
        case State::Installed:
            text += "Version " + updater->installedVersion() +
                    " is installed. Restart now or continue working until the next launch.\n\n"
                    "Restart uses your workspace restoration settings. Terminal scrollback is not "
                    "saved.";
            break;
        case State::Error:
            text += updater->error();
            break;
        case State::Restarting:
            text += "Preparing to restart…";
            break;
        case State::RestartReady:
            text += "Restarting…";
            break;
        }
        message->setText(text);
        const auto releaseNotes = updater->release()["notes"].toString();
        if (notes->toPlainText() != releaseNotes)
            notes->setPlainText(releaseNotes);
        check->setEnabled(!updater->busy());
        install->setVisible(state == State::Available);
        install->setEnabled(updater->installReason().isEmpty());
        skip->setVisible(state == State::Available);
        restart->setVisible(state == State::Installed);
        cancel->setVisible(state == State::Downloading || state == State::Checking);
        cancel->setText(state == State::Checking ? "Cancel Check" : "Cancel Download");
        close->setText(state == State::Installed ? "Later" : "Close");
        progress->setVisible(updater->busy());
        progress->setRange(0, state == State::Downloading ? 100 : 0);
        progress->setValue(updater->progress());
    };
    connect(updater, &Updater::changed, this, refresh);
    refresh();
    resize(850, 420);
}
} // namespace Archcodex
