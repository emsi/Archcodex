// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "asyncstore.h"
#include "backend.h"
#include "containerdiscovery.h"
#include "discovery.h"
#include "store.h"
#include <QMainWindow>
#include <QModelIndex>
#include <QPointer>
#include <memory>

class QTreeView;
class QTabBar;
class QLineEdit;
class QComboBox;
class QLabel;
class QPushButton;
class QSplitter;
class QToolButton;
namespace Archcodex {
class SessionModel;
class SessionFilter;
class TerminalArea;
class DiscoveryDialog;
class Window : public QMainWindow {
    Q_OBJECT
  public:
    explicit Window(const QString &dataDirectory, bool connectOnStart = true);
    ~Window() override;
    bool smokeTest(QString *error);
  signals:
    void connectionSummaryChanged();

  protected:
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
    QString scopeBackend, scopePath, pendingOpen;
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
    bool searchingView = false, storageFailed = false;
    SessionModel *model, *results;
    SessionFilter *filter;
    QTreeView *tree;
    QTabBar *tabs;
    QLineEdit *searchBox;
    QComboBox *scope;
    QLabel *searchNotice, *terminalTitle, *connections;
    QPushButton *loadMore;
    QSplitter *splitter;
    TerminalArea *terminal;
    HostDiscovery *discovery;
    QHash<int, QStringList> expanded;
    QHash<int, QSet<QString>> collapsed;
    QString searchSelection;
    void expandActivityViews();
    int previousTab = 0;
    QStringList topologyExpanded;
    QString topologySelected;
    QSet<QString> seenRoots;
    void refreshNames();
    void clientStateChanged(const Session &session);
    void attach(const BackendProfile &profile, bool start);
    void connectionsDialog();
    void editConnection(const QString &id = {});
    void discover();
    void applyDiscovery(const QJsonArray &endpoints, const QJsonArray &issues);
    void activate(const QModelIndex &index);
    void search();
    void clearSearch();
    void updateSummary();
    void updateSearchSummary();
    bool persist(const std::function<void()> &operation);
    void error(const QString &message);
};
} // namespace Archcodex
