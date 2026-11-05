#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HIST_SAMPLES 120   /* ~30s @ 250ms */

void history_init(void);
void history_destroy(void);

/* Append a cpu sample for (pid, create_time). Inserts if new, evicts oldest
   stale slot if table is full. Marks slot live at this tick. */
void history_update(unsigned pid, uint64_t create_time,
                    uint16_t cpu_pct, uint64_t tick_ms);

/* Drop slots whose last_seen tick is older than `tick_ms - max_age_ms`. */
void history_age(uint64_t tick_ms, uint64_t max_age_ms);

/* Copy up to `max` most-recent samples into `out` (oldest first).
   Returns count copied. 0 if pid not tracked. */
int  history_copy(unsigned pid, uint64_t create_time, uint16_t *out, int max);

#ifdef __cplusplus
}
#endif
