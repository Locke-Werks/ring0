#include "ringbuf.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <string.h>

struct ringbuf {
    size_t   elem_size;
    size_t   mask;          /* capacity - 1 */
    size_t   capacity;
    /* head and tail are free-running 64-bit counters so (head - tail) is the
       current count even across wraparound. */
    volatile LONG64 head;   /* producer writes */
    volatile LONG64 tail;   /* consumer reads */
};

static size_t pow2_up(size_t n)
{
    size_t p = 1;
    while (p < n) p <<= 1;
    return p;
}

static size_t align_up(size_t v, size_t a) { return (v + a - 1) & ~(a - 1); }

ringbuf_t *ringbuf_create(size_t capacity, size_t elem_size)
{
    if (capacity == 0 || elem_size == 0) return NULL;

    size_t cap   = pow2_up(capacity);
    size_t total = align_up(sizeof(struct ringbuf), 64) + cap * elem_size;
    total        = align_up(total, 4096);

    struct ringbuf *r = VirtualAlloc(NULL, total, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!r) return NULL;

    r->elem_size = elem_size;
    r->mask      = cap - 1;
    r->capacity  = cap;
    r->head      = 0;
    r->tail      = 0;
    return r;
}

void ringbuf_destroy(ringbuf_t *r)
{
    if (r) VirtualFree(r, 0, MEM_RELEASE);
}

static uint8_t *rb_data(ringbuf_t *r)
{
    return (uint8_t *)r + align_up(sizeof(struct ringbuf), 64);
}

int ringbuf_push(ringbuf_t *r, const void *src)
{
    LONG64 h = r->head;
    LONG64 t = r->tail;
    if ((size_t)(h - t) >= r->capacity) return -1;

    memcpy(rb_data(r) + ((size_t)h & r->mask) * r->elem_size, src, r->elem_size);
    /* publish */
    r->head = h + 1;
    return 0;
}

int ringbuf_pop(ringbuf_t *r, void *dst)
{
    LONG64 t = r->tail;
    LONG64 h = r->head;
    if (h == t) return -1;

    memcpy(dst, rb_data(r) + ((size_t)t & r->mask) * r->elem_size, r->elem_size);
    r->tail = t + 1;
    return 0;
}

size_t ringbuf_count(const ringbuf_t *r)
{
    LONG64 h = r->head;
    LONG64 t = r->tail;
    return (size_t)(h - t);
}

#ifdef DEBUG
static void die(void) { __debugbreak(); ExitProcess(0xB1DEAD); }

void ringbuf_selftest(void)
{
    ringbuf_t *r = ringbuf_create(4, sizeof(int));
    if (!r) die();

    int v;
    if (ringbuf_pop(r, &v) != -1) die();               /* empty */
    for (int i = 0; i < 4; ++i) if (ringbuf_push(r, &i) != 0) die();
    int x = 99;
    if (ringbuf_push(r, &x) != -1) die();              /* full */
    if (ringbuf_count(r) != 4) die();

    for (int i = 0; i < 4; ++i) {
        if (ringbuf_pop(r, &v) != 0) die();
        if (v != i) die();
    }
    if (ringbuf_count(r) != 0) die();

    /* wrap */
    for (int i = 0; i < 10; ++i) {
        int in = i * 7;
        if (ringbuf_push(r, &in) != 0) die();
        int out;
        if (ringbuf_pop(r, &out) != 0) die();
        if (out != in) die();
    }

    ringbuf_destroy(r);
}
#endif
