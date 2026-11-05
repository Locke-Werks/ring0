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

/* Call once per sample tick to update per-engine deltas. */
void     gpu_sample(uint64_t tick_ms);

/* GPU memory — DXGI adapter local + shared budgets. */
typedef struct {
    uint64_t dedicated_used;
    uint64_t dedicated_total;
    uint64_t shared_used;
    uint64_t shared_total;
} gpu_mem_info_t;
int  gpu_query_memory(gpu_mem_info_t *out);

/* GPU temperature in Celsius, -1 if unavailable (NVAPI/vendor). */
int  gpu_query_temp(void);

/* Adapter description (e.g. "NVIDIA GeForce RTX 4090"). */
const wchar_t *gpu_adapter_name(void);

/* Per-engine utilisation — updated by gpu_sample(). */
#define GPU_ENGINE_MAX 8
int            gpu_engine_count(void);
const wchar_t *gpu_engine_name(int idx);
uint16_t       gpu_engine_pct(int idx);   /* fixed-point /10 */

#ifdef __cplusplus
}
#endif
