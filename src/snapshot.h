#pragma once
#include <stdint.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "arena.h"
#include "physdisk.h"
#include "netif.h"

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
    uint32_t image_path;      /* intern handle, full DOS path; 0 if not resolved */
    uint16_t gpu_pct;         /* fixed point, value/10 = percent */
    uint16_t _pad4;
    uint64_t gpu_runtime_100ns;  /* cumulative across adapters + engines */
} process_t;

typedef struct {
    uint64_t phys_total;
    uint64_t phys_avail;
    uint64_t commit_limit;
    uint64_t commit_used;
    uint64_t paged_pool;
    uint64_t non_paged_pool;
    uint32_t page_faults_per_sec;
    uint32_t mem_load_pct;
} mem_stats_t;

typedef struct {
    uint64_t   timestamp;     /* GetTickCount64 at sample */
    uint32_t   n_processes;
    uint32_t   _pad;
    mem_stats_t mem;
    /* aggregate roll-ups for the time scrubber strip — sum across processes */
    uint32_t   agg_cpu_pct;   /* fixed point, value/10 = percent */
    uint32_t   agg_gpu_pct;
    uint64_t   agg_disk_bps;
    uint64_t   agg_net_bps;
    /* per-logical-core utilisation (0..100). Capped at 64 cores; if the
       machine has more, only the first 64 are reported. */
    uint8_t    cpu_core_count;
    uint8_t    _pad2[3];
    uint8_t    cpu_core_pct[64];       /* total busy */
    uint8_t    cpu_core_user_pct[64];
    uint8_t    cpu_core_kern_pct[64];  /* kernel, excluding idle */
    uint8_t    n_disks;
    uint8_t    n_ifaces;
    uint8_t    _pad3[6];
    phys_disk_t disks[PHYS_DISK_MAX];
    net_if_t   ifaces[NETIF_MAX];
    process_t *processes;     /* flat array in the same arena */
} snapshot_t;

snapshot_t *snapshot_build(arena_t *dest, arena_t *scratch);

/* Sampler. Spawns one background thread that wakes every `period_ms` ms,
   builds a fresh snapshot, and publishes it via atomic pointer swap.
   notify is invalidated after each successful publish so the UI repaints. */
void              snapshot_start(HWND notify, unsigned period_ms);
void              snapshot_stop(void);
const snapshot_t *snapshot_current(void);   /* atomic load, may be NULL early */

/* Historical access. back=0 is the most recent published snapshot. Returns
   NULL if back >= snapshot_count(). */
const snapshot_t *snapshot_at(int back);
int               snapshot_count(void);

#ifdef __cplusplus
}
#endif
