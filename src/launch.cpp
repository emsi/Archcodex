// SPDX-License-Identifier: GPL-3.0-or-later
#include "launch.h"
#include <QFile>
#include <QJsonDocument>
#include <QProcessEnvironment>
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
QStringList terminalCommand(const QStringList &command) {
    // KSandbox omits environment entries identical to Archcodex's environment.
    // Carry just these explicit overrides as literal argv entries across the host
    // boundary. Unset variables retain host inheritance; empty values stay empty.
    QStringList args{"/usr/bin/env", "--"};
    const auto environment = QProcessEnvironment::systemEnvironment();
    for (const auto *name : {"EDITOR", "VISUAL"})
        if (environment.contains(name))
            args << QString::fromLatin1(name) + '=' + environment.value(name);
    // Bash initializes the local host's interactive configuration, then replaces
    // itself with the client. A failed exec must not leave an interactive shell.
    return args + QStringList{"/bin/bash", "-ic", "exec \"$@\"; exit \"$?\"", "archcodex"} +
           command;
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
QString transportBootstrap() {
    QJsonObject sources;
    for (const auto &name : {"mux", "containers", "discover", "transport"})
        sources[name] = resourceScript(QString::fromLatin1(name) + ".py");
    // qCompress prepends a four-byte size; Python expects the zlib stream itself.
    const auto bundle = QString::fromLatin1(
        qCompress(QJsonDocument(sources).toJson(QJsonDocument::Compact), 9).mid(4).toBase64());
    return "import base64,json,sys,types,zlib\n"
           "S=json.loads(zlib.decompress(base64.b64decode('" +
           bundle +
           "')))\n"
           "for n in ('mux','containers','discover','transport'):\n"
           " m=types.ModuleType(n);m.__file__='<packaged-'+n+'>';sys.modules[n]=m;exec(S[n],m.__"
           "dict__)\n"
           "sys.modules['transport'].BUNDLE='" +
           bundle +
           "'\n"
           "sys.modules['transport'].main()\n";
}
QStringList transportCommand(const QString &role, const QString &argument) {
    QStringList args{"/usr/bin/python3", "-I", "-u", "-c", transportBootstrap(), role};
    if (!argument.isEmpty())
        args << argument;
    return args;
}
QStringList supervisedTransportCommand(const QString &instance) {
    // ExecStopPost also runs after SIGKILL, removing only this instance's sockets.
    auto quote = [](QString arg) {
        arg.replace('\\', "\\\\").replace('"', "\\\"").replace('%', "%%").replace('$', "$$");
        arg.replace('\n', "\\n");
        return '"' + arg + '"';
    };
    QStringList cleanup;
    for (const auto &arg : transportCommand("cleanup", instance))
        cleanup << quote(arg);
    QStringList args{"/usr/bin/systemd-run",
                     "--user",
                     "--quiet",
                     "--pipe",
                     "--collect",
                     "--service-type=exec",
                     "--unit=archcodex-transport-" + instance,
                     "--property=KillMode=control-group",
                     "--property=TimeoutStopSec=5s",
                     "--property=Restart=no",
                     "--property=UMask=0077",
                     "--property=ExecStopPost=" + cleanup.join(' '),
                     "--"};
    return hostCommand(args + transportCommand("desktop", instance));
}
} // namespace Archcodex
