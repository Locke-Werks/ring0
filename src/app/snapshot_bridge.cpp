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
    if (metric == u"cpuk") {
        if (s.cores.empty()) return 0.0;
        double k = 0;
        for (const auto& c : s.cores) k += c.kernel;
        return k / double(s.cores.size());
    }
    if (metric == u"commit") {
        const auto& m = s.memory;
        return m.commit_limit ? 100.0 * double(m.commit_used) / double(m.commit_limit) : 0.0;
    }

    // Indexed metrics: "core:N", "disk:N:r|w", "net:N:rx|tx", "gpu:A", "gpu:A:E".
    const auto parts = metric.split(u':');
    if (parts.size() < 2) return 0.0;
    const auto at = [](const auto& v, qsizetype i) { return i >= 0 && std::size_t(i) < v.size() ? &v[std::size_t(i)] : nullptr; };
    const auto i = parts[1].toInt();
    if (parts[0] == u"core") {
        auto* c = at(s.cores, i);
        return c ? c->total : 0.0;
    }
    if (parts[0] == u"corek") {
        auto* c = at(s.cores, i);
        return c ? c->kernel : 0.0;
    }
    if (parts[0] == u"disk") {
        auto* d = at(s.disks, i);
        if (!d) return 0.0;
        if (parts.size() > 2) return parts[2] == u"r" ? d->read_bps : d->write_bps;
        return d->read_bps + d->write_bps;
    }
    if (parts[0] == u"net") {
        auto* n = at(s.ifaces, i);
        if (!n) return 0.0;
        if (parts.size() > 2) return parts[2] == u"rx" ? n->rx_bps : n->tx_bps;
        return n->rx_bps + n->tx_bps;
    }
    if (parts[0] == u"gpu") {
        auto* g = at(s.gpus, i);
        if (!g) return 0.0;
        if (parts.size() > 2) {
            auto* e = at(g->engines, parts[2].toInt());
            return e ? e->pct : 0.0;
        }
        return g->total_pct;
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
double SnapshotBridge::maxProcessMemory() const
{
    auto s = current();
    double m = 0;
    if (s)
        for (const auto& p : s->processes) m = std::max(m, double(p.working_set));
    return m;
}

double SnapshotBridge::maxProcessCpu() const
{
    auto s = current();
    double m = 0;
    if (s)
        for (const auto& p : s->processes)
            if (p.pid != 0) m = std::max(m, double(p.cpu_pct));
    return m;
}

double SnapshotBridge::cpuKernel() const { auto s = current(); return s ? metric_of(*s, u"cpuk"_s) : 0; }

double SnapshotBridge::threadCount() const
{
    auto s = current();
    double n = 0;
    if (s) for (const auto& p : s->processes) n += p.thread_count;
    return n;
}

double SnapshotBridge::handleCount() const
{
    auto s = current();
    double n = 0;
    if (s) for (const auto& p : s->processes) n += p.handle_count;
    return n;
}

double SnapshotBridge::uptimeSeconds() const { auto s = current(); return s ? double(s->tick_ms) / 1000.0 : 0; }

QList<double> SnapshotBridge::ccdTemps() const
{
    QList<double> out;
    if (auto s = current()) for (float t : s->ccd_temps_c) out.push_back(t);
    return out;
}

double SnapshotBridge::cpuPowerW() const { auto s = current(); return s ? s->cpu_power_w : -1; }

double SnapshotBridge::avgClockMhz() const
{
    auto s = current();
    if (!s) return -1;
    double sum = 0;
    int n = 0;
    for (float m : s->core_mhz) if (m > 0) { sum += m; ++n; }
    return n ? sum / n : -1;
}

double SnapshotBridge::coreClockMhz(int core) const
{
    auto s = current();
    if (!s || core < 0 || std::size_t(core) >= s->core_mhz.size()) return -1;
    return s->core_mhz[std::size_t(core)];
}

double SnapshotBridge::cpuTempC() const { auto s = current(); return s ? s->cpu_temp_c : -1; }

double SnapshotBridge::coreLoad(int core) const
{
    auto s = current();
    if (!s || core < 0 || std::size_t(core) >= s->cores.size()) return 0;
    return s->cores[std::size_t(core)].total;
}

QVariantMap SnapshotBridge::memory() const
{
    QVariantMap m;
    auto s = current();
    if (!s) return m;
    const auto& v = s->memory;
    m[u"total"_s] = double(v.phys_total);
    m[u"used"_s] = double(v.phys_total - v.phys_avail);
    m[u"available"_s] = double(v.phys_avail);
    m[u"cached"_s] = double(v.cached);
    m[u"commitUsed"_s] = double(v.commit_used);
    m[u"commitLimit"_s] = double(v.commit_limit);
    m[u"paged"_s] = double(v.paged_pool);
    m[u"nonpaged"_s] = double(v.nonpaged_pool);
    m[u"faults"_s] = double(v.page_faults_per_sec);
    return m;
}

QVariantList SnapshotBridge::disks() const
{
    QVariantList out;
    auto s = current();
    if (!s) return out;
    for (const auto& d : s->disks)
        out.push_back(QVariantMap{
            {u"name"_s, QString::fromStdWString(d.name)},
            {u"model"_s, QString::fromStdWString(d.model)},
            {u"read"_s, double(d.read_bps)},
            {u"write"_s, double(d.write_bps)},
            {u"active"_s, double(d.active_pct)},
            {u"temp"_s, double(d.temp_c)},
            {u"volumes"_s, QString::fromStdWString(d.volumes)},
            {u"capacity"_s, double(d.capacity)},
            {u"free"_s, double(d.free_bytes)}});
    return out;
}

QVariantList SnapshotBridge::ifaces() const
{
    QVariantList out;
    auto s = current();
    if (!s) return out;
    for (const auto& n : s->ifaces)
        out.push_back(QVariantMap{
            {u"name"_s, QString::fromStdWString(n.name)},
            {u"link"_s, double(n.link_bps)},
            {u"rx"_s, double(n.rx_bps)},
            {u"tx"_s, double(n.tx_bps)}});
    return out;
}

QVariantList SnapshotBridge::gpus() const
{
    QVariantList out;
    auto s = current();
    if (!s) return out;
    for (const auto& g : s->gpus) {
        QVariantList engines;
        for (const auto& e : g.engines)
            engines.push_back(QVariantMap{{u"name"_s, QString::fromStdWString(e.name)}, {u"pct"_s, double(e.pct)}});
        out.push_back(QVariantMap{
            {u"name"_s, QString::fromStdWString(g.name)},
            {u"total"_s, double(g.total_pct)},
            {u"temp"_s, double(g.temp_c)},
            {u"dedicatedUsed"_s, double(g.dedicated_used)},
            {u"dedicatedTotal"_s, double(g.dedicated_total)},
            {u"sharedUsed"_s, double(g.shared_used)},
            {u"sharedTotal"_s, double(g.shared_total)},
            {u"engines"_s, engines}});
    }
    return out;
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

std::vector<SnapshotBridge::TimedValue> SnapshotBridge::timedSeries(const QString& metric, int n) const
{
    std::vector<TimedValue> out;
    if (n <= 0) return out;
    const auto snaps = history_.tail(std::size_t(n) + std::size_t(scrub_back_));
    const auto end = snaps.size() - std::min(snaps.size(), std::size_t(scrub_back_));
    out.reserve(end);
    for (std::size_t i = 0; i < end; ++i)
        out.push_back({snaps[i]->time_ms, metric_of(*snaps[i], metric)});
    return out;
}
