// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "types.h"
namespace Archcodex {
QString shellQuote(const QString &value);
bool validSshHost(const QString &host);
QString resourceScript(const QString &name);
QStringList hostCommand(QStringList command, bool sandbox = true);
QStringList sshCommand(const QString &host, const QStringList &command, bool terminal = false);
QStringList containerCommand(const BackendProfile &profile, const QString &mode,
                             const QString &thread = {}, const QString &cwd = {});
QString transportBootstrap();
QStringList transportCommand(const QString &role, const QString &argument = {});
QStringList supervisedTransportCommand(const QString &instance);
} // namespace Archcodex
