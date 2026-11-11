// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <KParts/ReadOnlyPart>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <kde_terminal_interface.h>

// Runs as the real Konsole PTY child, without Codex, credentials or a server.
// Record both SIGWINCH and explicit input acknowledgements: final widget sizes
// alone cannot detect a resize that was immediately undone.
class PtyProbe {
    QTemporaryDir directory;

  public:
    QString executable() const {
        return directory.filePath("client");
    }
    bool install() {
        QFile file(executable());
        if (!file.open(QIODevice::WriteOnly))
            return false;
        file.write(R"PY(#!/usr/bin/python3
import json, os, pathlib, signal, sys, tty
tty.setraw(0)
path = pathlib.Path(__file__).parent / (sys.argv[-1] + '.jsonl')
with path.open('wb', buffering=0) as log:
    def record(event):
        size = os.get_terminal_size(0)
        log.write((json.dumps(dict(event=event, rows=size.lines, columns=size.columns)) + '\n').encode())
    signal.signal(signal.SIGWINCH, lambda *_: record('resize'))
    record('start')
    os.write(1, b'PTY PROBE READY\r\n')
    while True:
        command = os.read(0, 1)
        if not command or command == b'x': break
        record('checkpoint')
)PY");
        file.close();
        return file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    }
    QJsonArray records(const QString &id) const {
        QFile file(directory.filePath(id + ".jsonl"));
        if (!file.open(QIODevice::ReadOnly))
            return {};
        QJsonArray result;
        for (const auto &line : file.readAll().split('\n')) {
            const auto record = QJsonDocument::fromJson(line);
            if (record.isObject())
                result.append(record.object());
        }
        return result;
    }
    static QSize size(const QJsonValue &record) {
        const auto value = record.toObject();
        return {value["columns"].toInt(), value["rows"].toInt()};
    }
    static void send(KParts::ReadOnlyPart *part, const QString &text = "s") {
        qobject_cast<TerminalInterface *>(part)->sendInput(text);
    }
};
