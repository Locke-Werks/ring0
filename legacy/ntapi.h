#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>

/* Private NT structs and info-class constants that aren't exposed in winternl.h.
   Layout taken from public reverse-engineered documentation; stable since Vista
   for the fields we touch. */

#ifndef SystemProcessInformation
#define SystemProcessInformation 5
#endif

#ifndef SystemProcessorPerformanceInformation
#define SystemProcessorPerformanceInformation 8
#endif

#ifndef SystemPerformanceInformation
#define SystemPerformanceInformation 2
#endif

/* Partial layout — we read fields up to NonPagedPoolPages. Stable since XP. */
typedef struct _SYSTEM_PERFORMANCE_INFORMATION_R0 {
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
    /* more fields follow, irrelevant */
    BYTE  _tail[512];
} SYSTEM_PERFORMANCE_INFORMATION_R0;

typedef struct _SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION_R0 {
    LARGE_INTEGER IdleTime;
    LARGE_INTEGER KernelTime;     /* includes IdleTime */
    LARGE_INTEGER UserTime;
    LARGE_INTEGER DpcTime;
    LARGE_INTEGER InterruptTime;
    ULONG         InterruptCount;
} SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION_R0;

typedef struct _SYSTEM_THREAD_INFORMATION_R0 {
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
} SYSTEM_THREAD_INFORMATION_R0;

typedef struct _SYSTEM_PROCESS_INFORMATION_R0 {
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
} SYSTEM_PROCESS_INFORMATION_R0;

typedef NTSTATUS (NTAPI *fn_NtQuerySystemInformation)(
    ULONG SystemInformationClass,
    PVOID SystemInformation,
    ULONG SystemInformationLength,
    PULONG ReturnLength);

typedef NTSTATUS (NTAPI *fn_NtQueryInformationProcess)(
    HANDLE ProcessHandle,
    ULONG  ProcessInformationClass,
    PVOID  ProcessInformation,
    ULONG  ProcessInformationLength,
    PULONG ReturnLength);

typedef NTSTATUS (NTAPI *fn_NtSuspendProcess)(HANDLE ProcessHandle);
typedef NTSTATUS (NTAPI *fn_NtResumeProcess)(HANDLE ProcessHandle);

extern fn_NtQuerySystemInformation   nt_qsi;
extern fn_NtQueryInformationProcess  nt_qip;
extern fn_NtSuspendProcess           nt_suspend;
extern fn_NtResumeProcess            nt_resume;

void nt_init(void);

#ifndef STATUS_INFO_LENGTH_MISMATCH
#define STATUS_INFO_LENGTH_MISMATCH ((NTSTATUS)0xC0000004L)
#endif
