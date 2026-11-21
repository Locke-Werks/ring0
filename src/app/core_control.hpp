// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/evacuate.hpp"

#include <QObject>
#include <QQmlEngine>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

// QML front for soft-disabling cores. While any core is evacuated a one-second
// sweep catches newly started processes; everything is restored when Ring0 exits.
class CoreControl : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(QVariantList evacuated READ evacuated NOTIFY changed)
    Q_PROPERTY(int moved READ moved NOTIFY changed)
    Q_PROPERTY(int refused READ refused NOTIFY changed)

public:
    explicit CoreControl(QObject* parent = nullptr);

    QVariantList evacuated() const;
    int moved() const { return stats_.moved; }
    int refused() const { return stats_.refused; }

    // Adds or removes cores from the evacuated set. Returns false, and emits
    // `failed`, when the result would leave no core for ordinary work.
    Q_INVOKABLE bool evacuate(const QVariantList& cores);
    Q_INVOKABLE bool restore(const QVariantList& cores);
    Q_INVOKABLE void restoreAll();

signals:
    void changed();
    void failed(const QString& message);

private:
    bool commit(std::vector<bool> next);
    void refresh();

    ring0::CoreEvacuator evac_;
    ring0::CoreEvacuator::Stats stats_;
};
