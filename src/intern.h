#pragma once
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One global interner. Handles are 32-bit offsets into an append-only UTF-16
   pool; 0 means "nothing." Same string always returns the same handle, so
   equality comparisons across snapshots reduce to integer compare. */

void           intern_init(void);
uint32_t       intern_str(const wchar_t *s, size_t len);
const wchar_t *intern_get(uint32_t h);            /* NULL if h == 0 */
size_t         intern_len(uint32_t h);            /* in wchars, 0 if h == 0 */

#ifdef DEBUG
void           intern_selftest(void);
#endif

#ifdef __cplusplus
}
#endif
