// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <windows.h>
#include <winternl.h>

#include <cstdint>
#include <utility>

// Private NT structs and info classes that winternl.h does not expose. Layouts follow
// the public reverse-engineered documentation and have been stable since Vista for the
// fields read here. Names carry an _R0 suffix so they never collide with the partial
// definitions winternl.h ships under the canonical names.

namespace ring0::nt {

inline constexpr ULONG kSystemPerformanceInformation          = 2;
inline constexpr ULONG kSystemProcessInformation              = 5;
inline constexpr ULONG kSystemProcessorPerformanceInformation = 8;
inline constexpr ULONG kProcessCommandLineInformation         = 60;

inline constexpr NTSTATUS kStatusSuccess            = 0;
inline constexpr NTSTATUS kStatusInfoLengthMismatch = static_cast<NTSTATUS>(0xC0000004L);
inline constexpr NTSTATUS kStatusBufferTooSmall     = static_cast<NTSTATUS>(0xC0000023L);
inline constexpr NTSTATUS kStatusBufferOverflow     = static_cast<NTSTATUS>(0x80000005L);

// KTHREAD_STATE Waiting and KWAIT_REASON Suspended.
inline constexpr ULONG kThreadStateWaiting = 5;
inline constexpr ULONG kWaitReasonSuspended = 5;

constexpr bool nt_success(NTSTATUS s) noexcept { return s >= 0; }

// Partial layout: fields up to FreeSystemPtes are read; the tail absorbs the rest so
// the call does not fail with a length mismatch on newer builds.
struct SYSTEM_PERFORMANCE_INFORMATION_R0 {
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
    BYTE  _tail[512];
};

struct SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION_R0 {
    LARGE_INTEGER IdleTime;
    LARGE_INTEGER KernelTime;     // includes IdleTime
    LARGE_INTEGER UserTime;
    LARGE_INTEGER DpcTime;
    LARGE_INTEGER InterruptTime;
    ULONG         InterruptCount;
};

struct SYSTEM_THREAD_INFORMATION_R0 {
    LARGE_INTEGER KernelTime;
    LARGE_INTEGER UserTime;
    LARGE_INTEGER CreateTime;
    ULONG         WaitTime;
    PVOID         StartAddress;
    CLIENT_ID     ClientId;
    LONG          Priority;
    LONG          BasePriority;
    ULONG         ContextSwitches;
    ULONG         ThreadState;
    ULONG         WaitReason;
};

struct SYSTEM_PROCESS_INFORMATION_R0 {
    ULONG          NextEntryOffset;
    ULONG          NumberOfThreads;
    LARGE_INTEGER  WorkingSetPrivateSize;
    ULONG          HardFaultCount;
    ULONG          NumberOfThreadsHighWatermark;
    ULONGLONG      CycleTime;
    LARGE_INTEGER  CreateTime;
    LARGE_INTEGER  UserTime;
    LARGE_INTEGER  KernelTime;
    UNICODE_STRING ImageName;
    LONG           BasePriority;
    HANDLE         UniqueProcessId;
    HANDLE         InheritedFromUniqueProcessId;
    ULONG          HandleCount;
    ULONG          SessionId;
    ULONG_PTR      UniqueProcessKey;
    SIZE_T         PeakVirtualSize;
    SIZE_T         VirtualSize;
    ULONG          PageFaultCount;
    SIZE_T         PeakWorkingSetSize;
    SIZE_T         WorkingSetSize;
    SIZE_T         QuotaPeakPagedPoolUsage;
    SIZE_T         QuotaPagedPoolUsage;
    SIZE_T         QuotaPeakNonPagedPoolUsage;
    SIZE_T         QuotaNonPagedPoolUsage;
    SIZE_T         PagefileUsage;
    SIZE_T         PeakPagefileUsage;
    SIZE_T         PrivatePageCount;
    LARGE_INTEGER  ReadOperationCount;
    LARGE_INTEGER  WriteOperationCount;
    LARGE_INTEGER  OtherOperationCount;
    LARGE_INTEGER  ReadTransferCount;
    LARGE_INTEGER  WriteTransferCount;
    LARGE_INTEGER  OtherTransferCount;
    SYSTEM_THREAD_INFORMATION_R0 Threads[1];
};

using NtQuerySystemInformationFn = NTSTATUS(NTAPI*)(ULONG, PVOID, ULONG, PULONG);
using NtQueryInformationProcessFn = NTSTATUS(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
using NtSuspendProcessFn = NTSTATUS(NTAPI*)(HANDLE);
using NtResumeProcessFn  = NTSTATUS(NTAPI*)(HANDLE);

// Resolved through GetProcAddress rather than ntdll.lib so nothing depends on which
// exports a given SDK's import library happens to carry. Any member may be null.
struct Api {
    NtQuerySystemInformationFn  query_system_information = nullptr;
    NtQueryInformationProcessFn query_information_process = nullptr;
    NtSuspendProcessFn          suspend_process = nullptr;
    NtResumeProcessFn           resume_process = nullptr;
};

// Resolved once on first use; thread-safe through static initialization.
const Api& api() noexcept;

// Owns a kernel handle. Treats both null and INVALID_HANDLE_VALUE as empty, since
// OpenProcess returns the former and CreateFile the latter.
class unique_handle {
public:
    unique_handle() noexcept = default;
    explicit unique_handle(HANDLE h) noexcept : h_(h) {}
    ~unique_handle() { reset(); }

    unique_handle(const unique_handle&) = delete;
    unique_handle& operator=(const unique_handle&) = delete;
    unique_handle(unique_handle&& o) noexcept : h_(std::exchange(o.h_, nullptr)) {}
    unique_handle& operator=(unique_handle&& o) noexcept {
        if (this != &o) reset(std::exchange(o.h_, nullptr));
        return *this;
    }

    [[nodiscard]] HANDLE get() const noexcept { return h_; }
    [[nodiscard]] bool valid() const noexcept { return h_ && h_ != INVALID_HANDLE_VALUE; }
    explicit operator bool() const noexcept { return valid(); }

    HANDLE release() noexcept { return std::exchange(h_, nullptr); }
    void reset(HANDLE h = nullptr) noexcept {
        if (valid()) ::CloseHandle(h_);
        h_ = h;
    }

private:
    HANDLE h_ = nullptr;
};

inline unique_handle open_process(DWORD access, std::uint32_t pid) noexcept {
    return unique_handle(::OpenProcess(access, FALSE, pid));
}

} // namespace ring0::nt
