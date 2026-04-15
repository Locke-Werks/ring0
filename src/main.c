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

static BOOL g_mica;

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
