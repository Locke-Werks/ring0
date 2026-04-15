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

static int find_pid_index(const snapshot_t *s, unsigned pid)
{
    if (!s) return -1;
    for (uint32_t i = 0; i < s->n_processes; ++i)
        if (s->processes[i].pid == pid) return (int)i;
    return -1;
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
        /* WM_LBUTTONDOWN gives client coords in physical pixels under PMv2,
           but render's layout coords are DIPs — convert before comparing. */
        UINT dpi = GetDpiForWindow(hwnd);
        if (dpi == 0) dpi = 96;
        int x = MulDiv((short)LOWORD(lp), 96, (int)dpi);
        int y = MulDiv((short)HIWORD(lp), 96, (int)dpi);

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
            g_selected_pid = 0xFFFFFFFFu;
            render_set_selected(0xFFFFFFFFu);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        break;
    }

    case WM_DESTROY:
        snapshot_stop();
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
    snapshot_start(hwnd, 250);
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
#endif
    ExitProcess((UINT)run());
}
