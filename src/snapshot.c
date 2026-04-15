#include "snapshot.h"
#include "ntapi.h"
#include "intern.h"
#include <stdint.h>

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
