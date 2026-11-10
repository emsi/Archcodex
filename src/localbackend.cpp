// SPDX-License-Identifier: GPL-3.0-or-later
#include "backend.h"
#include "launch.h"
#include <QFile>

namespace Archcodex {
QStringList LocalBackend::bridgeArguments() const {
    if (profile.isContainer())
        return QStringList{"--exec"} + hostCommand(transportCommand("proxy", profile.relaySocket));
    QStringList args = {"--codex", profile.executable};
    if (!profile.socketPath.isEmpty())
        args << "--socket" << profile.socketPath;
    if (QFile::exists(QStringLiteral("/.flatpak-info")))
        args << "--host-proxy";
    return args;
}
} // namespace Archcodex
