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

#ifdef DEBUG
void filter_selftest(void);
#endif

#ifdef __cplusplus
}
#endif
