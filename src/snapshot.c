#include "snapshot.h"
#include "ntapi.h"
#include "intern.h"
#include "history.h"
#include "etw.h"
#include "gpu.h"
#include "physdisk.h"
#include "netif.h"
#include <stdint.h>

/* Snapshot arena pool — sized so the time scrubber can show a full minute
   of history at the default 250ms cadence (240 = 60 / 0.25). */
#define POOL_N 240

static arena_t          *g_scratch;
static arena_t          *g_pool[POOL_N];
static snapshot_t       *g_published[POOL_N];   /* chronological pointers */
static int               g_pub_head;            /* index of most recent in g_published */
static int               g_pub_count;           /* valid entries, saturates at POOL_N */
static unsigned          g_next;                /* next pool slot to recycle */
static volatile snapshot_t *g_current;
static const snapshot_t *g_prev_for_cpu;    /* used to compute cpu_pct deltas */
static HANDLE            g_exit_event;
static HANDLE            g_thread;
static HWND              g_notify;
static unsigned          g_period_ms;
static unsigned          g_num_cores;

/* Per-core perf history for delta calculations. */
static LARGE_INTEGER     g_prev_idle[64];
static LARGE_INTEGER     g_prev_kern[64];
static LARGE_INTEGER     g_prev_user[64];
static int               g_have_core_prev;

static void fill_per_core(snapshot_t *cur)
{
    SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION_R0 info[64];
    ULONG ret = 0;
    if (!nt_qsi) return;
    NTSTATUS st = nt_qsi(SystemProcessorPerformanceInformation,
                         info, sizeof info, &ret);
    if (st != 0) return;
    int n = (int)(ret / sizeof(SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION_R0));
    if (n > 64) n = 64;
    cur->cpu_core_count = (uint8_t)n;

    for (int i = 0; i < n; ++i) {
        if (g_have_core_prev) {
            uint64_t didle = (uint64_t)(info[i].IdleTime.QuadPart  - g_prev_idle[i].QuadPart);
            uint64_t dkern = (uint64_t)(info[i].KernelTime.QuadPart - g_prev_kern[i].QuadPart);
            uint64_t duser = (uint64_t)(info[i].UserTime.QuadPart  - g_prev_user[i].QuadPart);
            uint64_t total = dkern + duser;          /* KernelTime includes idle */
            uint64_t busy  = total > didle ? total - didle : 0;
            uint64_t pct   = total > 0 ? (busy * 100 / total) : 0;
            if (pct > 100) pct = 100;
            cur->cpu_core_pct[i] = (uint8_t)pct;
        } else {
            cur->cpu_core_pct[i] = 0;
        }
        g_prev_idle[i] = info[i].IdleTime;
        g_prev_kern[i] = info[i].KernelTime;
        g_prev_user[i] = info[i].UserTime;
    }
    g_have_core_prev = 1;
}

/* Per-pid image-path cache. The exe path is invariant for the life of a PID,
   so resolving it once via QueryFullProcessImageNameW saves N OpenProcess calls
   per sample. Key is (pid, create_time) to handle PID reuse. */
struct path_slot {
    unsigned pid;
    uint64_t create_time;
    uint32_t path;            /* intern handle */
    uint32_t flags;
};
#define PATH_SLOTS 1024
static struct path_slot *g_paths;        /* VirtualAlloc'd lazily */

