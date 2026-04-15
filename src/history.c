#include "history.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* Per-pid rolling CPU history. Open-addressed hash table; pid+create_time keys
   so reused PIDs don't smear together. Updated by the sampler thread each tick
   and read by the UI thread for sparkline rendering — for now we accept the
   reader-during-write race because each sample is uint16 and worst case the
   sparkline shows one stale point. */

#define SLOTS 1024u

struct slot {
    unsigned pid;
    uint64_t create_time;
    uint64_t last_seen;
    uint16_t samples[HIST_SAMPLES];
    uint16_t head;            /* index of next slot to write */
    uint16_t count;           /* 0..HIST_SAMPLES */
};

static struct slot *g_slots;

void history_init(void)
{
    if (g_slots) return;
    g_slots = (struct slot *)VirtualAlloc(NULL, sizeof(struct slot) * SLOTS,
                                          MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
}

void history_destroy(void)
{
    if (g_slots) { VirtualFree(g_slots, 0, MEM_RELEASE); g_slots = NULL; }
}

static struct slot *find_or_insert(unsigned pid, uint64_t create_time, uint64_t tick)
{
    if (!g_slots) return NULL;
    /* hash on pid (top bits of create_time scrambled in for variety) */
    unsigned h = (unsigned)(pid * 2654435761u) ^ (unsigned)(create_time >> 8);
    h &= (SLOTS - 1);

    struct slot *first_empty = NULL;
    struct slot *first_stale = NULL;
    for (unsigned i = 0; i < SLOTS; ++i) {
        struct slot *s = &g_slots[(h + i) & (SLOTS - 1)];
        if (s->pid == 0 && s->last_seen == 0) {
            if (!first_empty) first_empty = s;
            break;       /* probe stops at first empty */
        }
        if (s->pid == pid && s->create_time == create_time) return s;
        if (!first_stale && tick - s->last_seen > 60000ull) first_stale = s;
    }
    struct slot *take = first_empty ? first_empty : first_stale;
    if (!take) return NULL;
    take->pid         = pid;
    take->create_time = create_time;
    take->head        = 0;
    take->count       = 0;
    return take;
}

void history_update(unsigned pid, uint64_t create_time,
                    uint16_t cpu_pct, uint64_t tick_ms)
{
    struct slot *s = find_or_insert(pid, create_time, tick_ms);
    if (!s) return;
    s->samples[s->head] = cpu_pct;
    s->head = (uint16_t)((s->head + 1) % HIST_SAMPLES);
    if (s->count < HIST_SAMPLES) ++s->count;
    s->last_seen = tick_ms;
}

void history_age(uint64_t tick_ms, uint64_t max_age_ms)
{
    if (!g_slots) return;
    for (unsigned i = 0; i < SLOTS; ++i) {
        struct slot *s = &g_slots[i];
        if (s->pid && tick_ms - s->last_seen > max_age_ms) {
            s->pid = 0;
            s->create_time = 0;
            s->last_seen = 0;
            s->head = s->count = 0;
        }
    }
}

int history_copy(unsigned pid, uint64_t create_time, uint16_t *out, int max)
{
    if (!g_slots || !out || max <= 0) return 0;
    unsigned h = (unsigned)(pid * 2654435761u) ^ (unsigned)(create_time >> 8);
    h &= (SLOTS - 1);
    for (unsigned i = 0; i < SLOTS; ++i) {
        struct slot *s = &g_slots[(h + i) & (SLOTS - 1)];
        if (s->pid == 0 && s->last_seen == 0) return 0;
        if (s->pid != pid || s->create_time != create_time) continue;

        int n = s->count < max ? s->count : max;
        /* oldest sample is at (head - count) mod N */
        int start = (int)s->head - (int)s->count;
        if (start < 0) start += HIST_SAMPLES;
        for (int j = 0; j < n; ++j)
            out[j] = s->samples[(start + j) % HIST_SAMPLES];
        return n;
    }
    return 0;
}
