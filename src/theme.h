#pragma once
#include <windows.h>

typedef struct {
    BOOL     dark;
    COLORREF background;
    COLORREF foreground;
    COLORREF accent;
    COLORREF subtle;
} theme_t;

void           theme_load(void);
const theme_t *theme_current(void);
