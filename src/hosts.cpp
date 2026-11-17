// SPDX-License-Identifier: GPL-3.0-or-later
#include "hosts.h"
#include "launch.h"
#include "types.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>

namespace Archcodex {
QJsonObject HostProfile::json() const {
    return {{"id", id},
            {"ssh", ssh},
            {"enabled", isLocal() || enabled},
            {"servers", isLocal() || servers},
            {"containers", containers},
            {"sshOptions", sshOptions()},
            {"generation", generation}};
}
QJsonObject HostProfile::sshOptions() const {
    QJsonObject options;
    if (!user.isEmpty())
        options["user"] = user;
    if (port)
        options["port"] = port;
    if (!identityFile.isEmpty())
        options["identityFile"] = identityFile;
    return options;
}
QString HostProfile::validate() const {
    if (!generation.isEmpty() && !QRegularExpression("^[a-f0-9]{32}$").match(generation).hasMatch())
        return "Invalid host connection generation.";
    if (id.isEmpty() || !validText(id, 128))
        return "Invalid host identity.";
    if (isLocal())
        return sshOptions().isEmpty() ? QString() : "This Computer cannot have SSH options.";
    if (!validSshHost(ssh))
        return "Enter an SSH alias, hostname or user@host without command options.";
    static const QRegularExpression username("^[A-Za-z0-9_][A-Za-z0-9_.@+\\-]{0,127}$");
    if (!user.isEmpty() && (!username.match(user).hasMatch() || ssh.contains('@')))
        return "Specify the SSH user either separately or as user@host.";
    if (port < 0 || port > 65535)
        return "SSH port must be between 1 and 65535, or inherited.";
    if (!validText(identityFile) || (!identityFile.isEmpty() && !identityFile.startsWith('/') &&
                                     !identityFile.startsWith("~/")))
        return "Identity file must be an absolute or ~/ path on the local computer.";
    return {};
}
QString HostProfile::connectionKey() const {
    auto target = ssh;
    auto options = sshOptions();
    const int at = target.lastIndexOf('@');
    if (at >= 0) {
        options["user"] = target.left(at);
        target = target.mid(at + 1);
    }
    return QString::fromUtf8(QJsonDocument(QJsonObject{{"host", target}, {"options", options}})
                                 .toJson(QJsonDocument::Compact));
}
QString HostProfile::discoveryScope() const {
    return id + (generation.isEmpty() ? QString() : QChar(0x1f) + generation);
}
QString HostProfile::label() const {
    if (isLocal())
        return "This Computer";
    return (user.isEmpty() ? QString() : user + '@') + ssh +
           (port ? QString(" · port %1").arg(port) : QString());
}
QMap<QString, HostProfile> readHosts(const QJsonArray &records, bool legacy) {
    QMap<QString, HostProfile> hosts;
    QSet<QString> addresses;
    for (const auto &value : records) {
        const auto record = value.toObject();
        if (!record["id"].isString() || !record["ssh"].isString())
            continue;
        HostProfile host{record["id"].toString(), record["ssh"].toString()};
        if (record.contains("generation") && !record["generation"].isString())
            continue;
        host.generation = record["generation"].toString();
        const auto options = record["sshOptions"].toObject();
        if (record.contains("sshOptions") && !record["sshOptions"].isObject())
            continue;
        bool validOptions = true;
        for (auto it = options.begin(); it != options.end(); ++it) {
            if (it.key() == "port") {
                validOptions &=
                    it.value().isDouble() && it.value().toDouble() == it.value().toInt(-1);
            } else
                validOptions &=
                    (it.key() == "user" || it.key() == "identityFile") && it.value().isString();
        }
        host.user = options["user"].toString();
        host.port = options["port"].toInt();
        host.identityFile = options["identityFile"].toString();
        if (!validOptions || !host.validate().isEmpty() || hosts.contains(host.id) ||
            addresses.contains(host.connectionKey()))
            continue;
        host.enabled = record["enabled"].toBool(true);
        host.servers = record["servers"].toBool(true);
        host.containers = record["containers"].toBool(true);
        if (host.isLocal()) {
            if (legacy)
                host.containers = host.enabled;
            host.enabled = host.servers = true;
        }
        addresses.insert(host.connectionKey());
        hosts.insert(host.id, host);
    }
    if (std::none_of(hosts.begin(), hosts.end(),
                     [](const HostProfile &h) { return h.isLocal(); })) {
        // Preserve the existing local identity used in container backend IDs.
        QString id = "local-mycodex";
        while (hosts.contains(id))
            id += "-local";
        hosts.insert(id, HostProfile{id, {}});
    }
    return hosts;
}
} // namespace Archcodex
