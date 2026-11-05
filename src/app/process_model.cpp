// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/process_model.hpp"
#include "app/snapshot_bridge.hpp"

#include <QString>

#include <unordered_map>

using namespace ring0;

namespace {

// A pid alone is not an identity: Windows recycles pids quickly, and a reused
// pid must read as an exit plus an arrival, not as the old row mutating.
struct Key {
    std::uint32_t pid;
    std::uint64_t create_time;
    bool operator==(const Key&) const = default;
};

struct KeyHash {
    std::size_t operator()(const Key& k) const noexcept
    {
        return std::hash<std::uint64_t>{}(k.create_time * 0x9E3779B97F4A7C15ull ^ k.pid);
    }
};

Key key_of(const Process& p) { return {p.pid, p.create_time}; }

QString qs(const std::wstring& w)
{
    return QString::fromWCharArray(w.data(), static_cast<qsizetype>(w.size()));
}

} // namespace

ProcessModel::ProcessModel(QObject* parent) : QAbstractListModel(parent)
{
    connect(SnapshotBridge::instance(), &SnapshotBridge::changed, this, &ProcessModel::refresh);
    refresh();
}

int ProcessModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : count();
}

QVariant ProcessModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= count()) return {};
    const auto& p = rows_[std::size_t(index.row())];
    switch (role) {
    case Qt::DisplayRole:
    case NameRole:         return qs(p.image_name);
    case PidRole:          return int(p.pid);
    case PpidRole:         return int(p.ppid);
    case PathRole:         return qs(p.image_path);
    case CommandLineRole:  return qs(p.command_line);
    case SessionRole:      return int(p.session_id);
    case ThreadsRole:      return int(p.thread_count);
    case HandlesRole:      return int(p.handle_count);
    case PriorityRole:     return int(p.base_priority);
    case SuspendedRole:    return p.suspended;
    case IsSystemRole:     return p.is_system;
    case CpuRole:          return double(p.cpu_pct);
    case MemoryRole:       return double(p.working_set);
    case PrivateBytesRole: return double(p.private_bytes);
    case DiskRole:         return double(p.disk_bps);
    case NetRole:          return double(p.net_bps);
    case GpuRole:          return double(p.gpu_pct);
    case CreateTimeRole:   return QVariant::fromValue<quint64>(p.create_time);
    default:               return {};
    }
}

QHash<int, QByteArray> ProcessModel::roleNames() const
{
    return {
        {PidRole, "pid"},
        {PpidRole, "ppid"},
        {NameRole, "name"},
        {PathRole, "path"},
        {CommandLineRole, "commandLine"},
        {SessionRole, "session"},
        {ThreadsRole, "threads"},
        {HandlesRole, "handles"},
        {PriorityRole, "priority"},
        {SuspendedRole, "suspended"},
        {IsSystemRole, "isSystem"},
        {CpuRole, "cpu"},
        {MemoryRole, "memory"},
        {PrivateBytesRole, "privateBytes"},
        {DiskRole, "disk"},
        {NetRole, "net"},
        {GpuRole, "gpu"},
        {CreateTimeRole, "createTime"},
    };
}

int ProcessModel::rowForPid(int pid) const
{
    for (std::size_t i = 0; i < rows_.size(); ++i)
        if (int(rows_[i].pid) == pid) return int(i);
    return -1;
}

void ProcessModel::refresh()
{
    const auto snap = SnapshotBridge::instance()->current();
    if (!snap) return;
    const auto& incoming = snap->processes;
    const int before = count();

    std::unordered_map<Key, std::size_t, KeyHash> next;
    next.reserve(incoming.size());
    for (std::size_t i = 0; i < incoming.size(); ++i) next.emplace(key_of(incoming[i]), i);

    // Walk backwards so earlier indices stay valid while removing, and coalesce
    // adjacent exits into one remove so a large exit burst is a few signals.
    for (int i = count() - 1; i >= 0;) {
        if (next.contains(key_of(rows_[std::size_t(i)]))) { --i; continue; }
        int first = i;
        while (first > 0 && !next.contains(key_of(rows_[std::size_t(first - 1)]))) --first;
        beginRemoveRows({}, first, i);
        rows_.erase(rows_.begin() + first, rows_.begin() + i + 1);
        endRemoveRows();
        i = first - 1;
    }

    std::vector<bool> seen(incoming.size(), false);
    for (auto& row : rows_) {
        const auto idx = next.at(key_of(row));
        row = incoming[idx];
        seen[idx] = true;
    }
    if (!rows_.empty())
        emit dataChanged(index(0), index(count() - 1));

    std::vector<std::size_t> arrivals;
    for (std::size_t i = 0; i < incoming.size(); ++i)
        if (!seen[i]) arrivals.push_back(i);
    if (!arrivals.empty()) {
        const int first = count();
        beginInsertRows({}, first, first + int(arrivals.size()) - 1);
        for (auto i : arrivals) rows_.push_back(incoming[i]);
        endInsertRows();
    }

    if (count() != before) emit countChanged();
}
