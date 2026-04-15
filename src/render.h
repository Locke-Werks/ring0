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

/* Command bar: filter text. Render makes a copy. caret is character offset. */
void render_set_filter(const wchar_t *text, int caret);

/* Scope pills. 0 = all, 1 = user (session != 0), 2 = system (session == 0). */
void render_set_scope(int scope);
int  render_hit_pill(int x, int y);   /* -1 if not on a pill */

/* Inspector side panel. */
void render_toggle_inspector(void);
void render_set_inspector_tab(int tab);   /* 0 modules, 1 network */
int  render_inspector_visible(void);
int  render_hit_tab(int x, int y);        /* -1 if not on an inspector tab */

/* Scrubber playhead. view_back == 0 means live (latest snapshot). */
void render_set_view_back(int back);
int  render_get_view_back(void);
int  render_hit_scrubber(int x, int y);   /* returns view_back for click x, or -1 */
void render_toggle_paused(void);
int  render_is_paused(void);

/* Snapshot diff. Pin current view as baseline; subsequent renders highlight
   newly-appeared processes. Calling again clears the diff. */
void render_toggle_diff(void);
int  render_diff_active(void);

#ifdef __cplusplus
}
#endif
