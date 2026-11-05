#include "arena.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>

/* Arena headers live in a fixed static pool rather than an allocation somewhere,
   because the build forbids HeapAlloc and we don't want to burn a whole page of
   committed VA on each header. 256 slots is plenty — we expect at most ~240 live
   snapshot arenas plus a handful of permanent ones. */
#define ARENA_MAX 256

struct arena {
    uint8_t *base;
    size_t   reserved;
    size_t   committed;
    size_t   used;
};

static struct arena g_slots[ARENA_MAX];
static uint8_t      g_used[ARENA_MAX];

static size_t align_up(size_t v, size_t a) { return (v + (a - 1)) & ~(a - 1); }

static arena_t *slot_alloc(void)
{
    for (int i = 0; i < ARENA_MAX; ++i) {
        if (!g_used[i]) { g_used[i] = 1; return &g_slots[i]; }
    }
    return NULL;
}

static void slot_free(arena_t *a)
{
    size_t i = (size_t)(a - g_slots);
    if (i < ARENA_MAX) g_used[i] = 0;
}

#define INITIAL_COMMIT (64 * 1024)

arena_t *arena_create(size_t reserve)
{
    arena_t *a = slot_alloc();
    if (!a) return NULL;

    reserve = align_up(reserve, 64 * 1024);
    uint8_t *base = VirtualAlloc(NULL, reserve, MEM_RESERVE, PAGE_READWRITE);
    if (!base) { slot_free(a); return NULL; }

    size_t initial = INITIAL_COMMIT < reserve ? INITIAL_COMMIT : reserve;
    if (!VirtualAlloc(base, initial, MEM_COMMIT, PAGE_READWRITE)) {
        VirtualFree(base, 0, MEM_RELEASE);
        slot_free(a);
        return NULL;
    }

    a->base      = base;
    a->reserved  = reserve;
    a->committed = initial;
    a->used      = 0;
    return a;
}

void *arena_alloc(arena_t *a, size_t n, size_t align)
{
    if (align < 1) align = 1;
    size_t start = align_up(a->used, align);
    size_t end   = start + n;
    if (end > a->reserved) return NULL;

    if (end > a->committed) {
        size_t want = a->committed;
        while (want < end) want *= 2;
        if (want > a->reserved) want = a->reserved;
        size_t delta = want - a->committed;
        if (!VirtualAlloc(a->base + a->committed, delta, MEM_COMMIT, PAGE_READWRITE))
            return NULL;
        a->committed = want;
    }

    a->used = end;
    return a->base + start;
}

void arena_reset(arena_t *a) { a->used = 0; }

void arena_destroy(arena_t *a)
{
    if (!a) return;
    VirtualFree(a->base, 0, MEM_RELEASE);
    a->base = NULL;
    slot_free(a);
}

size_t arena_used(const arena_t *a) { return a->used; }

#ifdef DEBUG
static void die(const char *msg) { (void)msg; __debugbreak(); ExitProcess(0xA1DEAD); }

void arena_selftest(void)
{
    arena_t *a = arena_create(1 << 20);
    if (!a) die("arena_create");

    uint8_t *p1 = arena_alloc(a, 16, 16);
    uint8_t *p2 = arena_alloc(a, 16, 16);
    if (!p1 || !p2 || p2 <= p1)        die("bump");
    if (((uintptr_t)p1 & 15) != 0)     die("align16");
    if ((size_t)(p2 - p1) != 16)       die("contig");

    /* force a commit growth */
    void *big = arena_alloc(a, 200 * 1024, 8);
    if (!big) die("grow");

    arena_reset(a);
    if (arena_used(a) != 0)            die("reset");

    uint8_t *p3 = arena_alloc(a, 16, 16);
    if (p3 != p1)                      die("reset_ptr");

    arena_destroy(a);
}
#endif
