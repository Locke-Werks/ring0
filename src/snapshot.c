#include "snapshot.h"
#include "ntapi.h"
#include "intern.h"
#include <stdint.h>

/* Snapshot arena pool. Four is enough cushion: the UI frame holds a snapshot
   pointer for ~16ms and the sampler period is 250ms+, so the oldest arena
   is always at least 750ms idle before recycling. */
#define POOL_N 4

static arena_t          *g_scratch;
static arena_t          *g_pool[POOL_N];
static unsigned          g_next;            /* next pool slot to recycle */
static volatile snapshot_t *g_current;
static HANDLE            g_exit_event;
static HANDLE            g_thread;
static HWND              g_notify;
static unsigned          g_period_ms;

/* Build a compact process table from NtQuerySystemInformation(SystemProcessInformation).
   The NT call returns a variable-sized blob (typically ~1 MB on a busy box) into
   `scratch`. We then walk the linked list and emit POD process_t entries into
   `dest`. Image names go through the global interner so they're deduped across
   snapshots. */

snapshot_t *snapshot_build(arena_t *dest, arena_t *scratch)
{
    if (!nt_qsi) return NULL;

    /* grow scratch until NtQSI stops complaining */
    size_t   cap = 256 * 1024;
    uint8_t *buf = NULL;
    ULONG    need = 0;
    NTSTATUS st;

    for (int tries = 0; tries < 8; ++tries) {
        arena_reset(scratch);
        buf = arena_alloc(scratch, cap, 16);
        if (!buf) return NULL;

        st = nt_qsi(SystemProcessInformation, buf, (ULONG)cap, &need);
        if (st == 0) break;
        if (st != STATUS_INFO_LENGTH_MISMATCH) return NULL;
        cap = (size_t)need + 64 * 1024;
    }
    if (st != 0) return NULL;

    /* first pass — count */
    uint32_t n = 0;
    uint8_t *p = buf;
    for (;;) {
        SYSTEM_PROCESS_INFORMATION_R0 *spi = (SYSTEM_PROCESS_INFORMATION_R0 *)p;
        ++n;
        if (spi->NextEntryOffset == 0) break;
        p += spi->NextEntryOffset;
    }

    snapshot_t *snap = arena_alloc(dest, sizeof *snap, 8);
    if (!snap) return NULL;
    snap->timestamp   = GetTickCount64();
    snap->n_processes = n;
    snap->_pad        = 0;
    snap->processes   = arena_alloc(dest, (size_t)n * sizeof(process_t), 16);
    if (!snap->processes) return NULL;

    /* second pass — fill */
    p = buf;
    for (uint32_t i = 0; i < n; ++i) {
        SYSTEM_PROCESS_INFORMATION_R0 *spi = (SYSTEM_PROCESS_INFORMATION_R0 *)p;
        process_t *pr = &snap->processes[i];

        pr->pid           = (uint32_t)(ULONG_PTR)spi->UniqueProcessId;
        pr->ppid          = (uint32_t)(ULONG_PTR)spi->InheritedFromUniqueProcessId;
        pr->session_id    = spi->SessionId;
        pr->thread_count  = spi->NumberOfThreads;
        pr->handle_count  = spi->HandleCount;
        pr->page_faults   = spi->PageFaultCount;
        pr->_pad          = 0;
        pr->working_set   = (uint64_t)spi->WorkingSetSize;
        pr->virtual_size  = (uint64_t)spi->VirtualSize;
        pr->cpu_100ns     = (uint64_t)(spi->KernelTime.QuadPart + spi->UserTime.QuadPart);
        pr->io_read_bytes = (uint64_t)spi->ReadTransferCount.QuadPart;
        pr->io_write_bytes= (uint64_t)spi->WriteTransferCount.QuadPart;
        pr->io_other_bytes= (uint64_t)spi->OtherTransferCount.QuadPart;
        pr->create_time   = (uint64_t)spi->CreateTime.QuadPart;

        if (spi->ImageName.Buffer && spi->ImageName.Length) {
            size_t wlen = spi->ImageName.Length / sizeof(WCHAR);
            pr->image_name = intern_str(spi->ImageName.Buffer, wlen);
        } else {
            pr->image_name = 0;
        }

        if (spi->NextEntryOffset == 0) break;
        p += spi->NextEntryOffset;
    }

    return snap;
}

static void take_sample(void)
{
    arena_t *dest = g_pool[g_next];
    arena_reset(dest);
    snapshot_t *snap = snapshot_build(dest, g_scratch);
    if (!snap) return;

    InterlockedExchangePointer((PVOID *)&g_current, snap);
    g_next = (g_next + 1) % POOL_N;

    if (g_notify) InvalidateRect(g_notify, NULL, FALSE);
}

static DWORD WINAPI sampler_thread(LPVOID p)
{
    (void)p;
    take_sample();
    while (WaitForSingleObject(g_exit_event, g_period_ms) == WAIT_TIMEOUT) {
        take_sample();
    }
    return 0;
}

void snapshot_start(HWND notify, unsigned period_ms)
{
    if (g_thread) return;
    nt_init();
    intern_init();

    g_notify    = notify;
    g_period_ms = period_ms ? period_ms : 250;

    g_scratch = arena_create(8 * 1024 * 1024);
    for (int i = 0; i < POOL_N; ++i) g_pool[i] = arena_create(2 * 1024 * 1024);

    g_exit_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_thread     = CreateThread(NULL, 0, sampler_thread, NULL, 0, NULL);
}

void snapshot_stop(void)
{
    if (!g_thread) return;
    SetEvent(g_exit_event);
    WaitForSingleObject(g_thread, 2000);
    CloseHandle(g_thread);
    CloseHandle(g_exit_event);
    g_thread = NULL;

    for (int i = 0; i < POOL_N; ++i) {
        arena_destroy(g_pool[i]);
        g_pool[i] = NULL;
    }
    arena_destroy(g_scratch);
    g_scratch = NULL;
    g_current = NULL;
}

const snapshot_t *snapshot_current(void)
{
    return (const snapshot_t *)g_current;
}
