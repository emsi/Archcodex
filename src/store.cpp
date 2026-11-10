// SPDX-License-Identifier: GPL-3.0-or-later
#include "store.h"
#include <QFile>
#include <QJsonDocument>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <stdexcept>

namespace Archcodex {
static void run(QSqlQuery &q) {
    if (!q.exec())
        throw std::runtime_error(q.lastError().text().toStdString());
}
Store::Store(const QString &path) {
    db = QSqlDatabase::addDatabase("QSQLITE", QUuid::createUuid().toString());
    db.setDatabaseName(path);
    if (!db.open())
        throw std::runtime_error(db.lastError().text().toStdString());
    QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner);
    QSqlQuery q(db);
    q.prepare("PRAGMA user_version");
    run(q);
    q.next();
    if (q.value(0).toInt() > 2)
        throw std::runtime_error("The database belongs to a newer Archcodex version.");
    q.prepare("PRAGMA foreign_keys=ON");
    run(q);
    q.prepare("PRAGMA busy_timeout=3000");
    run(q);
    const QStringList statements = {
        "CREATE TABLE IF NOT EXISTS backends (id TEXT PRIMARY KEY, data TEXT NOT NULL)",
        "CREATE TABLE IF NOT EXISTS projects (backend TEXT REFERENCES backends(id) ON DELETE "
        "CASCADE, path TEXT, name TEXT, PRIMARY KEY(backend,path))",
        "CREATE TABLE IF NOT EXISTS sessions (backend TEXT REFERENCES backends(id) ON DELETE "
        "CASCADE, id TEXT, data TEXT, opened INTEGER NOT NULL, PRIMARY KEY(backend,id))",
        "CREATE TABLE IF NOT EXISTS workspace (id INTEGER PRIMARY KEY CHECK(id=1), "
        "revision INTEGER NOT NULL, data TEXT NOT NULL)",
        "PRAGMA user_version=2"};
    if (!db.transaction())
        throw std::runtime_error("Could not begin database migration.");
    try {
        for (const auto &s : statements) {
            q.prepare(s);
            run(q);
        }
    } catch (...) {
        db.rollback();
        throw;
    }
    if (!db.commit())
        throw std::runtime_error("Could not commit database migration.");
}
Store::~Store() {
    const auto name = db.connectionName();
    db.close();
    db = QSqlDatabase();
    QSqlDatabase::removeDatabase(name);
}
QList<BackendProfile> Store::backends() const {
    QList<BackendProfile> out;
    QSqlQuery q(db);
    q.prepare("SELECT data FROM backends ORDER BY rowid");
    run(q);
    while (q.next()) {
        auto p =
            BackendProfile::fromJson(QJsonDocument::fromJson(q.value(0).toByteArray()).object());
        if (!p.validate().isEmpty())
            throw std::runtime_error("Invalid saved backend configuration.");
        out.append(p);
    }
    return out;
}
QList<Project> Store::projects() const {
    QList<Project> out;
    QSqlQuery q(db);
    q.prepare("SELECT backend,path,name FROM projects");
    run(q);
    while (q.next())
        out.append({q.value(0).toString(), q.value(1).toString(), q.value(2).toString()});
    return out;
}
QList<Session> Store::sessions() const {
    QList<Session> out;
    QSqlQuery q(db);
    q.prepare("SELECT backend,data,opened FROM sessions");
    run(q);
    while (q.next()) {
        auto s = Session::fromThread(q.value(0).toString(),
                                     QJsonDocument::fromJson(q.value(1).toByteArray()).object());
        s.known = false;
        s.opened = q.value(2).toLongLong();
        out.append(s);
    }
    return out;
}
void Store::save(const BackendProfile &p) {
    QSqlQuery q(db);
    q.prepare("INSERT INTO backends VALUES(?,?) ON CONFLICT(id) DO UPDATE SET data=excluded.data");
    q.addBindValue(p.id);
    q.addBindValue(QJsonDocument(p.json()).toJson(QJsonDocument::Compact));
    run(q);
}
void Store::save(const Project &p) {
    QSqlQuery q(db);
    q.prepare("INSERT INTO projects VALUES(?,?,?) ON CONFLICT(backend,path) DO UPDATE SET "
              "name=excluded.name");
    q.addBindValue(p.backendId);
    q.addBindValue(p.path);
    q.addBindValue(p.name);
    run(q);
}
void Store::save(const Session &s) {
    QSqlQuery q(db);
    q.prepare("INSERT INTO sessions VALUES(?,?,?,?) ON CONFLICT(backend,id) DO UPDATE SET "
              "data=excluded.data,opened=excluded.opened");
    q.addBindValue(s.backendId);
    q.addBindValue(s.id);
    q.addBindValue(QJsonDocument(s.metadata()).toJson(QJsonDocument::Compact));
    q.addBindValue(s.opened);
    run(q);
}
void Store::removeBackend(const QString &id) {
    QSqlQuery q(db);
    q.prepare("DELETE FROM backends WHERE id=?");
    q.addBindValue(id);
    run(q);
}
void Store::removeSession(const QString &backend, const QString &id) {
    QSqlQuery q(db);
    q.prepare("DELETE FROM sessions WHERE backend=? AND id=?");
    q.addBindValue(backend);
    q.addBindValue(id);
    run(q);
}
WorkspaceSnapshot Store::workspace() const {
    try {
        QSqlQuery q(db);
        q.prepare("SELECT revision,data FROM workspace WHERE id=1");
        run(q);
        if (!q.next())
            return {};
        const auto bytes = q.value(1).toByteArray();
        if (bytes.size() > 4 * 1024 * 1024)
            throw std::runtime_error("Terminal workspace exceeds its size limit.");
        QJsonParseError error;
        const auto doc = QJsonDocument::fromJson(bytes, &error);
        if (error.error != QJsonParseError::NoError || !doc.isObject())
            throw std::runtime_error("Cannot read the saved terminal workspace.");
        auto snapshot = WorkspaceSnapshot::fromJson(doc.object());
        if (snapshot.revision != q.value(0).toLongLong())
            throw std::runtime_error("Terminal workspace revision is inconsistent.");
        return snapshot;
    } catch (...) {
        workspaceReadable = false;
        throw;
    }
}
void Store::save(const WorkspaceSnapshot &snapshot) {
    if (!workspaceReadable)
        throw std::runtime_error("The unreadable terminal workspace has been preserved. "
                                 "Workspace saving is disabled for this run.");
    const auto error = snapshot.validate();
    if (!error.isEmpty())
        throw std::runtime_error(error.toStdString());
    // Validate existing data before the first write too, including direct callers.
    const auto current = workspace();
    if (snapshot.revision <= current.revision)
        return;
    const auto bytes = QJsonDocument(snapshot.json()).toJson(QJsonDocument::Compact);
    if (bytes.size() > 4 * 1024 * 1024)
        throw std::runtime_error("Terminal workspace exceeds its size limit.");
    QSqlQuery q(db);
    // One SQLite statement atomically replaces the complete intended workspace.
    q.prepare("INSERT INTO workspace VALUES(1,?,?) ON CONFLICT(id) DO UPDATE SET "
              "revision=excluded.revision,data=excluded.data "
              "WHERE excluded.revision>workspace.revision");
    q.addBindValue(snapshot.revision);
    q.addBindValue(bytes);
    run(q);
}
} // namespace Archcodex
