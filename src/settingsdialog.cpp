// SPDX-License-Identifier: GPL-3.0-or-later
#include "settingsdialog.h"
#include "commands.h"
#include "types.h"
#include <KShortcutsEditor>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace Archcodex {
SettingsDialog::SettingsDialog(bool hide, bool showAll, const QString &localExecutable,
                               QWidget *parent, Commands *registry, bool restore)
    : QDialog(parent), commands(registry) {
    setWindowTitle("Configure Archcodex");
    setObjectName("settingsDialog");
    auto *layout = new QVBoxLayout(this);
    auto *body = new QHBoxLayout;
    auto *categories = new QListWidget;
    categories->setObjectName("settingsCategories");
    categories->setAccessibleName("Settings categories");
    categories->setMaximumWidth(170);
    auto *pages = new QStackedWidget;
    body->addWidget(categories);
    body->addWidget(pages, 1);
    layout->addLayout(body, 1);
    // Add pages as real settings become available; no plugin/configuration
    // framework or empty categories are needed for future terminal/backends UI.
    const auto page = [&](const QString &category, const QString &title) {
        categories->addItem(category);
        auto *widget = new QWidget;
        auto *content = new QVBoxLayout(widget);
        auto *heading = new QLabel(title);
        auto font = heading->font();
        font.setBold(true);
        heading->setFont(font);
        content->addWidget(heading);
        pages->addWidget(widget);
        return content;
    };
    const auto help = [](QVBoxLayout *content, const QString &text) {
        auto *label = new QLabel(text);
        label->setWordWrap(true);
        label->setTextFormat(Qt::PlainText);
        content->addWidget(label);
        content->addStretch();
    };
    auto *application = page("Application", "Application preferences");
    restoreOpened = new QCheckBox("Restore opened terminals on startup");
    restoreOpened->setObjectName("restoreOpenedTerminals");
    restoreOpened->setChecked(restore);
    application->addWidget(restoreOpened);
    auto *restoreHelp = new QLabel(
        "Takes effect at next startup. Saves the conversation opened by Archcodex; in-TUI "
        "/resume, /fork, /new and agent switches are not tracked. New-session launchers with "
        "no confirmed ID are remembered as disconnected views. Terminal output is not saved.");
    restoreHelp->setWordWrap(true);
    application->addWidget(restoreHelp);
    hideUnsaved = new QCheckBox("Hide unsaved sessions");
    hideUnsaved->setObjectName("hideUnsavedSetting");
    hideUnsaved->setChecked(hide);
    application->addWidget(hideUnsaved);
    auto *clientLabel = new QLabel("Local Codex executable (this computer)");
    localCodex = new QLineEdit(localExecutable);
    localCodex->setObjectName("localCodexExecutable");
    localCodex->setPlaceholderText("Automatic: detected local Codex, then local PATH");
    localCodex->setClearButtonEnabled(true);
    clientLabel->setBuddy(localCodex);
    application->addWidget(clientLabel);
    application->addWidget(localCodex);
    validation = new QLabel;
    validation->setWordWrap(true);
    validation->setTextFormat(Qt::PlainText);
    validation->hide();
    help(application,
         "Hide entries without saved history. Open terminals bypass this unsaved-session filter. "
         "This setting also appears in the View menu.\n\n"
         "myCodex terminals use Codex on this computer, including for remote containers. "
         "Automatic selection reuses the executable found by local server discovery. "
         "If needed, enter the absolute path reported by command -v codex in your local terminal. "
         "This does not change the executable of a local host server or start an app-server.");

    auto *backends = page("Backends", "myCodex");
    showAllMyCodexSessions =
        new QCheckBox("Show sessions outside the container's working directory");
    showAllMyCodexSessions->setObjectName("showAllMyCodexSessions");
    showAllMyCodexSessions->setChecked(showAll);
    backends->addWidget(showAllMyCodexSessions);
    help(backends,
         "By default, each local or remote myCodex server lists sessions in its container's "
         "WORKDIR and subdirectories. This applies to history/activity tabs and search results. "
         "Opened always lists all open terminal views. "
         "Other shared-history sessions stay hidden until you enable this option.\n\n"
         "Shared-history sessions can be opened only where Archcodex has already observed them "
         "running. "
         "The separate unsaved-session setting still applies.");
    if (commands) {
        auto *shortcuts = page("Keyboard Shortcuts", "Keyboard shortcuts");
        shortcutDraft = commands->draft(this);
        shortcutDraft->setObjectName("shortcutDraft");
        auto *editor = new KShortcutsEditor(
            shortcutDraft, this, KShortcutsEditor::WindowAction | KShortcutsEditor::WidgetAction,
            KShortcutsEditor::LetterShortcutsDisallowed);
        editor->setObjectName("shortcutEditor");
        shortcuts->addWidget(editor, 1);
        auto *defaults = new QPushButton("Restore Defaults");
        defaults->setObjectName("restoreShortcutDefaults");
        shortcuts->addWidget(defaults);
        connect(defaults, &QPushButton::clicked, editor, &KShortcutsEditor::allDefault);
        help(shortcuts,
             "Shortcuts work in this window, including inside terminals. "
             "Archcodex bindings take precedence over terminal bindings. "
             "Use single key combinations. Apply saves; Cancel discards unapplied edits.");
    }
    layout->addWidget(validation);
    connect(categories, &QListWidget::currentRowChanged, pages, &QStackedWidget::setCurrentIndex);
    categories->setCurrentRow(0);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Apply |
                                         QDialogButtonBox::Cancel);
    layout->addWidget(buttons);
    connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, this,
            &SettingsDialog::apply);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        if (apply())
            accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    resize(900, 600);
}
bool SettingsDialog::apply() {
    const auto executable = localCodex->text().trimmed();
    if (!executable.isEmpty() && (!QDir::isAbsolutePath(executable) || !validText(executable))) {
        validation->setText(
            "Enter an absolute path on this computer, or leave it empty for automatic selection.");
        validation->show();
        localCodex->setFocus();
        return false;
    }
    validation->hide();
    if (commands) {
        QString error;
        if (!commands->apply(Commands::bindings(shortcutDraft), &error)) {
            validation->setText(error);
            validation->show();
            return false;
        }
    }
    emit settingsApplied(hideUnsaved->isChecked(), showAllMyCodexSessions->isChecked(), executable,
                         restoreOpened->isChecked());
    return true;
}
} // namespace Archcodex
