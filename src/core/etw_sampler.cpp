// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/sampler.hpp"
#include "core/etw_parse.hpp"

#include <windows.h>
#include <evntcons.h>
#include <evntrace.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>

// NT Kernel Logger session: DiskIo read/write and TcpIp/UdpIp send/recv, folded into
// cumulative per-pid byte counts. Thread events are traced too: disk completions name
// their issuing thread rather than a process, so a tid -> pid map resolves them.
// Only one kernel logger can exist machine-wide, so an
// orphan left by a crashed run is stopped and the start retried, as the legacy code did.

namespace ring0 {
namespace {

constexpr GUID kSystemTraceGuid =
    {0x9E814AAD, 0x3204, 0x11D2, {0x9A, 0x82, 0x00, 0x60, 0x08, 0xA8, 0x69, 0x39}};
constexpr GUID kDiskIoGuid =
    {0x3D6FA8D4, 0xFE05, 0x11D0, {0x9D, 0xDA, 0x00, 0xC0, 0x4F, 0xD7, 0xBA, 0x7C}};
constexpr GUID kThreadGuid =
    {0x3D6FA8D1, 0xFE05, 0x11D0, {0x9D, 0xDA, 0x00, 0xC0, 0x4F, 0xD7, 0xBA, 0x7C}};
constexpr GUID kTcpIpGuid =
    {0x9A280AC0, 0xC8E0, 0x11D1, {0x84, 0xE2, 0x00, 0xC0, 0x4F, 0xB9, 0x98, 0xA2}};
constexpr GUID kUdpIpGuid =
    {0xBF3A50C5, 0xA9C9, 0x4988, {0xA0, 0x05, 0x2D, 0xF0, 0xB7, 0xC8, 0x0F, 0x80}};

// Kernel classic-event opcodes.
constexpr UCHAR kDiskRead = 10, kDiskWrite = 11;
// Thread start/end, and the rundown the kernel logger emits for threads already running.
constexpr UCHAR kThreadStart = 1, kThreadEnd = 2, kThreadDcStart = 3, kThreadDcEnd = 4;
constexpr UCHAR kNetSend = 10, kNetRecv = 11, kNetSendV6 = 26, kNetRecvV6 = 27;

// TcpIp/UdpIp send/recv (v4 and v6): PID then size lead the payload.
constexpr std::size_t kNetHeadBytes = 2 * sizeof(ULONG);

constexpr ULONG kPropsBytes = sizeof(EVENT_TRACE_PROPERTIES) + 2 * sizeof(KERNEL_LOGGER_NAMEW);

struct PropsBuf {
    alignas(EVENT_TRACE_PROPERTIES) BYTE bytes[kPropsBytes];
    EVENT_TRACE_PROPERTIES* get() { return reinterpret_cast<EVENT_TRACE_PROPERTIES*>(bytes); }
};

void fill_props(PropsBuf& buf, ULONG flags) {
    std::memset(buf.bytes, 0, sizeof buf.bytes);
    auto* p = buf.get();
    p->Wnode.BufferSize = kPropsBytes;
    p->Wnode.Guid = kSystemTraceGuid;
    p->Wnode.ClientContext = 1; // QPC timestamps
    p->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    p->EnableFlags = flags;
    p->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
    p->FlushTimer = 1;
    p->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
}

struct Totals {
    std::uint64_t disk = 0;
    std::uint64_t net = 0;
};

ULONG read_ulong(const EVENT_RECORD* ev, std::size_t offset) {
    ULONG v = 0;
    std::memcpy(&v, static_cast<const BYTE*>(ev->UserData) + offset, sizeof v);
    return v;
}

class EtwSampler final : public Sampler {
public:
    EtwSampler() noexcept { start(); }

    ~EtwSampler() override { stop(); }

    EtwSampler(const EtwSampler&) = delete;
    EtwSampler& operator=(const EtwSampler&) = delete;

    void sample(Snapshot& out, const Snapshot* prev, double dt_seconds) override {
        if (session_ == 0) return;
        try {
            fold(out, prev, dt_seconds);
        } catch (...) {
            // Samplers must not throw; a failed allocation just skips this tick.
        }
    }

private:
    void fold(Snapshot& out, const Snapshot* prev, double dt_seconds) {

        std::unordered_map<std::uint32_t, Totals> copy;
        {
            std::lock_guard lock(mu_);
            copy = totals_;
        }

        // Keyed by pid + create_time so a recycled pid never produces a negative or
        // inflated rate against its predecessor.
        std::unordered_map<std::uint64_t, std::pair<std::uint64_t, std::uint64_t>> before;
        if (prev && dt_seconds > 0) {
            before.reserve(prev->processes.size());
            for (const auto& p : prev->processes)
                before.emplace(key(p), std::pair{p.etw_disk_bytes, p.etw_net_bytes});
        }

        for (auto& p : out.processes) {
            const auto it = copy.find(p.pid);
            if (it == copy.end()) continue;
            p.etw_disk_bytes = it->second.disk;
            p.etw_net_bytes = it->second.net;

            const auto b = before.find(key(p));
            if (b == before.end()) continue;
            if (p.etw_disk_bytes >= b->second.first)
                p.disk_bps = static_cast<float>(
                    static_cast<double>(p.etw_disk_bytes - b->second.first) / dt_seconds);
            if (p.etw_net_bytes >= b->second.second)
                p.net_bps = static_cast<float>(
                    static_cast<double>(p.etw_net_bytes - b->second.second) / dt_seconds);
        }

        // Exited pids would otherwise accumulate for the life of the session.
        if (++prune_tick_ % 30 == 0) {
            std::unordered_map<std::uint32_t, bool> live;
            live.reserve(out.processes.size());
            for (const auto& p : out.processes) live.emplace(p.pid, true);
            std::lock_guard lock(mu_);
            std::erase_if(totals_, [&](const auto& kv) { return !live.contains(kv.first); });
        }
    }

