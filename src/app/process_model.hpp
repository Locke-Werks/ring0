// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/snapshot.hpp"

#include <QAbstractListModel>
#include <QQmlEngine>
#include <QtQml/qqmlregistration.h>

#include <cstdint>
#include <vector>

// Flat process list fed by SnapshotBridge. Rows are diffed in place rather than
// reset so a view keeps its scroll position and selection across samples.
class ProcessModel : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Role {
        PidRole = Qt::UserRole + 1,
        PpidRole,
        NameRole,
        PathRole,
        CommandLineRole,
        SessionRole,
        ThreadsRole,
        HandlesRole,
        PriorityRole,
        SuspendedRole,
        IsSystemRole,
        CpuRole,
        MemoryRole,
        PrivateBytesRole,
        DiskRole,
        NetRole,
        GpuRole,
        CreateTimeRole,
    };
    Q_ENUM(Role)

    explicit ProcessModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const { return static_cast<int>(rows_.size()); }

    // -1 when the pid is not present.
    Q_INVOKABLE int rowForPid(int pid) const;

signals:
    void countChanged();

private:
    void refresh();

    std::vector<ring0::Process> rows_;
};
