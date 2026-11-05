#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Fuzzy subsequence match. Returns a positive score on match (higher = better),
   0 for an empty pattern (match everything), negative if pattern can't be found
   as a subsequence of haystack. Case-insensitive ASCII; non-ASCII is compared
   by raw wchar value. */
int filter_score(const wchar_t *pattern, const wchar_t *haystack);

/* Parsed query combining prefix operators and fuzzy name match. Build from the
   raw input once per frame, then hit each row with filter_query_match. */
typedef struct {
    unsigned pid;
    unsigned session;
    int      has_pid;
    int      has_session;
    int      name_len;
    wchar_t  name[256];
} filter_query_t;

void filter_parse(const wchar_t *input, filter_query_t *out);
int  filter_query_match(const filter_query_t *q,
                        unsigned pid, unsigned session,
                        const wchar_t *name);

#ifdef DEBUG
void filter_selftest(void);
#endif

#ifdef __cplusplus
}
#endif
