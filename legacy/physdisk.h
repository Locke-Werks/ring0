#pragma once
#include <stdint.h>

#define PHYS_DISK_MAX 8

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    wchar_t  name[24];        /* "PhysicalDrive0" */
    uint64_t read_bytes;      /* cumulative, from IOCTL_DISK_PERFORMANCE */
    uint64_t write_bytes;
    uint32_t read_bps;        /* delta since last sample / wall_ms */
    uint32_t write_bps;
    uint32_t queue_depth;
    uint32_t _pad;
} phys_disk_t;

/* Opens \\.\PhysicalDriveN for N=0..PHYS_DISK_MAX-1, queries IOCTL_DISK_PERFORMANCE,
   and fills `out`. Returns number of disks successfully queried. Caches handles
   so the per-call cost is just N IOCTLs. `now_ms` is GetTickCount64() at sample. */
int physdisk_sample(phys_disk_t *out, int max, uint64_t now_ms);

#ifdef __cplusplus
}
#endif
