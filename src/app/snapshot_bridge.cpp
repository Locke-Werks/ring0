// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/snapshot_bridge.hpp"

#include <QTimer>

#include <algorithm>

using namespace ring0;
using namespace Qt::StringLiterals;

namespace {

double metric_of(const Snapshot& s, const QString& metric)
{
    if (metric == u"cpu") return s.cpu_total;
    if (metric == u"gpu") return s.gpu_total;
    if (metric == u"disk") return s.disk_bps_total;
    if (metric == u"net") return s.net_bps_total;
    if (metric == u"mem") {
        const auto& m = s.memory;
        return m.phys_total ? 100.0 * double(m.phys_total - m.phys_avail) / double(m.phys_total) : 0.0;
    }
    if (metric.startsWith(u"core:")) {
        const auto i = metric.sliced(5).toInt();
        return i >= 0 && std::size_t(i) < s.cores.size() ? s.cores[std::size_t(i)].total : 0.0;
    }
    return 0.0;
}

} // namespace

SnapshotBridge* SnapshotBridge::instance()
{
    static auto* self = new SnapshotBridge;
    return self;
}

SnapshotBridge::SnapshotBridge()
{
    sampler_ = std::make_unique<SamplerThread>(history_, 1000);
    auto* t = new QTimer(this);
    t->setTimerType(Qt::PreciseTimer);
    connect(t, &QTimer::timeout, this, &SnapshotBridge::poll);
    t->start(16);
}

void SnapshotBridge::poll()
{
    const auto s = history_.latest();
    if (!s || s->seq == seq_) return;
    seq_ = s->seq;
    // Keep a held scrub position pinned to the same moment as new samples arrive.
    if (scrub_back_ > 0) {
        scrub_back_ = std::min<int>(scrub_back_ + 1, int(history_.size()) - 1);
        emit scrubChanged();
    }
    emit changed();
}

SnapshotPtr SnapshotBridge::current() const
{
    return scrub_back_ > 0 ? history_.at(std::size_t(scrub_back_)) : history_.latest();
}

void SnapshotBridge::setScrubBack(int back)
{
    back = std::clamp(back, 0, std::max(0, int(history_.size()) - 1));
    if (back == scrub_back_) return;
    scrub_back_ = back;
    emit scrubChanged();
    emit changed();
}

double SnapshotBridge::cpuTotal() const { auto s = current(); return s ? s->cpu_total : 0; }
double SnapshotBridge::gpuTotal() const { auto s = current(); return s ? s->gpu_total : 0; }
double SnapshotBridge::diskBps() const { auto s = current(); return s ? s->disk_bps_total : 0; }
double SnapshotBridge::netBps() const { auto s = current(); return s ? s->net_bps_total : 0; }
double SnapshotBridge::memPct() const { auto s = current(); return s ? metric_of(*s, u"mem"_s) : 0; }
int SnapshotBridge::coreCount() const { auto s = current(); return s ? int(s->cores.size()) : 0; }
int SnapshotBridge::processCount() const { auto s = current(); return s ? int(s->processes.size()) : 0; }

double SnapshotBridge::coreLoad(int core) const
{
    auto s = current();
    if (!s || core < 0 || std::size_t(core) >= s->cores.size()) return 0;
    return s->cores[std::size_t(core)].total;
}

QList<double> SnapshotBridge::series(const QString& metric, int n) const
{
    QList<double> out;
    if (n <= 0) return out;
    const auto snaps = history_.tail(std::size_t(n) + std::size_t(scrub_back_));
    const auto end = snaps.size() - std::min(snaps.size(), std::size_t(scrub_back_));
    out.reserve(qsizetype(end));
    for (std::size_t i = 0; i < end; ++i)
        out.push_back(metric_of(*snaps[i], metric));
    return out;
}
