// SPDX-License-Identifier: GPL-3.0-or-later
#include "hosts.h"
#include "launch.h"
#include "types.h"
#include <QJsonObject>
#include <QSet>

namespace Archcodex {
QJsonObject HostProfile::json() const {
    return {{"id", id},
            {"ssh", ssh},
            {"enabled", isLocal() || enabled},
            {"servers", isLocal() || servers},
            {"containers", containers}};
}
QMap<QString, HostProfile> readHosts(const QJsonArray &records, bool legacy) {
    QMap<QString, HostProfile> hosts;
    QSet<QString> addresses;
    for (const auto &value : records) {
        const auto record = value.toObject();
        if (!record["id"].isString() || !record["ssh"].isString())
            continue;
        HostProfile host{record["id"].toString(), record["ssh"].toString()};
        if (host.id.isEmpty() || !validText(host.id, 128) || hosts.contains(host.id) ||
            addresses.contains(host.ssh) || (!host.isLocal() && !validSshHost(host.ssh)))
            continue;
        host.enabled = record["enabled"].toBool(true);
        host.servers = record["servers"].toBool(true);
        host.containers = record["containers"].toBool(true);
        if (host.isLocal()) {
            if (legacy)
                host.containers = host.enabled;
            host.enabled = host.servers = true;
        }
        addresses.insert(host.ssh);
        hosts.insert(host.id, host);
    }
    if (!addresses.contains(QString())) {
        // Preserve the existing local identity used in container backend IDs.
        QString id = "local-mycodex";
        while (hosts.contains(id))
            id += "-local";
        hosts.insert(id, HostProfile{id, {}});
    }
    return hosts;
}
} // namespace Archcodex
