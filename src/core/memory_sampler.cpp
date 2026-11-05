// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/sampler.hpp"

#include <windows.h>
#include <psapi.h>
#include <winternl.h>

#include <cstdint>

#pragma comment(lib, "ntdll.lib")
#pragma comment(lib, "psapi.lib")

namespace ring0 {
namespace {

constexpr ULONG kSystemPerformanceInformation = 2;

// Only the prefix up to NonPagedPoolPages is read; the layout of that prefix has been
// stable since XP. The tail pads the buffer so newer kernels that write a longer struct
// do not fail with STATUS_INFO_LENGTH_MISMATCH.
struct SystemPerformanceInformation {
    LARGE_INTEGER IdleProcessTime;
    LARGE_INTEGER IoReadTransferCount;
    LARGE_INTEGER IoWriteTransferCount;
    LARGE_INTEGER IoOtherTransferCount;
    ULONG IoReadOperationCount;
    ULONG IoWriteOperationCount;
    ULONG IoOtherOperationCount;
    ULONG AvailablePages;
    ULONG CommittedPages;
    ULONG CommitLimit;
    ULONG PeakCommitment;
    ULONG PageFaultCount;
    ULONG CopyOnWriteCount;
    ULONG TransitionCount;
    ULONG CacheTransitionCount;
    ULONG DemandZeroCount;
    ULONG PageReadCount;
    ULONG PageReadIoCount;
    ULONG CacheReadCount;
    ULONG CacheIoCount;
    ULONG DirtyPagesWriteCount;
    ULONG DirtyWriteIoCount;
    ULONG MappedPagesWriteCount;
    ULONG MappedWriteIoCount;
    ULONG PagedPoolPages;
    ULONG NonPagedPoolPages;
    ULONG PagedPoolAllocs;
    ULONG PagedPoolFrees;
    ULONG NonPagedPoolAllocs;
    ULONG NonPagedPoolFrees;
    ULONG FreeSystemPtes;
    BYTE  tail[512];
};

class MemorySampler final : public Sampler {
public:
    void sample(Snapshot& out, const Snapshot* prev, double dt_seconds) override {
        Memory& m = out.memory;

        MEMORYSTATUSEX ms{};
        ms.dwLength = sizeof ms;
        if (GlobalMemoryStatusEx(&ms)) {
            m.phys_total   = ms.ullTotalPhys;
            m.phys_avail   = ms.ullAvailPhys;
            m.commit_limit = ms.ullTotalPageFile;
            m.commit_used  = ms.ullTotalPageFile > ms.ullAvailPageFile
                                 ? ms.ullTotalPageFile - ms.ullAvailPageFile : 0;
        }

        PERFORMANCE_INFORMATION pi{};
        pi.cb = sizeof pi;
        std::uint64_t page = 0;
        if (GetPerformanceInfo(&pi, sizeof pi)) {
            page = pi.PageSize;
            // SystemCache is standby plus modified pages, which is what Task Manager
            // labels "Cached".
            m.cached = static_cast<std::uint64_t>(pi.SystemCache) * page;
        }
        if (page == 0) {
            SYSTEM_INFO si{};
            GetSystemInfo(&si);
            page = si.dwPageSize;
        }

        SystemPerformanceInformation spi{};
        ULONG ret = 0;
        if (!NT_SUCCESS(NtQuerySystemInformation(
                static_cast<SYSTEM_INFORMATION_CLASS>(kSystemPerformanceInformation),
                &spi, sizeof spi, &ret)))
            return;

        m.paged_pool    = static_cast<std::uint64_t>(spi.PagedPoolPages) * page;
        m.nonpaged_pool = static_cast<std::uint64_t>(spi.NonPagedPoolPages) * page;

        // The counter is a 32-bit ULONG that wraps; unsigned subtraction handles that.
        if (have_prev_ && prev && dt_seconds > 0) {
            const std::uint32_t delta = spi.PageFaultCount - prev_faults_;
            m.page_faults_per_sec = static_cast<float>(delta / dt_seconds);
        }
        prev_faults_ = spi.PageFaultCount;
        have_prev_ = true;
    }

private:
    std::uint32_t prev_faults_ = 0;
    bool have_prev_ = false;
};

} // namespace

std::unique_ptr<Sampler> make_memory_sampler() { return std::make_unique<MemorySampler>(); }

} // namespace ring0
