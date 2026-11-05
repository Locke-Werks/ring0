#pragma once
#include <stddef.h>

typedef struct ringbuf ringbuf_t;

/* Single-producer single-consumer lock-free ring. Capacity is rounded up to the
   next power of two. elem_size is fixed at creation. Both producer and consumer
   paths are wait-free on x64 (TSO memory model makes volatile load/store acquire
   /release for the head/tail fields). */
ringbuf_t *ringbuf_create(size_t capacity, size_t elem_size);
void       ringbuf_destroy(ringbuf_t *r);

int        ringbuf_push(ringbuf_t *r, const void *src);  /* 0 ok, -1 full */
int        ringbuf_pop (ringbuf_t *r, void       *dst);  /* 0 ok, -1 empty */
size_t     ringbuf_count(const ringbuf_t *r);

#ifdef DEBUG
void       ringbuf_selftest(void);
#endif
