#pragma once
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    BOOL     dark;
    COLORREF background;
    COLORREF foreground;
    COLORREF accent;
    COLORREF subtle;
} theme_t;

void           theme_load(void);
const theme_t *theme_current(void);

#ifdef __cplusplus
}
#endif
