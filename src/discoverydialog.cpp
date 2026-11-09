// SPDX-License-Identifier: GPL-3.0-or-later
#include "discoverydialog.h"
#include <QApplication>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QJsonObject>
#include <QLabel>
#include <QLocale>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QToolButton>
#include <QVBoxLayout>

namespace Archcodex {
namespace {
QLabel *textLabel(const QString &text) {
    auto *label = new QLabel(text);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    label->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return label;
}
} // namespace
DiscoveryDialog::DiscoveryDialog(QWidget *parent) : QDialog(parent) {
    setObjectName("discoveryDetails");
    setWindowTitle("Discovery details");
    setAttribute(Qt::WA_DeleteOnClose);
    resize(740, 520);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(18, 18, 18, 18);
    layout->setSpacing(12);
    heading = textLabel({});
    auto font = heading->font();
    font.setPointSizeF(font.pointSizeF() + 2);
    font.setBold(true);
    heading->setFont(font);
    context = textLabel({});
    checked = textLabel({});
    layout->addWidget(heading);
    layout->addWidget(context);
    layout->addWidget(checked);
    scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *content = new QWidget;
    cards = new QVBoxLayout(content);
    cards->setContentsMargins(0, 0, 0, 0);
    cards->setAlignment(Qt::AlignTop);
    scroll->setWidget(content);
    layout->addWidget(scroll, 1);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    retry = buttons->addButton("Retry discovery", QDialogButtonBox::ActionRole);
    retry->setObjectName("retryDiscovery");
    auto *add = buttons->addButton("Add connection…", QDialogButtonBox::ActionRole);
    auto *copy = buttons->addButton("Copy report", QDialogButtonBox::ActionRole);
    copy->setObjectName("copyDiscoveryReport");
    connect(retry, &QPushButton::clicked, this, &DiscoveryDialog::retryRequested);
    connect(add, &QPushButton::clicked, this, &DiscoveryDialog::addConnectionRequested);
    connect(copy, &QPushButton::clicked, this,
            [this] { QApplication::clipboard()->setText(copyText); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
    layout->addWidget(buttons);
}
void DiscoveryDialog::setReport(const DiscoveryReport &report) {
    auto issues = report.issues;
    if (!report.failure.isEmpty())
        issues.prepend(QJsonObject{
            {"code", "helper-failure"},
            {"summary", "Discovery could not finish"},
            {"explanation", "Automatic discovery is unavailable. Existing connections and saved "
                            "sessions are retained."},
            {"action",
             "Retry discovery. You can also add an existing server's Unix socket manually."},
            {"details", QJsonArray{report.failure}}});
    const auto title =
        report.inProgress   ? "Checking local servers…"
        : !issues.isEmpty() ? "Local discovery needs attention"
        : report.checked.isValid()
            ? (report.endpoints ? "Local discovery is up to date" : "No local servers found")
            : "Local discovery has not run yet";
    heading->setText(title);
    context->setText("Discovery checks Codex servers visible to your user on this host.");
    checked->setText(report.checked.isValid()
                         ? "Last check: " + QLocale().toString(report.checked.toLocalTime(),
                                                               QLocale::ShortFormat)
                         : "Choose Retry discovery to check this host.");
    retry->setEnabled(!report.inProgress);
    copyText = "Archcodex " + QString(ARCHCODEX_VERSION) + " — " + title + "\n" + checked->text() +
               "\n" + context->text();
    for (const auto &value : issues) {
        const auto issue = value.toObject();
        copyText += "\n\n" + issue["summary"].toString() + "\n" + issue["explanation"].toString() +
                    "\n" + issue["action"].toString();
        for (const auto &line : issue["details"].toArray())
            copyText += "\n" + line.toString();
        if (issue["omitted"].toInt())
            copyText +=
                QString("\n%1 additional affected entries omitted.").arg(issue["omitted"].toInt());
    }
    // Keep selection, scroll and expanded details stable on ordinary connection updates.
    const QString emptyText =
        report.inProgress && !report.checked.isValid()
            ? "The first check is in progress. Results will appear here when it finishes."
        : report.checked.isValid() && report.endpoints
            ? "No discovery access problems were reported by the last check."
            : "Start Codex on this host and retry, or add a connection if you know its socket.";
    if (issues == displayedIssues && cards->count() &&
        (!issues.isEmpty() || emptyText == displayedEmptyText))
        return;
    displayedEmptyText = emptyText;
    QSet<QString> expanded;
    for (auto *button : scroll->findChildren<QToolButton *>())
        if (button->isChecked())
            expanded.insert(button->property("issueCode").toString());
    const auto position = scroll->verticalScrollBar()->value();
    while (auto *item = cards->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    displayedIssues = issues;
    if (issues.isEmpty()) {
        cards->addWidget(textLabel(emptyText));
    }
    for (const auto &value : issues) {
        const auto issue = value.toObject();
        auto *card = new QFrame;
        card->setFrameShape(QFrame::StyledPanel);
        card->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
        auto *layout = new QVBoxLayout(card);
        layout->setContentsMargins(12, 12, 12, 12);
        auto *summary = textLabel(issue["summary"].toString());
        auto font = summary->font();
        font.setBold(true);
        summary->setFont(font);
        layout->addWidget(summary);
        layout->addWidget(textLabel(issue["explanation"].toString()));
        layout->addWidget(textLabel(issue["action"].toString()));
        QStringList details;
        for (const auto &line : issue["details"].toArray())
            details << line.toString();
        if (issue["omitted"].toInt())
            details
                << QString("%1 additional affected entries omitted.").arg(issue["omitted"].toInt());
        if (!details.isEmpty()) {
            auto *toggle = new QToolButton;
            const auto code = issue["code"].toString();
            toggle->setProperty("issueCode", code);
            toggle->setText("Technical details");
            toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
            toggle->setCheckable(true);
            toggle->setChecked(expanded.contains(code));
            toggle->setArrowType(toggle->isChecked() ? Qt::DownArrow : Qt::RightArrow);
            auto *technical = new QPlainTextEdit(details.join('\n'));
            technical->setReadOnly(true);
            technical->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
            technical->setAccessibleName("Discovery technical details");
            technical->setMaximumHeight(150);
            technical->setVisible(toggle->isChecked());
            connect(toggle, &QToolButton::toggled, technical, [toggle, technical](bool open) {
                toggle->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
                technical->setVisible(open);
            });
            layout->addWidget(toggle);
            layout->addWidget(technical);
        }
        cards->addWidget(card);
    }
    scroll->verticalScrollBar()->setValue(position);
}
} // namespace Archcodex
