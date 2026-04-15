#define WIN32_LEAN_AND_MEAN
#define WINVER       0x0A00
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <dwmapi.h>
#include "theme.h"
#include "arena.h"
#include "ringbuf.h"

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

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        if (!g_mica) {
            HBRUSH br = CreateSolidBrush(theme_current()->background);
            FillRect(dc, &ps.rcPaint, br);
            DeleteObject(br);
        }
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

    HWND hwnd = CreateWindowExW(
        0, L"ring0", L"ring0",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 960, 640,
        NULL, NULL, hi, NULL);
    if (!hwnd) return 1;

    apply_dwm(hwnd);
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
#endif
    ExitProcess((UINT)run());
}
