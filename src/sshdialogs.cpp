// SPDX-License-Identifier: GPL-3.0-or-later
#include "sshdialogs.h"
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>
#include <sys/prctl.h>
#include <sys/resource.h>

namespace Archcodex {
std::optional<HostProfile> editSshHost(QWidget *parent, const HostProfile &original,
                                       const QMap<QString, HostProfile> &others) {
    auto host = original;
    QDialog dialog(parent);
    dialog.setWindowTitle(others.contains(host.id) ? "Edit SSH host" : "Add SSH host");
    auto *layout = new QFormLayout(&dialog);
    auto *target = new QLineEdit(host.ssh);
    auto *user = new QLineEdit(host.user);
    auto *port = new QSpinBox;
    auto *identity = new QLineEdit(host.identityFile);
    target->setObjectName("sshTarget");
    user->setObjectName("sshUser");
    port->setObjectName("sshPort");
    identity->setObjectName("sshIdentityFile");
    target->setPlaceholderText("SSH alias, hostname or user@host");
    user->setPlaceholderText("Inherit SSH configuration");
    port->setRange(0, 65535);
    port->setSpecialValueText("Inherit SSH configuration");
    port->setValue(host.port);
    identity->setPlaceholderText("Optional: ~/.ssh/key or absolute local-computer path");
    layout->addRow("Host", target);
    layout->addRow("User", user);
    layout->addRow("Port", port);
    layout->addRow("Identity file", identity);
    auto *help = new QLabel("Other SSH options, including jump hosts, use your local computer's "
                            "~/.ssh/config. Passwords and passphrases are requested when "
                            "connecting and are never saved.");
    help->setWordWrap(true);
    layout->addRow(help);
    auto *error = new QLabel;
    error->setObjectName("sshHostError");
    error->setTextFormat(Qt::PlainText);
    error->setWordWrap(true);
    layout->addRow(error);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    layout->addRow(buttons);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        host.ssh = target->text().trimmed();
        host.user = user->text().trimmed();
        host.port = port->value();
        host.identityFile = identity->text().trimmed();
        auto problem = host.isLocal() ? QString("Enter an SSH host.") : host.validate();
        for (const auto &other : others)
            if (other.id != host.id && other.connectionKey() == host.connectionKey())
                problem = "This SSH connection is already configured.";
        if (!problem.isEmpty()) {
            error->setText(problem);
            return;
        }
        dialog.accept();
    });
    dialog.resize(600, 290);
    return dialog.exec() == QDialog::Accepted ? std::optional(host) : std::nullopt;
}

SshPromptDialog::SshPromptDialog(QWidget *parent) : QDialog(parent) {
    setAttribute(Qt::WA_DeleteOnClose);
    setObjectName("sshAuthentication");
    setModal(false);
    auto *layout = new QVBoxLayout(this);
    description = new QPlainTextEdit;
    description->setReadOnly(true);
    description->setMaximumHeight(180);
    description->setObjectName("sshPrompt");
    layout->addWidget(description);
    answer = new QLineEdit;
    answer->setObjectName("sshAnswer");
    answer->setEchoMode(QLineEdit::Password);
    answer->setMaxLength(1023);
    layout->addWidget(answer);
    reveal = new QCheckBox("Show answer");
    layout->addWidget(reveal);
    connect(reveal, &QCheckBox::toggled, this, [this](bool show) {
        answer->setEchoMode(show ? QLineEdit::Normal : QLineEdit::Password);
    });
    auto *note = new QLabel("Your answer is used for this connection only and is never saved.");
    note->setWordWrap(true);
    layout->addWidget(note);
    buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] { submit(false); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(this, &QDialog::finished, this, [this] {
        if (!withdrawn)
            submit(true);
        answer->clear();
    });
    resize(580, 300);
}
void SshPromptDialog::setPrompt(const QString &host, const QJsonObject &next) {
    if (prompt["id"] == next["id"] && prompt["attempt"] == next["attempt"])
        return;
    // Linux core handlers may otherwise retain widget/process memory containing credentials.
    const rlimit limit{0, 0};
    if (setrlimit(RLIMIT_CORE, &limit) != 0 || prctl(PR_SET_DUMPABLE, 0, 0, 0, 0) != 0) {
        prompt = next;
        submit(true);
        description->setPlainText("Cannot protect authentication memory from core dumps. "
                                  "The connection was cancelled.");
        answer->setEnabled(false);
        buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
        return;
    }
    prompt = next;
    sent = false;
    answer->clear();
    reveal->setChecked(false);
    setWindowTitle("SSH authentication · " + host);
    description->setPlainText(next["text"].toString());
    const auto kind = next["kind"].toString();
    const bool entry = kind != "confirm" && kind != "none";
    answer->setVisible(entry);
    answer->setEnabled(true);
    reveal->setVisible(entry);
    buttons->button(QDialogButtonBox::Ok)->setVisible(kind != "none");
    buttons->button(QDialogButtonBox::Ok)->setEnabled(true);
    buttons->button(QDialogButtonBox::Ok)->setText(kind == "confirm" ? "Accept" : "Continue");
    if (entry)
        answer->setFocus();
}
void SshPromptDialog::submit(bool cancelled) {
    if (withdrawn || (sent && !cancelled) || prompt.isEmpty())
        return;
    QJsonObject reply{{"id", prompt["id"]}, {"attempt", prompt["attempt"]}};
    if (cancelled)
        reply["cancel"] = true;
    else
        reply["answer"] = prompt["kind"] == "confirm" ? QString("yes") : answer->text();
    answer->clear();
    sent = true;
    answer->setEnabled(false);
    buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
    emit answered(reply);
}
void SshPromptDialog::withdraw() {
    withdrawn = true;
    answer->clear();
    close();
}
} // namespace Archcodex
