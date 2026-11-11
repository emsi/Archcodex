// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "types.h"
#include <QDBusConnection>
#include <QHash>
#include <QObject>
#include <QPointer>

class KNotification;
class QWidget;
namespace Archcodex {
// Presentation only. Backends produce live events; this class never queries Codex.
class Notifications : public QObject {
    Q_OBJECT
  public:
    explicit Notifications(QObject *parent = nullptr);
    Notifications(bool portal, const QDBusConnection &bus, QObject *parent = nullptr);
    ~Notifications() override;
    void show(const SessionAttention &event, const BackendProfile &profile);
    void setEnabled(bool enabled);
    void setMyCodexWorkdirOnly(bool enabled);
    void withdrawBackend(const QString &backend);
    void withdrawSession(const QString &backend, const QString &thread);
    void setViewedSession(const QString &backend, const QString &thread);
    static void presentWindow(QWidget *window, const QString &activationToken);
  signals:
    void openRequested(const QString &backend, const QString &thread, const QString &token);
  private slots:
    void portalAction(const QString &id, const QString &action, const QVariantList &parameters);

  private:
    struct Target {
        Session session;
        BackendProfile profile;
        QPointer<KNotification> native;
        quint64 delivery = 0;
    };
    bool enabled = true, portal;
    bool myCodexWorkdirOnly = true;
    quint64 nextDelivery = 0;
    int pendingDeliveries = 0;
    QDBusConnection bus;
    QHash<QString, Target> targets;
    QStringList order;
    QString viewedSession;
    bool inScope(const Session &session, const BackendProfile &profile) const;
    void remove(const QString &id);
    void activate(const QString &id, const QString &token);
    void clear();
};
} // namespace Archcodex
