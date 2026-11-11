// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "asyncstore.h"
#include "backend.h"
#include "containerdiscovery.h"
#include "discovery.h"
#include "sessionmodel.h"
#include "store.h"
#include <QMainWindow>
#include <QModelIndex>
#include <QPointer>
#include <memory>
#include <optional>

class QTreeView;
class QTabBar;
class QLineEdit;
class QComboBox;
class QLabel;
class QPushButton;
class QSplitter;
class QToolButton;
namespace Archcodex {
class TerminalArea;
class DiscoveryDialog;
class Commands;
class OpeningCoordinator;
class Notifications;
class Window : public QMainWindow {
    Q_OBJECT
  public:
    explicit Window(const QString &dataDirectory, bool connectOnStart = true);
    ~Window() override;
    bool smokeTest(QString *error);
  signals:
    void connectionSummaryChanged();

  protected:
    bool event(QEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

  private:
    std::unique_ptr<AsyncStore> store;
    QString dataDirectory;
    QHash<QString, BackendProfile> profiles;
    QHash<QString, Backend *> backends;
    QHash<QString, Session> sessions, searchSessions;
    QHash<QString, QString> connectionStates;
    QHash<QString, bool> connectionReady, more;
    QSet<QString> searching;
    QStringList searchErrors;
    quint64 searchToken = 0;
    QSet<QString> scopeBackends;
    bool scopedSearch = false;
    QString scopePath;
    QSet<QString> pendingForks;
    quint64 nextStartToken = 0;
    QHash<quint64, QPointer<Backend>> pendingStarts;
    QString localUser, localHost;
    QString configuredLocalCodex, discoveredLocalCodex;
    bool localIdentityDiscovered = false;
    QHash<QString, QString> containerHostNames;
    DiscoveryReport discoveryReport;
    QPointer<DiscoveryDialog> discoveryDialog;
    QToolButton *discoveryStatus;
    QToolButton *containerStatus;
    QHash<QString, ContainerDiscovery *> containerDiscovery;
    QHash<QString, QString> containerHosts, containerFeedback;
    QSet<QString> disabledContainerHosts, emptyContainers, containerProblems;
    void containerHostsDialog();
    void addContainerHost(const QString &id, const QString &host, bool start);
    void saveContainerHosts();
    void applyContainers(const QString &host, const QJsonObject &inventory);
    void updateContainerSummary();
    void applySessions(const QList<Session> &items);
    QList<Session> cachePending;
    void restoreCache(bool connectOnStart);
    void showDiscoveryDetails();
    QSet<QString> paused, ignoredSockets, discoveredPresent;
    QHash<QString, QString> discoveredPaths;
    // A submitted All search may continue while a different tab is visible.
    bool globalSearchActive = false, storageFailed = false;
    bool changingExplorer = false;
    SessionModel *model, *results;
    SessionFilter *filter;
    QTreeView *tree;
    QTabBar *tabs;
    QLineEdit *searchBox;
    QComboBox *scope;
    QLabel *searchNotice, *connections;
    QPushButton *loadMore;
    QSplitter *splitter;
    TerminalArea *terminal = nullptr;
    OpeningCoordinator *opening;
    Notifications *notifications = nullptr;
    void updateViewedSession();
    QToolButton *workspaceStatus;
    bool workspaceLoaded = false, workspaceWritable = false, workspaceSaving = false;
    bool closing = false;
    qint64 workspaceRevision = 0;
    QString workspaceError;
    void queueWorkspaceSave();
    void saveWorkspace();
    void freezeWorkspace();
    void updateWorkspaceStatus();
    void showWorkspaceDetails();
    void openSession(const QString &backend, const Session &session);
    Commands *commands;
    QWidget *projectPane;
    bool projectsContext = true;
    struct CommandTarget {
        QString backend, thread, path, key;
        NodeKind kind = NodeKind::Group;
    };
    std::optional<CommandTarget> paletteTarget;
    CommandTarget commandTarget() const;
    void updateCommandAvailability();
    void showCommandPalette();
    void contextualCommand(const QString &name);
    HostDiscovery *discovery;
    struct NavigationState {
        QStringList expanded;
        QSet<QString> collapsed;
        QString selected;
        bool saved = false;
    };
    struct ExplorerState {
        QString query;
        int scopeIndex = 0;
        SessionQuery selection;
        QString scopeLabel;
        bool scopeCaptured = false, filtering = false;
        NavigationState navigation, unfiltered;
    };
    QHash<int, ExplorerState> explorer;
    void rememberNavigation(int tab);
    void beginFiltering();
    void endFiltering();
    bool captureScope();
    void queryChanged(const QString &text);
    void switchExplorer(int tab);
    void showExplorer();
    void updateSearchControls();
    bool showingSearch() const;
    void expandActivityViews();
    int previousTab = 0;
    QStringList topologyExpanded;
    QString topologySelected;
    QSet<QString> seenRoots;
    void refreshNames();
    QString groupKey(const BackendProfile &profile) const;
    QString groupLabel(const BackendProfile &profile) const;
    void presentProfile(const BackendProfile &profile, const QString &label = {});
    void terminalFocused(const QString &key);
    void syncCurrentSession();
    void clientStateChanged(const Session &session);
    void restartTerminal(const Session &session);
    void showSettings();
    void attach(const BackendProfile &profile, bool start);
    void connectionsDialog();
    void editConnection(const QString &id = {});
    void discover();
    void applyDiscovery(const QJsonArray &endpoints, const QJsonArray &issues);
    void activate(const QModelIndex &index);
    void activateSession(const QString &backend, const QString &thread,
                         bool requireConnected = false);
    void sessionContextMenu(const QPoint &position);
    void newSession(const QString &backend, const QString &path);
    void forkSession(const QString &backend, const QString &thread);
    void search();
    void clearSearch();
    void updateSummary();
    void updateSearchSummary();
    bool persist(const std::function<void()> &operation);
    void error(const QString &message);
};
} // namespace Archcodex
