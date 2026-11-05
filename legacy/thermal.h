#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void  thermal_init(void);
void  thermal_destroy(void);
void  thermal_sample(void);
/* CPU package temperature in Celsius. Returns -1.0f if unavailable. */
float thermal_cpu_temp(void);

#ifdef __cplusplus
}
#endif
