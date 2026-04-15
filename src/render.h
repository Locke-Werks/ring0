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

/* Selection highlight by PID — survives snapshot churn. */
void render_set_selected(unsigned pid);

/* Layout introspection for hit testing in the message handler. */
void render_get_data_layout(int *top_y, int *row_h);

/* Scroll offset in whole rows. Render clamps internally. */
void render_set_scroll(int rows);
int  render_get_scroll(void);
int  render_get_visible_rows(void);

/* Header hit testing + sort. */
void render_get_header_layout(int *top_y, int *h);
int  render_col_at_x(int x);          /* -1 if outside any column */
void render_toggle_sort(int col);     /* same col flips dir, new col resets to descending */

/* Density: -1 compact, 0 normal, +1 comfortable. delta is signed step. */
void render_cycle_density(int delta);

#ifdef __cplusplus
}
#endif
