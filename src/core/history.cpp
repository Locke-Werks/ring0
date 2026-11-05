// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/history.hpp"
#include "core/sampler.hpp"
#include "core/timebase.hpp"

#include <windows.h>

#include <chrono>

namespace ring0 {

void History::push(SnapshotPtr s)
{
    {
        std::scoped_lock lk(mu_);
        ring_[head_] = s;
        head_ = (head_ + 1) % kCapacity;
        if (count_ < kCapacity) ++count_;
    }
    latest_.store(std::move(s), std::memory_order_release);
}

SnapshotPtr History::at(std::size_t back) const
{
    std::scoped_lock lk(mu_);
    if (back >= count_) return {};
    return ring_[(head_ + kCapacity - 1 - back) % kCapacity];
}

std::size_t History::size() const
{
    std::scoped_lock lk(mu_);
    return count_;
}

std::vector<SnapshotPtr> History::tail(std::size_t n) const
{
    std::scoped_lock lk(mu_);
    n = n < count_ ? n : count_;
    std::vector<SnapshotPtr> out;
    out.reserve(n);
    for (std::size_t i = n; i-- > 0;)
        out.push_back(ring_[(head_ + kCapacity - 1 - i) % kCapacity]);
    return out;
}

SamplerThread::SamplerThread(History& history, unsigned period_ms)
    : history_(history), period_ms_(period_ms),
      thread_([this](std::stop_token st) { run(st); })
{
}

SamplerThread::~SamplerThread() = default;  // jthread requests stop and joins

void SamplerThread::run(std::stop_token st)
{
    SetThreadDescription(GetCurrentThread(), L"ring0 sampler");

    // Order matters: gpu and etw annotate the process list the process sampler built.
    std::vector<std::unique_ptr<Sampler>> samplers;
    samplers.push_back(make_process_sampler());
    samplers.push_back(make_cpu_sampler());
    samplers.push_back(make_memory_sampler());
    samplers.push_back(make_disk_sampler());
    samplers.push_back(make_net_sampler());
    samplers.push_back(make_gpu_sampler());
    samplers.push_back(make_thermal_sampler());
    samplers.push_back(make_etw_sampler());

    // A waitable event lets the destructor's stop request cut the sleep short.
    HANDLE wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    std::stop_callback cb(st, [wake] { SetEvent(wake); });

    // Absolute deadlines: sample k is taken at start + k * period no matter how
    // long the previous pass took, so the spacing between samples stays even.
    std::uint64_t seq = 0;
    double last_time = 0;
    const double start = timebase::now_ms();
    double deadline = start;
    while (!st.stop_requested()) {
        auto snap = std::make_shared<Snapshot>();
        snap->seq = ++seq;
        snap->tick_ms = GetTickCount64();
        snap->time_ms = timebase::now_ms();
        const double dt = last_time > 0 ? (snap->time_ms - last_time) / 1000.0 : 0.0;
        last_time = snap->time_ms;

        SnapshotPtr prev = history_.latest();
        for (auto& s : samplers)
            if (s) s->sample(*snap, prev.get(), dt);

        history_.push(std::move(snap));

        deadline += period_ms_;
        const double now = timebase::now_ms();
        // After a long stall (sleep, debugger) resync instead of firing a burst.
        if (deadline < now) deadline = now + period_ms_;
        // Default Windows timer granularity is ~15.6 ms; the timestamp, not the
        // wake-up, is what the graphs use, so a coarse wake is harmless.
        WaitForSingleObject(wake, DWORD(deadline - now));
    }
    CloseHandle(wake);
}

} // namespace ring0
