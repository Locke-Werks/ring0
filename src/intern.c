#include "intern.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <string.h>

/* Pool: append-only. Each entry is { u32 len; WCHAR s[len]; WCHAR 0; } padded
   to a 4-byte boundary. Handle == byte offset of the len field.
   Handle 0 is reserved for "null" — the first 4 bytes of the pool are a zeroed
   sentinel, so intern_get(0) can distinguish. */

#define POOL_BYTES (4u * 1024u * 1024u)   /* 4 MiB of interned string data */
#define TABLE_SLOTS 8192u                 /* power of 2, >> expected unique */

static uint8_t  *g_pool;
static uint32_t  g_pool_used;
static uint32_t *g_table;                 /* TABLE_SLOTS entries, each a handle */

static size_t align4(size_t v) { return (v + 3) & ~(size_t)3; }

static uint32_t fnv1a(const wchar_t *s, size_t len)
{
    uint32_t h = 2166136261u;
    const uint8_t *b = (const uint8_t *)s;
    size_t n = len * sizeof(wchar_t);
    for (size_t i = 0; i < n; ++i) {
        h ^= b[i];
        h *= 16777619u;
    }
    return h;
}

void intern_init(void)
{
    if (g_pool) return;
    g_pool  = VirtualAlloc(NULL, POOL_BYTES, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    g_table = VirtualAlloc(NULL, TABLE_SLOTS * sizeof(uint32_t),
                           MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    /* reserve offset 0 as the null sentinel so handle 0 stays "nothing" */
    g_pool_used = 4;
}

static int entry_equal(uint32_t h, const wchar_t *s, size_t len)
{
    uint32_t plen = *(uint32_t *)(g_pool + h);
    if (plen != len) return 0;
    const wchar_t *p = (const wchar_t *)(g_pool + h + 4);
    for (size_t i = 0; i < len; ++i) if (p[i] != s[i]) return 0;
    return 1;
}

uint32_t intern_str(const wchar_t *s, size_t len)
{
    if (!g_pool) intern_init();
    if (len == 0 || !s) return 0;

    uint32_t mask = TABLE_SLOTS - 1;
    uint32_t i    = fnv1a(s, len) & mask;

    for (;;) {
        uint32_t slot = g_table[i];
        if (slot == 0) break;
        if (entry_equal(slot, s, len)) return slot;
        i = (i + 1) & mask;
    }

    /* append to pool */
    size_t need = align4(4 + (len + 1) * sizeof(wchar_t));
    if (g_pool_used + need > POOL_BYTES) return 0;
    uint32_t handle = g_pool_used;
    *(uint32_t *)(g_pool + handle) = (uint32_t)len;
    wchar_t *dst = (wchar_t *)(g_pool + handle + 4);
    for (size_t j = 0; j < len; ++j) dst[j] = s[j];
    dst[len] = 0;
    g_pool_used += (uint32_t)need;

    g_table[i] = handle;
    return handle;
}

const wchar_t *intern_get(uint32_t h)
{
    if (h == 0 || !g_pool) return NULL;
    return (const wchar_t *)(g_pool + h + 4);
}

size_t intern_len(uint32_t h)
{
    if (h == 0 || !g_pool) return 0;
    return *(uint32_t *)(g_pool + h);
}

#ifdef DEBUG
static void die(void) { __debugbreak(); ExitProcess(0xC1DEAD); }

void intern_selftest(void)
{
    intern_init();

    uint32_t a = intern_str(L"hello", 5);
    uint32_t b = intern_str(L"hello", 5);
    uint32_t c = intern_str(L"world", 5);
    uint32_t d = intern_str(L"chrome.exe", 10);
    uint32_t e = intern_str(L"chrome.exe", 10);

    if (a == 0 || c == 0 || d == 0)      die();
    if (a != b)                           die();
    if (d != e)                           die();
    if (a == c)                           die();
    if (intern_get(0) != NULL)            die();
    if (intern_len(0) != 0)               die();

    const wchar_t *p = intern_get(a);
    if (!p || p[0] != L'h' || p[4] != L'o' || p[5] != 0) die();
    if (intern_len(a) != 5)               die();

    /* empty input is 0 */
    if (intern_str(L"", 0) != 0)          die();
    if (intern_str(NULL, 3) != 0)         die();
}
#endif
