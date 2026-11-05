// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/snapshot.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <mutex>
#include <stop_token>
#include <thread>
#include <vector>

namespace ring0 {

// Fixed-capacity history. Writer is the sampler thread, readers are the UI thread.
// A short mutex guards the ring; the newest pointer is also published atomically
// so the per-frame read never contends.
class History {
public:
    static constexpr std::size_t kCapacity = 600;   // 10 minutes at 1 Hz

    void push(SnapshotPtr s);
    SnapshotPtr latest() const { return latest_.load(std::memory_order_acquire); }
    // back = 0 is newest. Null if out of range.
    SnapshotPtr at(std::size_t back) const;
    std::size_t size() const;
    // Oldest-to-newest copy of up to n entries, for graph uploads.
    std::vector<SnapshotPtr> tail(std::size_t n) const;

private:
    mutable std::mutex mu_;
    std::array<SnapshotPtr, kCapacity> ring_{};
    std::size_t head_ = 0;
    std::size_t count_ = 0;
    std::atomic<SnapshotPtr> latest_;
};

class SamplerThread {
public:
    explicit SamplerThread(History& history, unsigned period_ms = 1000);
    ~SamplerThread();
    SamplerThread(const SamplerThread&) = delete;
    SamplerThread& operator=(const SamplerThread&) = delete;

private:
    void run(std::stop_token st);
    History& history_;
    unsigned period_ms_;
    std::jthread thread_;
};

} // namespace ring0
