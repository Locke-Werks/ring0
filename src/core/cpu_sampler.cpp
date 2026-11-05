// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/sampler.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace ring0 {
namespace {

using NTSTATUS = LONG;

constexpr ULONG kSystemProcessorPerformanceInformation = 8;

struct ProcPerf {
    LARGE_INTEGER IdleTime;
    LARGE_INTEGER KernelTime;     // includes IdleTime
    LARGE_INTEGER UserTime;
    LARGE_INTEGER DpcTime;
    LARGE_INTEGER InterruptTime;
    ULONG         InterruptCount;
};

using NtQsiFn   = NTSTATUS(NTAPI*)(ULONG, PVOID, ULONG, PULONG);
using NtQsiExFn = NTSTATUS(NTAPI*)(ULONG, PVOID, ULONG, PVOID, ULONG, PULONG);

struct Times {
    std::uint64_t idle = 0, kernel = 0, user = 0;
};

float pct(std::uint64_t part, std::uint64_t whole) {
    if (whole == 0) return 0.0f;
    return std::clamp(static_cast<float>(static_cast<double>(part) * 100.0 / static_cast<double>(whole)),
                      0.0f, 100.0f);
}

class CpuSampler final : public Sampler {
public:
    CpuSampler() {
        // ntdll is mapped into every process for its lifetime, so no reference is taken.
        if (HMODULE nt = GetModuleHandleW(L"ntdll.dll")) {
            qsi_   = reinterpret_cast<NtQsiFn>(reinterpret_cast<void*>(GetProcAddress(nt, "NtQuerySystemInformation")));
            qsiex_ = reinterpret_cast<NtQsiExFn>(reinterpret_cast<void*>(GetProcAddress(nt, "NtQuerySystemInformationEx")));
        }
    }

    void sample(Snapshot& out, const Snapshot*, double) override {
        out.cores.clear();
        out.cpu_total = 0;
        if (!read(cur_)) return;

        const bool have_prev = prev_.size() == cur_.size();
        out.cores.resize(cur_.size());
        std::uint64_t sum_total = 0, sum_busy = 0;
        if (have_prev) {
            for (std::size_t i = 0; i < cur_.size(); ++i) {
                const std::uint64_t didle = cur_[i].idle - prev_[i].idle;
                const std::uint64_t dkern = cur_[i].kernel - prev_[i].kernel;
                const std::uint64_t duser = cur_[i].user - prev_[i].user;
                const std::uint64_t total = dkern + duser;
                const std::uint64_t busy  = total > didle ? total - didle : 0;
                const std::uint64_t kbusy = dkern > didle ? dkern - didle : 0;
                out.cores[i] = {pct(busy, total), pct(duser, total), pct(kbusy, total)};
                sum_total += total;
                sum_busy += busy;
            }
            out.cpu_total = pct(sum_busy, sum_total);
        }
        prev_.swap(cur_);
    }

private:
    bool read(std::vector<Times>& dst) {
        dst.clear();
        const WORD groups = GetActiveProcessorGroupCount();
        if (groups > 1 && qsiex_) {
            for (WORD g = 0; g < groups && dst.size() < kMaxCores; ++g) {
                USHORT group = g;
                ULONG ret = 0;
                if (qsiex_(kSystemProcessorPerformanceInformation, &group, sizeof group,
                           buf_.data(), static_cast<ULONG>(buf_.size() * sizeof(ProcPerf)), &ret) < 0)
                    return false;
                append(dst, ret);
            }
            return !dst.empty();
        }
        // Without groups the plain query sees only the caller's group, which is the whole
        // machine on 64 or fewer logical processors.
        if (!qsi_) return false;
        ULONG ret = 0;
        if (qsi_(kSystemProcessorPerformanceInformation, buf_.data(),
                 static_cast<ULONG>(buf_.size() * sizeof(ProcPerf)), &ret) < 0)
            return false;
        append(dst, ret);
        return !dst.empty();
    }

    void append(std::vector<Times>& dst, ULONG bytes) const {
        const std::size_t n = std::min<std::size_t>(bytes / sizeof(ProcPerf), buf_.size());
        for (std::size_t i = 0; i < n && dst.size() < kMaxCores; ++i) {
            dst.push_back({static_cast<std::uint64_t>(buf_[i].IdleTime.QuadPart),
                           static_cast<std::uint64_t>(buf_[i].KernelTime.QuadPart),
                           static_cast<std::uint64_t>(buf_[i].UserTime.QuadPart)});
        }
    }

    NtQsiFn   qsi_   = nullptr;
    NtQsiExFn qsiex_ = nullptr;
    // A group holds at most 64 logical processors, so one group always fits.
    std::vector<ProcPerf> buf_ = std::vector<ProcPerf>(64);
    std::vector<Times> cur_, prev_;
};

} // namespace

std::unique_ptr<Sampler> make_cpu_sampler() { return std::make_unique<CpuSampler>(); }

} // namespace ring0
