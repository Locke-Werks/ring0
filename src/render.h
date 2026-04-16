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
int  render_hit_tree_pill(int x, int y);   /* 1 if on the tree-mode toggle */

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

/* Stack tab: capture button hit-test. */
int  render_hit_capture_btn(int x, int y);

/* Process tree mode (Ctrl+G). When on, rows are reordered as a parent-child
   tree with disclosure prefixes; tree_toggle_collapse flips a pid's collapse
   state. */
void render_toggle_tree(void);
int  render_tree_mode(void);
void render_tree_toggle_collapse(unsigned pid);

/* Top-level resource views (Ctrl+1..7). */
enum {
    VIEW_PROCESSES = 0,
    VIEW_CPU       = 1,
    VIEW_MEMORY    = 2,
    VIEW_DISK      = 3,
    VIEW_NETWORK   = 4,
    VIEW_GPU       = 5,
    VIEW_FORENSIC  = 6,
    VIEW_COUNT     = 7,
};
void render_set_view(int v);
int  render_get_view(void);
int  render_hit_view_tab(int x, int y);   /* -1 if not on a top tab */
void render_set_hover(int x, int y);      /* update mouse position for hover highlight */

/* Stack frames pushed in by stack.c after a capture. */
typedef struct {
    unsigned long long addr;
    unsigned           module;   /* intern handle, basename */
    unsigned           symbol;   /* intern handle */
    unsigned           hits;
} stack_frame_t;
void render_set_stack_frames(const stack_frame_t *frames, int n, unsigned pid);

/* Universal per-row hit-test. Returns the pid under (x,y) across any view
   that renders process rows (main grid, memory top-N, disk/net/gpu top-N,
   forensic). 0xFFFFFFFFu if nothing. Populated during render_frame. */
unsigned render_hit_proc_row(int x, int y);

#ifdef __cplusplus
}
#endif
