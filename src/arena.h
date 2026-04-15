#pragma once
#include <stddef.h>

typedef struct arena arena_t;

arena_t *arena_create(size_t reserve);
void    *arena_alloc(arena_t *a, size_t n, size_t align);
void     arena_reset(arena_t *a);
void     arena_destroy(arena_t *a);
size_t   arena_used(const arena_t *a);

#ifdef DEBUG
void     arena_selftest(void);
#endif
