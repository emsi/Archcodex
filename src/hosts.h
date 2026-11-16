// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QJsonArray>
#include <QMap>
#include <QString>

namespace Archcodex {
struct HostProfile {
    QString id, ssh;
    bool enabled = true, servers = true, containers = true;
    bool isLocal() const {
        return ssh.isEmpty();
    }
    QJsonObject json() const;
};
// Legacy local disabling applied only to myCodex. The local host itself is permanent.
QMap<QString, HostProfile> readHosts(const QJsonArray &records, bool legacy = false);
} // namespace Archcodex
