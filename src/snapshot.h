#pragma once
#include <stdint.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "arena.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t pid;
    uint32_t ppid;
    uint32_t image_name;      /* intern handle */
    uint32_t session_id;
    uint32_t thread_count;
    uint32_t handle_count;
    uint32_t page_faults;
    uint32_t _pad;
    uint64_t working_set;     /* bytes */
    uint64_t virtual_size;    /* bytes */
    uint64_t cpu_100ns;       /* kernel + user time, 100ns units */
    uint64_t io_read_bytes;
    uint64_t io_write_bytes;
    uint64_t io_other_bytes;
    uint64_t create_time;     /* 100ns since 1601 */
    uint16_t cpu_pct;         /* fixed point, value/10 = percent (235 = 23.5%) */
    uint16_t _pad2;
    uint32_t _pad3;
    uint64_t etw_disk_total;  /* cumulative bytes, read + write, from ETW DiskIo */
    uint64_t etw_net_total;   /* cumulative bytes, in + out, from ETW TcpIp */
    uint32_t disk_bps;        /* bytes/second since last sample */
    uint32_t net_bps;
} process_t;

typedef struct {
    uint64_t   timestamp;     /* GetTickCount64 at sample */
    uint32_t   n_processes;
    uint32_t   _pad;
    process_t *processes;     /* flat array in the same arena */
} snapshot_t;

snapshot_t *snapshot_build(arena_t *dest, arena_t *scratch);

/* Sampler. Spawns one background thread that wakes every `period_ms` ms,
   builds a fresh snapshot, and publishes it via atomic pointer swap.
   notify is invalidated after each successful publish so the UI repaints. */
void              snapshot_start(HWND notify, unsigned period_ms);
void              snapshot_stop(void);
const snapshot_t *snapshot_current(void);   /* atomic load, may be NULL early */

#ifdef __cplusplus
}
#endif
