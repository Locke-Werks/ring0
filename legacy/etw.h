#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t disk_read;
    uint64_t disk_write;
    uint64_t net_in;
    uint64_t net_out;
} etw_counters_t;

void etw_start(void);
void etw_stop(void);
void etw_counters_for(unsigned pid, etw_counters_t *out);
int  etw_is_running(void);

#ifdef __cplusplus
}
#endif
