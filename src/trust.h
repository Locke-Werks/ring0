#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    TRUST_UNKNOWN  = 0,
    TRUST_SIGNED   = 1,
    TRUST_UNSIGNED = 2,
    TRUST_BAD      = 3,
};

void    trust_start(void);
void    trust_stop(void);
/* Returns TRUST_UNKNOWN until the worker has gotten to this path. */
uint8_t trust_query(uint32_t image_path_intern);

#ifdef __cplusplus
}
#endif
