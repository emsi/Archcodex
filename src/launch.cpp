// SPDX-License-Identifier: GPL-3.0-or-later
#include "launch.h"
#include <QFile>
#include <QJsonDocument>
#include <QRegularExpression>

static void initResources() {
    Q_INIT_RESOURCE(discovery);
}
namespace Archcodex {
QString shellQuote(const QString &value) {
    auto s = value;
    return "'" + s.replace("'", "'\\''") + "'";
}
bool validSshHost(const QString &host) {
    // SSH aliases and user@host, including IPv6. No options, URLs or shell syntax.
    static const QRegularExpression pattern("^[A-Za-z0-9_][A-Za-z0-9_.@:%+\\[\\]-]{0,254}$");
    return pattern.match(host).hasMatch();
}
QString resourceScript(const QString &name) {
    initResources();
    QFile f(":/archcodex/" + name);
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
}
QStringList hostCommand(QStringList command, bool sandbox) {
    if (sandbox && QFile::exists("/.flatpak-info"))
        command = QStringList{"/usr/bin/flatpak-spawn", "--watch-bus", "--host"} + command;
    return command;
}
QStringList sshCommand(const QString &host, const QStringList &command, bool terminal) {
    if (host.isEmpty())
        return command;
    QStringList quoted;
    for (const auto &arg : command)
        quoted << shellQuote(arg);
    // Background discovery never prompts or weakens host-key verification.
    // Interactive authentication remains available in the terminal client.
    QStringList args{"ssh", terminal ? "-tt" : "-T", "-o", "ConnectTimeout=10"};
    if (!terminal)
        args << "-o" << "BatchMode=yes";
    args << "--" << host << "exec " + quoted.join(' ');
    return args;
}
QStringList containerCommand(const BackendProfile &p, const QString &mode, const QString &thread,
                             const QString &cwd) {
    auto data = p.json();
    data["thread"] = thread;
    data["cwd"] = cwd;
    const auto encoded = QJsonDocument(data).toJson(QJsonDocument::Compact).toBase64();
    return sshCommand(p.sshHost,
                      {"/usr/bin/python3", "-I", "-u", "-c", resourceScript("containers.py"), mode,
                       QString::fromLatin1(encoded)},
                      mode == "tui");
}
} // namespace Archcodex
