// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QHash>
#include <QTemporaryDir>

// Restore unset and explicitly empty values separately, including on test failure.
class TestEnvironment {
    QHash<QByteArray, QByteArray> previous;

  public:
    void set(const QByteArray &name, const QByteArray &value = {}) {
        if (!previous.contains(name))
            previous.insert(name, qgetenv(name));
        if (value.isNull())
            qunsetenv(name);
        else
            qputenv(name, value);
    }
    ~TestEnvironment() {
        for (auto it = previous.cbegin(); it != previous.cend(); ++it)
            if (it.value().isNull())
                qunsetenv(it.key());
            else
                qputenv(it.key(), it.value());
    }
};

// Client tests must not execute the developer's interactive shell configuration.
class TestHome {
    TestEnvironment environment;

  public:
    QTemporaryDir directory;
    TestHome() {
        Q_ASSERT(directory.isValid());
        environment.set("HOME", directory.path().toUtf8());
        environment.set("EDITOR");
        environment.set("VISUAL");
    }
};