static void paths_ensure(void)
{
    if (g_paths) return;
    g_paths = (struct path_slot *)VirtualAlloc(NULL,
        sizeof(struct path_slot) * PATH_SLOTS,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
}

static uint32_t resolve_image_path(unsigned pid, uint64_t create_time)
{
    if (pid == 0) return 0;
    paths_ensure();
    if (!g_paths) return 0;
    unsigned h = (pid * 2654435761u) & (PATH_SLOTS - 1);
    struct path_slot *empty = NULL;
    for (unsigned i = 0; i < PATH_SLOTS; ++i) {
        struct path_slot *s = &g_paths[(h + i) & (PATH_SLOTS - 1)];
        if (s->pid == pid && s->create_time == create_time) {
            return s->path;       /* may be 0 if access denied */
        }
        if (s->pid == 0 && !empty) empty = s;
        if (s->pid == 0) break;
    }
    if (!empty) return 0;

    /* attempt the query; cache the outcome either way so we don't retry */
    HANDLE ph = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    uint32_t handle = 0;
    if (ph) {
        WCHAR buf[MAX_PATH];
        DWORD sz = MAX_PATH;
        if (QueryFullProcessImageNameW(ph, 0, buf, &sz) && sz > 0) {
            handle = intern_str(buf, sz);
        }
        CloseHandle(ph);
    }
    empty->pid = pid;
    empty->create_time = create_time;
    empty->path = handle;
    empty->flags = 1;
    return handle;
}

static void path_cache_age(const snapshot_t *snap)
{
    if (!g_paths) return;
    /* keep slots only for pids that exist in the latest snapshot */
    static uint64_t last_age_ms;
    if (snap->timestamp - last_age_ms < 5000) return;
    last_age_ms = snap->timestamp;

    for (unsigned i = 0; i < PATH_SLOTS; ++i) {
        struct path_slot *s = &g_paths[i];
        if (s->pid == 0) continue;
        int alive = 0;
        for (uint32_t j = 0; j < snap->n_processes; ++j) {
            if (snap->processes[j].pid == s->pid &&
                snap->processes[j].create_time == s->create_time) {
                alive = 1;
                break;
            }
        }
        if (!alive) {
            s->pid = 0;
            s->create_time = 0;
            s->path = 0;
        }
    }
}

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
        pr->cpu_pct       = 0;
        pr->_pad2         = 0;
        pr->_pad3         = 0;

        etw_counters_t ec;
        etw_counters_for(pr->pid, &ec);
        pr->etw_disk_total = ec.disk_read + ec.disk_write;
        pr->etw_net_total  = ec.net_in    + ec.net_out;
        pr->disk_bps       = 0;
        pr->net_bps        = 0;
        pr->image_path     = resolve_image_path(pr->pid, pr->create_time);
        pr->gpu_pct        = 0;
        pr->_pad4          = 0;
        pr->gpu_runtime_100ns = gpu_runtime_100ns(pr->pid);

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

/* Walk the new and previous process arrays and fill cpu_pct + disk/net rates
   on cur as normalized deltas. O(n*m) is fine at our process counts; revisit
   if it ever becomes hot. */
static void fill_deltas(snapshot_t *cur, const snapshot_t *prev)
{
    cur->agg_cpu_pct = 0;
    cur->agg_gpu_pct = 0;
    cur->agg_disk_bps = 0;
    cur->agg_net_bps = 0;

    if (!prev || !g_num_cores) return;
    uint64_t dwall_ms = cur->timestamp - prev->timestamp;
    if (dwall_ms == 0) return;

    for (uint32_t i = 0; i < cur->n_processes; ++i) {
        process_t *c = &cur->processes[i];
        for (uint32_t j = 0; j < prev->n_processes; ++j) {
            const process_t *pp = &prev->processes[j];
            if (pp->pid != c->pid || pp->create_time != c->create_time) continue;

            if (c->cpu_100ns >= pp->cpu_100ns) {
                uint64_t dcpu_ms = (c->cpu_100ns - pp->cpu_100ns) / 10000;
                uint64_t pct_x10 = (dcpu_ms * 1000) / (dwall_ms * g_num_cores);
                c->cpu_pct = pct_x10 > 65535 ? 65535 : (uint16_t)pct_x10;
            }
            if (c->etw_disk_total >= pp->etw_disk_total) {
                uint64_t bps = (c->etw_disk_total - pp->etw_disk_total) * 1000 / dwall_ms;
                c->disk_bps = bps > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)bps;
            }
            if (c->etw_net_total >= pp->etw_net_total) {
                uint64_t bps = (c->etw_net_total - pp->etw_net_total) * 1000 / dwall_ms;
                c->net_bps = bps > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)bps;
            }
            if (c->gpu_runtime_100ns >= pp->gpu_runtime_100ns) {
                uint64_t dgpu_ms = (c->gpu_runtime_100ns - pp->gpu_runtime_100ns) / 10000;
                uint64_t pct_x10 = (dgpu_ms * 1000) / dwall_ms;
                c->gpu_pct = pct_x10 > 65535 ? 65535 : (uint16_t)pct_x10;
            }
            break;
        }
    }

    /* roll-up totals for the scrubber strip */
    for (uint32_t i = 0; i < cur->n_processes; ++i) {
        const process_t *c = &cur->processes[i];
        cur->agg_cpu_pct  += c->cpu_pct;
        cur->agg_gpu_pct  += c->gpu_pct;
        cur->agg_disk_bps += c->disk_bps;
        cur->agg_net_bps  += c->net_bps;
    }
}

static void take_sample(void)
{
    arena_t *dest = g_pool[g_next];
    arena_reset(dest);
    snapshot_t *snap = snapshot_build(dest, g_scratch);
    if (!snap) return;

    fill_deltas(snap, g_prev_for_cpu);
    fill_per_core(snap);
    snap->n_disks  = (uint8_t)physdisk_sample(snap->disks,  PHYS_DISK_MAX, snap->timestamp);
    snap->n_ifaces = (uint8_t)netif_sample(snap->ifaces, NETIF_MAX, snap->timestamp);
    g_prev_for_cpu = snap;

    /* roll cpu samples into per-pid history for sparklines */
    for (uint32_t i = 0; i < snap->n_processes; ++i) {
        const process_t *p = &snap->processes[i];
        history_update(p->pid, p->create_time, p->cpu_pct, snap->timestamp);
    }
    history_age(snap->timestamp, 60000);
    path_cache_age(snap);

    g_published[g_next] = snap;
    g_pub_head = (int)g_next;
    if (g_pub_count < POOL_N) ++g_pub_count;

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
    history_init();

    g_notify    = notify;
    g_period_ms = period_ms ? period_ms : 250;

    SYSTEM_INFO si;
    GetSystemInfo(&si);
    g_num_cores = si.dwNumberOfProcessors;
    if (g_num_cores == 0) g_num_cores = 1;

    gpu_init();

    g_scratch = arena_create(8 * 1024 * 1024);
    /* 512 KiB reserve per pool slot is plenty: a snapshot of ~400 processes
       fits in ~40 KiB. With POOL_N=240 the total VA reserve is ~120 MiB and
       committed memory tracks actual snapshot size. */
    for (int i = 0; i < POOL_N; ++i) g_pool[i] = arena_create(512 * 1024);

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
    g_prev_for_cpu = NULL;
    g_pub_count = 0;
    g_pub_head = 0;
    for (int i = 0; i < POOL_N; ++i) g_published[i] = NULL;
    history_destroy();
    gpu_destroy();
}

const snapshot_t *snapshot_current(void)
{
    return (const snapshot_t *)g_current;
}

const snapshot_t *snapshot_at(int back)
{
    if (back < 0 || back >= g_pub_count) return NULL;
    int idx = (g_pub_head - back + POOL_N) % POOL_N;
    return g_published[idx];
}

int snapshot_count(void) { return g_pub_count; }