    static std::uint64_t key(const Process& p) {
        return p.create_time ^ (static_cast<std::uint64_t>(p.pid) << 1);
    }

    void start() noexcept {
        PropsBuf buf;
        const ULONG flags = EVENT_TRACE_FLAG_DISK_IO | EVENT_TRACE_FLAG_THREAD | EVENT_TRACE_FLAG_NETWORK_TCPIP;
        fill_props(buf, flags);
        TRACEHANDLE h = 0;
        ULONG r = StartTraceW(&h, KERNEL_LOGGER_NAMEW, buf.get());
        if (r == ERROR_ALREADY_EXISTS) {
            fill_props(buf, 0);
            ControlTraceW(0, KERNEL_LOGGER_NAMEW, buf.get(), EVENT_TRACE_CONTROL_STOP);
            fill_props(buf, flags);
            r = StartTraceW(&h, KERNEL_LOGGER_NAMEW, buf.get());
        }
        // ERROR_ACCESS_DENIED without elevation lands here too: leave the fields zero.
        if (r != ERROR_SUCCESS) return;
        session_ = h;

        EVENT_TRACE_LOGFILEW lf{};
        lf.LoggerName = const_cast<LPWSTR>(KERNEL_LOGGER_NAMEW);
        lf.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
        lf.EventRecordCallback = &EtwSampler::on_event;
        lf.Context = this;
        // Opened here rather than on the consumer thread so stop() always has a handle
        // to close, which is what unblocks ProcessTrace.
        consumer_ = OpenTraceW(&lf);
        if (consumer_ == INVALID_PROCESSTRACE_HANDLE) {
            consumer_ = 0;
            stop();
            return;
        }

        try {
            thread_ = std::jthread([this] {
                TRACEHANDLE c = consumer_;
                ProcessTrace(&c, 1, nullptr, nullptr);
            });
        } catch (...) {
            stop();
        }
    }

    void stop() noexcept {
        if (session_ != 0) {
            PropsBuf buf;
            fill_props(buf, 0);
            ControlTraceW(session_, KERNEL_LOGGER_NAMEW, buf.get(), EVENT_TRACE_CONTROL_STOP);
            session_ = 0;
        }
        if (consumer_ != 0) {
            CloseTrace(consumer_);
            consumer_ = 0;
        }
        if (thread_.joinable()) thread_.join();
    }

    static VOID WINAPI on_event(PEVENT_RECORD ev) {
        auto* self = static_cast<EtwSampler*>(ev->UserContext);
        if (!self || !ev->UserData) return;
        const auto& hdr = ev->EventHeader;
        const UCHAR op = hdr.EventDescriptor.Opcode;

        if (IsEqualGUID(hdr.ProviderId, kThreadGuid)) {
            const auto ids = etw::thread_ids(ev->UserData, ev->UserDataLength);
            if (!ids) return;
            try {
                if (op == kThreadStart || op == kThreadDcStart) self->threads_[ids->tid] = ids->pid;
                else if (op == kThreadEnd || op == kThreadDcEnd) self->threads_.erase(ids->tid);
            } catch (...) {
            }
            return;
        }

        if (IsEqualGUID(hdr.ProviderId, kDiskIoGuid)) {
            if (op != kDiskRead && op != kDiskWrite) return;
            const auto size = etw::disk_transfer_size(ev->UserData, ev->UserDataLength);
            if (!size) return;
            const std::size_t ptr = (hdr.Flags & EVENT_HEADER_FLAG_32_BIT_HEADER) ? 4 : 8;
            std::uint32_t pid = 0;
            if (const auto tid = etw::disk_issuing_tid(ev->UserData, ev->UserDataLength, ptr)) {
                if (const auto it = self->threads_.find(*tid); it != self->threads_.end())
                    pid = it->second;
            }
            if (pid == 0) pid = hdr.ProcessId;
            if (pid == 0 || pid == 0xFFFFFFFFu) return;
            self->add(pid, *size, 0);
            return;
        }

        if (IsEqualGUID(hdr.ProviderId, kTcpIpGuid) || IsEqualGUID(hdr.ProviderId, kUdpIpGuid)) {
            if (op != kNetSend && op != kNetRecv && op != kNetSendV6 && op != kNetRecvV6) return;
            if (ev->UserDataLength < kNetHeadBytes) return;
            const std::uint32_t pid = read_ulong(ev, 0);
            if (pid == 0) return;
            self->add(pid, 0, read_ulong(ev, sizeof(ULONG)));
        }
    }

    void add(std::uint32_t pid, std::uint64_t disk, std::uint64_t net) noexcept {
        try {
            std::lock_guard lock(mu_);
            auto& t = totals_[pid];
            t.disk += disk;
            t.net += net;
        } catch (...) {
            // Allocation failure on the consumer thread drops one event; nothing better to do.
        }
    }

    std::mutex mu_;
    // Touched only by the consumer thread inside on_event, so it needs no lock.
    std::unordered_map<std::uint32_t, std::uint32_t> threads_;
    std::unordered_map<std::uint32_t, Totals> totals_;
    TRACEHANDLE session_ = 0;
    TRACEHANDLE consumer_ = 0;
    std::uint64_t prune_tick_ = 0;
    std::jthread thread_; // last member: joined before the state it touches is destroyed
};

} // namespace

std::unique_ptr<Sampler> make_etw_sampler() {
    try {
        return std::make_unique<EtwSampler>();
    } catch (...) {
        return nullptr;
    }
}

} // namespace ring0
