// SPDX-License-Identifier: GPL-3.0-or-later
#include "asyncstore.h"
#include <QMutex>
#include <QMutexLocker>
#include <list>

namespace Archcodex {
class StoreWriter : public QObject {
    Q_OBJECT
  public:
    using Operation = std::function<void(Store &)>;
    struct Entry {
        QString key;
        Operation operation;
        std::function<void(const QString &)> completed;
    };
    std::unique_ptr<Store> store;
    QMutex mutex;
    std::list<Entry> queue;
    QHash<QString, std::list<Entry>::iterator> saves;
    bool scheduled = false;
    void initialize(const QString &path) {
        try {
            store = std::make_unique<Store>(path);
            StoreSnapshot snapshot;
            snapshot.profiles = store->backends();
            snapshot.projects = store->projects();
            snapshot.sessions = store->sessions();
            snapshot.readStates = store->readStates();
            try {
                snapshot.workspace = store->workspace();
            } catch (const std::exception &e) {
                snapshot.workspaceError = QString::fromUtf8(e.what());
            }
            emit loaded(snapshot);
        } catch (const std::exception &e) {
            store.reset(); // Do not overwrite a cache that could not be read safely.
            emit failed(QString::fromUtf8(e.what()));
            StoreSnapshot snapshot;
            snapshot.workspaceError = QString::fromUtf8(e.what());
            emit loaded(snapshot);
        }
    }
    void enqueue(const QString &key, Operation operation,
                 std::function<void(const QString &)> completed = {}) {
        QMutexLocker lock(&mutex);
        if (!key.isEmpty() && saves.contains(key)) {
            saves[key]->operation = std::move(operation);
            saves[key]->completed = std::move(completed);
            return;
        }
        if (queue.size() >= 65536) {
            lock.unlock();
            const QString error =
                "Metadata queue exceeded its limit; some cache updates were not saved.";
            if (completed)
                completed(error);
            else
                emit failed(error);
            return;
        }
        if (key.isEmpty())
            saves.clear(); // Removal is an ordering barrier.
        queue.push_back({key, std::move(operation), std::move(completed)});
        if (!key.isEmpty())
            saves[key] = std::prev(queue.end());
        if (!scheduled) {
            scheduled = true;
            QMetaObject::invokeMethod(this, [this] { drain(); }, Qt::QueuedConnection);
        }
    }
    void drain() {
        std::list<Entry> batch;
        {
            QMutexLocker lock(&mutex);
            while (!queue.empty() && batch.size() < 128) {
                auto it = queue.begin();
                if (saves.contains(it->key) && saves[it->key] == it)
                    saves.remove(it->key);
                batch.splice(batch.end(), queue, it);
            }
        }
        for (auto &entry : batch) {
            // Initialization already reports an unreadable store. Operations with
            // acknowledgements must still report that they could not commit.
            if (!store && !entry.completed)
                continue;
            QString error;
            try {
                if (!store)
                    throw std::runtime_error("The metadata store is unavailable.");
                entry.operation(*store);
            } catch (const std::exception &e) {
                error = QString::fromUtf8(e.what());
            }
            if (entry.completed)
                entry.completed(error);
            else if (!error.isEmpty()) {
                if (entry.key == "workspace")
                    emit workspaceFailed(error);
                else
                    emit failed(error);
            }
        }
        QMutexLocker lock(&mutex);
        if (queue.empty())
            scheduled = false;
        else
            QMetaObject::invokeMethod(this, [this] { drain(); }, Qt::QueuedConnection);
    }
    void finish() {
        // Flush already accepted cache writes on the DB worker, never on the GUI.
        for (;;) {
            {
                QMutexLocker lock(&mutex);
                if (queue.empty())
                    break;
            }
            drain();
        }
        deleteLater();
    }
  signals:
    void loaded(const Archcodex::StoreSnapshot &snapshot);
    void failed(const QString &error);
    void workspaceFailed(const QString &error);
    void workspaceSaved(const Archcodex::WorkspaceSnapshot &snapshot);
    void backendRetired(const QString &id, quint64 token, const QString &error);
};
AsyncStore::AsyncStore(const QString &path, QObject *parent)
    : QObject(parent), worker(std::make_shared<Worker>()), writer(new StoreWriter) {
    qRegisterMetaType<StoreSnapshot>();
    writer->moveToThread(worker->context->thread());
    connect(writer, &StoreWriter::loaded, this, &AsyncStore::loaded);
    connect(writer, &StoreWriter::failed, this, &AsyncStore::failed);
    connect(writer, &StoreWriter::workspaceFailed, this, &AsyncStore::workspaceFailed);
    connect(writer, &StoreWriter::workspaceSaved, this, &AsyncStore::workspaceSaved);
    connect(writer, &StoreWriter::backendRetired, this, &AsyncStore::backendRetired,
            Qt::QueuedConnection);
    worker->post([w = writer, path] { w->initialize(path); });
}
AsyncStore::~AsyncStore() {
    worker->post([w = writer] { w->finish(); });
}
void AsyncStore::save(const BackendProfile &p) {
    if (retired.contains(p.id))
        return;
    writer->enqueue("b:" + p.id, [p](Store &s) { s.save(p); });
}
void AsyncStore::save(const Session &s) {
    if (retired.contains(s.backendId))
        return;
    writer->enqueue("s:" + sessionKey(s.backendId, s.id), [s](Store &store) { store.save(s); });
}
void AsyncStore::save(const SessionReadState &s) {
    if (!retired.contains(s.backendId))
        writer->enqueue("read:" + sessionKey(s.backendId, s.id),
                        [s](Store &store) { store.save(s); });
}
void AsyncStore::save(const WorkspaceSnapshot &snapshot) {
    writer->enqueue(
        "workspace", [snapshot](Store &store) { store.save(snapshot); },
        [w = writer, snapshot](const QString &error) {
            if (error.isEmpty())
                emit w->workspaceSaved(snapshot);
            else
                emit w->workspaceFailed(error);
        });
}
void AsyncStore::retireBackend(const QString &id, quint64 token) {
    retired.insert(id);
    writer->enqueue(
        {}, [id](Store &store) { store.removeBackend(id); },
        [w = writer, id, token](const QString &error) {
            emit w->backendRetired(id, token, error);
        });
}
void AsyncStore::allowBackend(const QString &id) {
    retired.remove(id);
}
void AsyncStore::removeBackend(const QString &id) {
    writer->enqueue({}, [id](Store &s) { s.removeBackend(id); });
}
void AsyncStore::removeSession(const QString &backend, const QString &id) {
    writer->enqueue({}, [backend, id](Store &s) { s.removeSession(backend, id); });
}
} // namespace Archcodex
#include "asyncstore.moc"
