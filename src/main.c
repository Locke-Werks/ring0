#define WIN32_LEAN_AND_MEAN
#define WINVER       0x0A00
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <dwmapi.h>
#include "theme.h"
#include "arena.h"
#include "ringbuf.h"
#include "intern.h"
#include "snapshot.h"
#include "render.h"
#include "filter.h"
#include "etw.h"
#include "trust.h"

/* dwmapi constants that only exist on recent Windows SDKs. Declared unconditionally
   so the build works on older SDKs; DWM silently rejects them on pre-Win11 22H2. */
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE   20
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE  33
#endif
#ifndef DWMWA_SYSTEMBACKDROP_TYPE
#define DWMWA_SYSTEMBACKDROP_TYPE       38
#endif
#ifndef DWMWCP_ROUND
#define DWMWCP_ROUND                    2
#endif
#ifndef DWMSBT_MAINWINDOW
#define DWMSBT_MAINWINDOW               2
#endif

static BOOL     g_mica;
static unsigned g_selected_pid = 0xFFFFFFFFu;   /* sentinel for "nothing selected"
                                                   so we don't accidentally match
                                                   the System Idle Process at pid 0 */
static wchar_t  g_filter_buf[256];
static int      g_filter_len;
static int      g_filter_caret;

static int find_pid_index(const snapshot_t *s, unsigned pid)
{
    if (!s) return -1;
    for (uint32_t i = 0; i < s->n_processes; ++i)
        if (s->processes[i].pid == pid) return (int)i;
    return -1;
}

static void push_filter(HWND hwnd)
{
    g_filter_buf[g_filter_len] = 0;
    render_set_filter(g_filter_buf, g_filter_caret);
    InvalidateRect(hwnd, NULL, FALSE);
}

static void filter_insert(HWND hwnd, wchar_t c)
{
    if (g_filter_len >= 255) return;
    for (int i = g_filter_len; i > g_filter_caret; --i)
        g_filter_buf[i] = g_filter_buf[i - 1];
    g_filter_buf[g_filter_caret] = c;
    ++g_filter_len;
    ++g_filter_caret;
    push_filter(hwnd);
}

static void filter_backspace(HWND hwnd)
{
    if (g_filter_caret == 0) return;
    for (int i = g_filter_caret - 1; i < g_filter_len - 1; ++i)
        g_filter_buf[i] = g_filter_buf[i + 1];
    --g_filter_caret;
    --g_filter_len;
    push_filter(hwnd);
}

static void filter_clear(HWND hwnd)
{
    g_filter_len   = 0;
    g_filter_caret = 0;
    push_filter(hwnd);
}

static void select_index(HWND hwnd, int idx)
{
    const snapshot_t *s = snapshot_current();
    if (!s || s->n_processes == 0) return;
    if (idx < 0) idx = 0;
    if (idx >= (int)s->n_processes) idx = (int)s->n_processes - 1;
    g_selected_pid = s->processes[idx].pid;
    render_set_selected(g_selected_pid);
    InvalidateRect(hwnd, NULL, FALSE);
}

