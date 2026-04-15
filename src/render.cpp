#include <windows.h>
/* GDI's DrawText macro shadows ID2D1RenderTarget::DrawText. */
#undef DrawText
#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <stdint.h>
#include "render.h"
#include "theme.h"
#include "intern.h"

static ID2D1Factory          *g_d2d        = nullptr;
static IDWriteFactory        *g_dw         = nullptr;
static ID2D1HwndRenderTarget *g_rt         = nullptr;
static IDWriteTextFormat     *g_fmt        = nullptr;
static ID2D1SolidColorBrush  *g_text_brush = nullptr;
static HWND                   g_hwnd       = nullptr;
static unsigned               g_w, g_h;
static BOOL                   g_transparent;

static D2D1_COLOR_F to_d2d(COLORREF c, float a)
{
    D2D1_COLOR_F out;
    out.r = GetRValue(c) / 255.0f;
    out.g = GetGValue(c) / 255.0f;
    out.b = GetBValue(c) / 255.0f;
    out.a = a;
    return out;
}

static void create_target()
{
    if (g_rt) return;
    if (g_w == 0 || g_h == 0) {
        RECT rc;
        GetClientRect(g_hwnd, &rc);
        g_w = (unsigned)(rc.right  - rc.left);
        g_h = (unsigned)(rc.bottom - rc.top);
    }

    auto props = D2D1::RenderTargetProperties();
    props.pixelFormat = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                          D2D1_ALPHA_MODE_PREMULTIPLIED);
    auto hp = D2D1::HwndRenderTargetProperties(g_hwnd,
        D2D1::SizeU(g_w ? g_w : 1, g_h ? g_h : 1),
        D2D1_PRESENT_OPTIONS_NONE);

    if (FAILED(g_d2d->CreateHwndRenderTarget(props, hp, &g_rt))) {
        g_rt = nullptr;
        return;
    }

    auto white = D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f);
    g_rt->CreateSolidColorBrush(white, &g_text_brush);
}

extern "C" void render_init(HWND hwnd)
{
    g_hwnd = hwnd;
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                      __uuidof(ID2D1Factory),
                      reinterpret_cast<void **>(&g_d2d));
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                        __uuidof(IDWriteFactory),
                        reinterpret_cast<IUnknown **>(&g_dw));

    g_dw->CreateTextFormat(L"Cascadia Mono", nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"en-us", &g_fmt);
    if (!g_fmt) {
        g_dw->CreateTextFormat(L"Consolas", nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"en-us", &g_fmt);
    }

    create_target();
}

extern "C" void render_set_transparent(BOOL t) { g_transparent = t; }

extern "C" void render_resize(unsigned w, unsigned h)
{
    g_w = w; g_h = h;
    if (g_rt) {
        D2D1_SIZE_U sz = D2D1::SizeU(w, h);
        g_rt->Resize(sz);
    }
}

static int format_row(wchar_t *buf, const process_t *p)
{
    const wchar_t *name = intern_get(p->image_name);
    if (!name) name = L"<unknown>";
    unsigned ws_mb  = (unsigned)(p->working_set / (1024 * 1024));
    unsigned cpu_ms = (unsigned)(p->cpu_100ns / 10000);
    return wsprintfW(buf, L"%6u %4u %6uM %8u %s",
        p->pid, p->thread_count, ws_mb, cpu_ms, name);
}

extern "C" void render_frame(const snapshot_t *snap)
{
    if (!g_rt) create_target();
    if (!g_rt || !g_fmt || !g_text_brush) return;

    const theme_t *th = theme_current();

    g_text_brush->SetColor(to_d2d(th->foreground, 1.0f));

    D2D1_COLOR_F clear;
    if (g_transparent) { clear.r = clear.g = clear.b = clear.a = 0.0f; }
    else               clear = to_d2d(th->background, 1.0f);

    g_rt->BeginDraw();
    g_rt->Clear(clear);

    if (snap) {
        const float line_h = 16.0f;
        float y = 8.0f;
        wchar_t buf[512];

        int hl = wsprintfW(buf, L"   PID  THR     WS    CPU(ms) IMAGE");
        D2D1_RECT_F r = D2D1::RectF(12.0f, y, (float)g_w - 12.0f, y + line_h);
        g_rt->DrawText(buf, (UINT32)hl, g_fmt, r, g_text_brush);
        y += line_h * 1.5f;

        for (uint32_t i = 0; i < snap->n_processes; ++i) {
            if (y > (float)g_h) break;
            int n = format_row(buf, &snap->processes[i]);
            r.top = y; r.bottom = y + line_h;
            g_rt->DrawText(buf, (UINT32)n, g_fmt, r, g_text_brush);
            y += line_h;
        }
    }

    HRESULT hr = g_rt->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        if (g_text_brush) { g_text_brush->Release(); g_text_brush = nullptr; }
        if (g_rt)         { g_rt->Release();         g_rt         = nullptr; }
    }
}

extern "C" void render_destroy()
{
    if (g_text_brush) { g_text_brush->Release(); g_text_brush = nullptr; }
    if (g_rt)   { g_rt->Release();   g_rt  = nullptr; }
    if (g_fmt)  { g_fmt->Release();  g_fmt = nullptr; }
    if (g_dw)   { g_dw->Release();   g_dw  = nullptr; }
    if (g_d2d)  { g_d2d->Release();  g_d2d = nullptr; }
}
