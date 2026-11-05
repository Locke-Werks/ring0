#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Sample stacks of every thread in `pid` once and aggregate by frame.
   Returns the number of unique frames captured, or 0 on failure. Result is
   exposed through render's g_stack_frames table. */
int  stack_capture(unsigned pid);
void stack_clear(void);

#ifdef __cplusplus
}
#endif
