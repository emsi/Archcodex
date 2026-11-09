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
    };
    std::unique_ptr<Store> store;
    QMutex mutex;
    std::list<Entry> queue;
    QHash<QString, std::list<Entry>::iterator> saves;
    bool scheduled = false;
    void initialize(const QString &path) {
        try {
            store = std::make_unique<Store>(path);
            emit loaded({store->backends(), store->projects(), store->sessions()});
        } catch (const std::exception &e) {
            store.reset(); // Do not overwrite a cache that could not be read safely.
            emit failed(QString::fromUtf8(e.what()));
            emit loaded({});
        }
    }
    void enqueue(const QString &key, Operation operation) {
        QMutexLocker lock(&mutex);
        if (!key.isEmpty() && saves.contains(key)) {
            saves[key]->operation = std::move(operation);
            return;
        }
        if (queue.size() >= 65536) {
            lock.unlock();
            emit failed("Metadata queue exceeded its limit; some cache updates were not saved.");
            return;
        }
        if (key.isEmpty())
            saves.clear(); // Removal is an ordering barrier.
        queue.push_back({key, std::move(operation)});
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
        if (store) {
            try {
                for (auto &entry : batch)
                    entry.operation(*store);
            } catch (const std::exception &e) {
                emit failed(QString::fromUtf8(e.what()));
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
};
AsyncStore::AsyncStore(const QString &path, QObject *parent)
    : QObject(parent), worker(std::make_shared<Worker>()), writer(new StoreWriter) {
    qRegisterMetaType<StoreSnapshot>();
    writer->moveToThread(worker->context->thread());
    connect(writer, &StoreWriter::loaded, this, &AsyncStore::loaded);
    connect(writer, &StoreWriter::failed, this, &AsyncStore::failed);
    worker->post([w = writer, path] { w->initialize(path); });
}
AsyncStore::~AsyncStore() {
    worker->post([w = writer] { w->finish(); });
}
void AsyncStore::save(const BackendProfile &p) {
    writer->enqueue("b:" + p.id, [p](Store &s) { s.save(p); });
}
void AsyncStore::save(const Session &s) {
    writer->enqueue("s:" + sessionKey(s.backendId, s.id), [s](Store &store) { store.save(s); });
}
void AsyncStore::removeBackend(const QString &id) {
    writer->enqueue({}, [id](Store &s) { s.removeBackend(id); });
}
void AsyncStore::removeSession(const QString &backend, const QString &id) {
    writer->enqueue({}, [backend, id](Store &s) { s.removeSession(backend, id); });
}
} // namespace Archcodex
#include "asyncstore.moc"