static void apply_dwm(HWND hwnd)
{
    BOOL dark   = theme_current()->dark;
    int  corner = DWMWCP_ROUND;
    int  back   = DWMSBT_MAINWINDOW;

    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE,  &dark,   sizeof dark);
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof corner);

    /* DWMWA_SYSTEMBACKDROP_TYPE returns E_INVALIDARG on pre-22H2; treat that
       as "no mica, paint solid in WM_PAINT." */
    HRESULT hr = DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &back, sizeof back);
    g_mica = SUCCEEDED(hr);
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;

    case WM_SIZE:
        render_resize(LOWORD(lp), HIWORD(lp));
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        render_frame(snapshot_current());
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_SETTINGCHANGE:
        if (lp && lstrcmpW((LPCWSTR)lp, L"ImmersiveColorSet") == 0) {
            theme_load();
            BOOL dark = theme_current()->dark;
            DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof dark);
            InvalidateRect(hwnd, NULL, TRUE);
        }
        break;

    case WM_LBUTTONDOWN: {
        /* render is pinned to 96 DPI so its coord system == physical pixels.
           WM_LBUTTONDOWN already carries physical client coords, no conversion. */
        int x = (short)LOWORD(lp);
        int y = (short)HIWORD(lp);

        int pill = render_hit_pill(x, y);
        if (pill >= 0) {
            render_set_scope(pill);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        int tab = render_hit_tab(x, y);
        if (tab >= 0) {
            render_set_inspector_tab(tab);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        int back = render_hit_scrubber(x, y);
        if (back >= 0) {
            SetCapture(hwnd);
            render_set_view_back(back);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }

        int htop = 0, hh = 0;
        render_get_header_layout(&htop, &hh);
        if (y >= htop && y < htop + hh) {
            int col = render_col_at_x(x);
            if (col >= 0) {
                render_toggle_sort(col);
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
        }
        int top = 0, row = 0;
        render_get_data_layout(&top, &row);
        if (row > 0 && y >= top)
            select_index(hwnd, render_get_scroll() + (y - top) / row);
        SetFocus(hwnd);
        return 0;
    }

    case WM_MOUSEMOVE:
        if (wp & MK_LBUTTON) {
            int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
            int back = render_hit_scrubber(x, y);
            if (back >= 0) {
                render_set_view_back(back);
                InvalidateRect(hwnd, NULL, FALSE);
            }
        }
        return 0;

    case WM_LBUTTONUP:
        if (GetCapture() == hwnd) ReleaseCapture();
        return 0;

    case WM_MOUSEWHEEL: {
        int delta = (short)HIWORD(wp);
        UINT lines = 3;
        SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
        if (lines == (UINT)-1) lines = 3;     /* "scroll one screen at a time" */
        int rows = -delta * (int)lines / WHEEL_DELTA;
        render_set_scroll(render_get_scroll() + rows);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case WM_KEYDOWN: {
        BOOL ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        if (ctrl) {
            if (wp == VK_OEM_PLUS || wp == VK_ADD) {
                render_cycle_density(+1);
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
            if (wp == VK_OEM_MINUS || wp == VK_SUBTRACT) {
                render_cycle_density(-1);
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
            if (wp == 'I') {
                render_toggle_inspector();
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
            if (wp >= '1' && wp <= '2') {
                render_set_inspector_tab((int)(wp - '1'));
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
        }

        /* filter caret / deletion */
        if (wp == VK_BACK) { filter_backspace(hwnd); return 0; }
        if (wp == VK_LEFT) {
            if (g_filter_caret > 0) { --g_filter_caret; push_filter(hwnd); }
            return 0;
        }
        if (wp == VK_RIGHT) {
            if (g_filter_caret < g_filter_len) { ++g_filter_caret; push_filter(hwnd); }
            return 0;
        }

        const snapshot_t *s = snapshot_current();
        int cur = find_pid_index(s, g_selected_pid);
        switch (wp) {
        case VK_DOWN:  select_index(hwnd, cur + 1);          return 0;
        case VK_UP:    select_index(hwnd, cur < 0 ? 0 : cur - 1); return 0;
        case VK_NEXT:  select_index(hwnd, cur + 20);         return 0;
        case VK_PRIOR: select_index(hwnd, cur - 20);         return 0;
        case VK_HOME:  select_index(hwnd, 0);                return 0;
        case VK_END:   if (s) select_index(hwnd, (int)s->n_processes - 1); return 0;
        case VK_ESCAPE:
            if (g_filter_len > 0) { filter_clear(hwnd); return 0; }
            if (render_get_view_back() > 0) {
                render_set_view_back(0);
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
            g_selected_pid = 0xFFFFFFFFu;
            render_set_selected(0xFFFFFFFFu);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        case VK_SPACE:
            render_toggle_paused();
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        break;
    }

    case WM_CHAR: {
        /* wp is the character code. ignore control combos and non-printable. */
        if ((GetKeyState(VK_CONTROL) & 0x8000) && wp != L'/') return 0;
        if (wp < 0x20) return 0;             /* control chars, handled in KEYDOWN */
        filter_insert(hwnd, (wchar_t)wp);
        return 0;
    }

    case WM_DESTROY:
        trust_stop();
        snapshot_stop();
        etw_stop();
        render_destroy();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static int run(void)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    theme_load();

    HINSTANCE hi = GetModuleHandleW(NULL);

    WNDCLASSEXW wc = {0};
    wc.cbSize        = sizeof wc;
    wc.lpfnWndProc   = wndproc;
    wc.hInstance     = hi;
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = L"ring0";
    if (!RegisterClassExW(&wc)) return 1;

    /* CW_USEDEFAULT for x silently forces width/height to system defaults, which
       gave us a 480x320 window. Compute an explicit size + position from the
       primary monitor's work area instead. */
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int  sw = wa.right - wa.left, sh = wa.bottom - wa.top;
    int  w  = (sw * 4) / 5, h = (sh * 4) / 5;
    if (w > 1600) w = 1600;
    if (h > 1000) h = 1000;
    int  x  = wa.left + (sw - w) / 2;
    int  y  = wa.top  + (sh - h) / 2;

    HWND hwnd = CreateWindowExW(
        0, L"ring0", L"ring0",
        WS_OVERLAPPEDWINDOW,
        x, y, w, h,
        NULL, NULL, hi, NULL);
    if (!hwnd) return 1;

    apply_dwm(hwnd);
    render_init(hwnd);
    render_set_transparent(g_mica);
    etw_start();
    snapshot_start(hwnd, 250);
    trust_start();
    ShowWindow(hwnd, SW_SHOWDEFAULT);

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return (int)m.wParam;
}

void WinMainCRTStartup(void)
{
#ifdef DEBUG
    arena_selftest();
    ringbuf_selftest();
    intern_selftest();
    filter_selftest();
#endif
    ExitProcess((UINT)run());
}
