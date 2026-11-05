#include "physdisk.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>

/* IOCTL_DISK_PERFORMANCE works on \\.\PhysicalDriveN without elevation and
   without GENERIC_READ/GENERIC_WRITE — dwDesiredAccess=0 is the standard idiom
   for metadata-only disk IOCTLs. Counters are enabled by default on Win10+. */

typedef struct {
    HANDLE   h;           /* NULL = not tried yet, INVALID_HANDLE_VALUE = absent */
    uint64_t prev_read;
    uint64_t prev_write;
    uint64_t prev_ms;
} slot_t;

static slot_t g_slots[PHYS_DISK_MAX];

static HANDLE open_disk(int n)
{
    /* build "\\.\PhysicalDriveN". Two digits handled for completeness. */
    wchar_t path[32];
    const wchar_t prefix[] = L"\\\\.\\PhysicalDrive";
    int i = 0;
    for (; prefix[i]; ++i) path[i] = prefix[i];
    if (n >= 10) { path[i++] = (wchar_t)(L'0' + (n / 10)); n %= 10; }
    path[i++] = (wchar_t)(L'0' + n);
    path[i] = 0;
    return CreateFileW(path, 0,
                       FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                       OPEN_EXISTING, 0, NULL);
}

int physdisk_sample(phys_disk_t *out, int max, uint64_t now_ms)
{
    int n = 0;
    for (int i = 0; i < PHYS_DISK_MAX && n < max; ++i) {
        slot_t *s = &g_slots[i];
        if (s->h == NULL) {
            s->h = open_disk(i);
        }
        if (s->h == INVALID_HANDLE_VALUE) continue;

        DISK_PERFORMANCE dp; dp.BytesRead.QuadPart = 0;
        DWORD ret = 0;
        if (!DeviceIoControl(s->h, IOCTL_DISK_PERFORMANCE, NULL, 0,
                             &dp, sizeof dp, &ret, NULL)) {
            continue;
        }

        phys_disk_t *o = &out[n++];
        const wchar_t nm[] = L"PhysicalDrive";
        int k = 0;
        for (; nm[k]; ++k) o->name[k] = nm[k];
        int idx = i;
        if (idx >= 10) { o->name[k++] = (wchar_t)(L'0' + (idx / 10)); idx %= 10; }
        o->name[k++] = (wchar_t)(L'0' + idx);
        o->name[k]   = 0;

        o->read_bytes  = (uint64_t)dp.BytesRead.QuadPart;
        o->write_bytes = (uint64_t)dp.BytesWritten.QuadPart;
        o->queue_depth = dp.QueueDepth;
        o->read_bps    = 0;
        o->write_bps   = 0;
        o->_pad        = 0;

        if (s->prev_ms > 0 && now_ms > s->prev_ms) {
            uint64_t dwall = now_ms - s->prev_ms;
            if (o->read_bytes >= s->prev_read) {
                uint64_t bps = (o->read_bytes - s->prev_read) * 1000 / dwall;
                o->read_bps = bps > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)bps;
            }
            if (o->write_bytes >= s->prev_write) {
                uint64_t bps = (o->write_bytes - s->prev_write) * 1000 / dwall;
                o->write_bps = bps > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)bps;
            }
        }
        s->prev_read  = o->read_bytes;
        s->prev_write = o->write_bytes;
        s->prev_ms    = now_ms;
    }
    return n;
}
