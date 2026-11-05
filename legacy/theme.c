#include "theme.h"
#include <dwmapi.h>

static theme_t g;

static BOOL system_is_dark(void)
{
    /* AppsUseLightTheme == 0 means apps use dark theme.
       Default to dark if the key is missing — this app is for 2am sessions. */
    DWORD val = 0, sz = sizeof val;
    LONG r = RegGetValueW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme", RRF_RT_DWORD, NULL, &val, &sz);
    if (r != ERROR_SUCCESS) return TRUE;
    return val == 0;
}

static COLORREF system_accent(void)
{
    /* DwmGetColorizationColor returns ARGB; strip alpha. Good enough until
       phase 2 swaps in UISettings.GetColorValue for a proper accent. */
    DWORD argb = 0;
    BOOL  op   = FALSE;
    if (FAILED(DwmGetColorizationColor(&argb, &op))) return RGB(0, 120, 212);
    return RGB((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF);
}

void theme_load(void)
{
    g.dark   = system_is_dark();
    g.accent = system_accent();
    if (g.dark) {
        g.background = RGB( 32,  32,  32);
        g.foreground = RGB(240, 240, 240);
        g.subtle     = RGB( 60,  60,  60);
    } else {
        g.background = RGB(243, 243, 243);
        g.foreground = RGB(  0,   0,   0);
        g.subtle     = RGB(200, 200, 200);
    }
}

const theme_t *theme_current(void) { return &g; }
