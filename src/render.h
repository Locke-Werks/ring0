#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "snapshot.h"

#ifdef __cplusplus
extern "C" {
#endif

void render_init(HWND hwnd);
void render_resize(unsigned w, unsigned h);
void render_frame(const snapshot_t *snap);
void render_destroy(void);

/* When mica is on the client area should clear to transparent so the dwm
   backdrop shows through; otherwise we paint the theme background color. */
void render_set_transparent(BOOL t);

#ifdef __cplusplus
}
#endif
