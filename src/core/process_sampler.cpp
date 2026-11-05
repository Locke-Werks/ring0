// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/sampler.hpp"
#include "core/nt.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ring0 {

namespace {

struct ProcessKey {
    std::uint32_t pid;
    std::uint64_t create_time;
    bool operator==(const ProcessKey&) const = default;
};

struct ProcessKeyHash {
    std::size_t operator()(const ProcessKey& k) const noexcept {
        std::uint64_t h = k.create_time * 0x9E3779B97F4A7C15ull;
        return static_cast<std::size_t>(h ^ (static_cast<std::uint64_t>(k.pid) * 2654435761u));
    }
};

// Path and command line never change for the life of a process, so each is resolved
// once. Failures are cached too: an access-denied process would otherwise cost two
// OpenProcess calls every sample for its whole life.
struct Resolved {
    std::wstring image_path;
    std::wstring command_line;
    bool access_denied = false;
    std::uint64_t seen_gen = 0;
};

std::wstring_view to_view(const UNICODE_STRING& us) noexcept {
    if (!us.Buffer || us.Length == 0) return {};
    return {us.Buffer, us.Length / sizeof(WCHAR)};
}

std::wstring query_image_path(HANDLE ph) {
    // MAX_PATH is not a real limit for process images; long-path-aware installs exceed it.
    std::array<wchar_t, 32768> buf;
    DWORD sz = static_cast<DWORD>(buf.size());
    if (::QueryFullProcessImageNameW(ph, 0, buf.data(), &sz) && sz > 0)
        return std::wstring(buf.data(), sz);
    return {};
}

std::wstring query_command_line(HANDLE ph, nt::NtQueryInformationProcessFn qip) {
    if (!qip) return {};
    // UNICODE_STRING header followed by the text; 8-byte elements keep the header aligned.
    std::vector<std::uint64_t> buf(512);
    for (int tries = 0; tries < 4; ++tries) {
        ULONG bytes = static_cast<ULONG>(buf.size() * sizeof(std::uint64_t));
        ULONG ret = 0;
        NTSTATUS st = qip(ph, nt::kProcessCommandLineInformation, buf.data(), bytes, &ret);
        if (nt::nt_success(st)) {
            if (ret < sizeof(UNICODE_STRING)) return {};
            const auto* us = reinterpret_cast<const UNICODE_STRING*>(buf.data());
            return std::wstring(to_view(*us));
        }
        if (st != nt::kStatusInfoLengthMismatch && st != nt::kStatusBufferTooSmall &&
            st != nt::kStatusBufferOverflow)
            return {};
        if (ret <= bytes) return {};
        buf.resize(ret / sizeof(std::uint64_t) + 1);
    }
    return {};
}

bool all_threads_suspended(const nt::SYSTEM_PROCESS_INFORMATION_R0& spi) noexcept {
    if (spi.NumberOfThreads == 0) return false;
    // Threads is a trailing variable-length array; indexing past [0] is the documented layout.
    const nt::SYSTEM_THREAD_INFORMATION_R0* t = spi.Threads;
    for (ULONG i = 0; i < spi.NumberOfThreads; ++i) {
        if (t[i].ThreadState != nt::kThreadStateWaiting ||
            t[i].WaitReason != nt::kWaitReasonSuspended)
            return false;
    }
    return true;
}

class ProcessSampler final : public Sampler {
public:
    ProcessSampler() {
        cpus_ = ::GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
        if (cpus_ == 0) cpus_ = 1;
    }

    void sample(Snapshot& out, const Snapshot* prev, double dt_seconds) override {
        try {
            walk(out);
            fill_rates(out, prev, dt_seconds);
        } catch (...) {
            out.processes.clear();
        }
    }

private:
    bool query_process_list() {
        auto qsi = nt::api().query_system_information;
        if (!qsi) return false;
        if (buf_.empty()) buf_.resize(256 * 1024 / sizeof(std::uint64_t));
        for (int tries = 0; tries < 8; ++tries) {
            ULONG bytes = static_cast<ULONG>(buf_.size() * sizeof(std::uint64_t));
            ULONG need = 0;
            NTSTATUS st = qsi(nt::kSystemProcessInformation, buf_.data(), bytes, &need);
            if (nt::nt_success(st)) return true;
            if (st != nt::kStatusInfoLengthMismatch) return false;
            // Headroom because the process list can grow between the two calls.
            buf_.resize((static_cast<std::size_t>(need) + 64 * 1024) / sizeof(std::uint64_t) + 1);
        }
        return false;
    }

