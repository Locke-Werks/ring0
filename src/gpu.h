#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void     gpu_init(void);
void     gpu_destroy(void);
/* Returns total GPU runtime in 100-ns units across all adapters and engines
   for the given pid. 0 if unavailable / access denied. */
uint64_t gpu_runtime_100ns(unsigned pid);

#ifdef __cplusplus
}
#endif
