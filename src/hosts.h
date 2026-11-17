// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QString>

namespace Archcodex {
struct HostProfile {
    QString id, ssh;
    bool enabled = true, servers = true, containers = true;
    QString user = {}, identityFile = {};
    QString generation = {}; // Changes only when the configured SSH destination/options are edited.
    int port = 0;            // Unspecified inherits OpenSSH configuration.
    bool isLocal() const {
        return ssh.isEmpty();
    }
    QJsonObject json() const;
    QJsonObject sshOptions() const;
    QString validate() const;
    QString connectionKey() const;
    QString label() const;
    QString discoveryScope() const;
};
// Legacy local disabling applied only to myCodex. The local host itself is permanent.
QMap<QString, HostProfile> readHosts(const QJsonArray &records, bool legacy = false);
} // namespace Archcodex