    Resolved& resolve(const ProcessKey& key) {
        auto [it, inserted] = cache_.try_emplace(key);
        Resolved& r = it->second;
        r.seen_gen = gen_;
        if (!inserted || key.pid == 0) return r;

        nt::unique_handle ph = nt::open_process(PROCESS_QUERY_LIMITED_INFORMATION, key.pid);
        if (!ph) {
            r.access_denied = true;
            return r;
        }
        r.image_path = query_image_path(ph.get());
        r.command_line = query_command_line(ph.get(), nt::api().query_information_process);
        return r;
    }

    void walk(Snapshot& out) {
        out.processes.clear();
        if (!query_process_list()) return;
        ++gen_;

        const auto* base = reinterpret_cast<const std::byte*>(buf_.data());
        const std::byte* end = base + buf_.size() * sizeof(std::uint64_t);
        const std::byte* p = base;
        for (;;) {
            if (p + sizeof(nt::SYSTEM_PROCESS_INFORMATION_R0) > end) break;
            const auto& spi = *reinterpret_cast<const nt::SYSTEM_PROCESS_INFORMATION_R0*>(p);
            const auto pid = static_cast<std::uint32_t>(reinterpret_cast<ULONG_PTR>(spi.UniqueProcessId));

            // pid 0 is the idle pseudo-process; its "cpu time" is idle time.
            if (pid != 0) emit(out, spi, pid);

            if (spi.NextEntryOffset == 0) break;
            p += spi.NextEntryOffset;
        }

        std::erase_if(cache_, [this](const auto& kv) { return kv.second.seen_gen != gen_; });
    }

    void emit(Snapshot& out, const nt::SYSTEM_PROCESS_INFORMATION_R0& spi, std::uint32_t pid) {
        Process& pr = out.processes.emplace_back();
        pr.pid            = pid;
        pr.ppid           = static_cast<std::uint32_t>(reinterpret_cast<ULONG_PTR>(spi.InheritedFromUniqueProcessId));
        pr.session_id     = spi.SessionId;
        pr.thread_count   = spi.NumberOfThreads;
        pr.handle_count   = spi.HandleCount;
        pr.base_priority  = static_cast<std::uint8_t>(std::clamp<LONG>(spi.BasePriority, 0, 255));
        pr.suspended      = all_threads_suspended(spi);
        pr.create_time    = static_cast<std::uint64_t>(spi.CreateTime.QuadPart);
        pr.working_set    = spi.WorkingSetSize;
        // PrivatePageCount is a byte count despite the name.
        pr.private_bytes  = spi.PrivatePageCount;
        pr.cpu_100ns      = static_cast<std::uint64_t>(spi.KernelTime.QuadPart + spi.UserTime.QuadPart);
        pr.io_read_bytes  = static_cast<std::uint64_t>(spi.ReadTransferCount.QuadPart);
        pr.io_write_bytes = static_cast<std::uint64_t>(spi.WriteTransferCount.QuadPart);

        if (auto name = to_view(spi.ImageName); !name.empty())
            pr.image_name.assign(name);
        else if (pid == 4)
            pr.image_name = L"System";

        const Resolved& r = resolve({pid, pr.create_time});
        pr.image_path = r.image_path;
        pr.command_line = r.command_line;

        // Session 0 holds services and the kernel's own processes; anything a medium
        // integrity caller cannot even open with limited rights is protected (csrss,
        // smss, Registry, Memory Compression) and belongs in the same bucket.
        pr.is_system = pr.session_id == 0 || pid == 4 || (r.access_denied && pr.image_path.empty());
    }

    void fill_rates(Snapshot& out, const Snapshot* prev, double dt_seconds) {
        if (!prev || dt_seconds <= 0.0) return;

        prev_index_.clear();
        prev_index_.reserve(prev->processes.size());
        for (const Process& pp : prev->processes)
            prev_index_.emplace(ProcessKey{pp.pid, pp.create_time}, pp.cpu_100ns);

        const double budget_100ns = dt_seconds * 1e7 * static_cast<double>(cpus_);
        for (Process& pr : out.processes) {
            auto it = prev_index_.find({pr.pid, pr.create_time});
            if (it == prev_index_.end() || pr.cpu_100ns < it->second) continue;
            double pct = static_cast<double>(pr.cpu_100ns - it->second) * 100.0 / budget_100ns;
            pr.cpu_pct = static_cast<float>(std::clamp(pct, 0.0, 100.0));
        }
    }

    std::vector<std::uint64_t> buf_;
    std::unordered_map<ProcessKey, Resolved, ProcessKeyHash> cache_;
    std::unordered_map<ProcessKey, std::uint64_t, ProcessKeyHash> prev_index_;
    std::uint64_t gen_ = 0;
    DWORD cpus_ = 1;
};

} // namespace

std::unique_ptr<Sampler> make_process_sampler() { return std::make_unique<ProcessSampler>(); }

} // namespace ring0
