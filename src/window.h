// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "backend.h"
#include "store.h"
#include <QMainWindow>
#include <QModelIndex>
#include <memory>

class QTreeView;
class QTabBar;
class QLineEdit;
class QComboBox;
class QLabel;
class QPushButton;
class QSplitter;
namespace Archcodex {
class SessionModel;
class SessionFilter;
class TerminalArea;
class HostDiscovery;
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
    std::unique_ptr<Store> store;
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
    QString discoveryMessage;
    QSet<QString> paused, ignoredSockets, discoveredPresent;
    QHash<QString, QString> discoveredPaths;
    bool searchingView = false, storageFailed = false;
    SessionModel *model, *results;
    SessionFilter *filter;
    QTreeView *tree;
    QTabBar *tabs;
    QLineEdit *searchBox;
    QComboBox *scope;
    QLabel *notice, *terminalTitle, *connections;
    QPushButton *loadMore;
    QSplitter *splitter;
    TerminalArea *terminal;
    HostDiscovery *discovery;
    QHash<int, QStringList> expanded;
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
    void applyDiscovery(const QJsonArray &endpoints, const QStringList &warnings);
    void activate(const QModelIndex &index);
    void search();
    void clearSearch();
    void updateSummary();
    void updateSearchSummary();
    bool persist(const std::function<void()> &operation);
    void error(const QString &message);
};
} // namespace Archcodex
