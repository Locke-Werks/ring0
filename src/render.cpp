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
static IDWriteTextFormat     *g_fmt_data   = nullptr;   /* cascadia mono, tabular */
static IDWriteTextFormat     *g_fmt_chrome = nullptr;   /* segoe ui variable, header/chrome */
static ID2D1SolidColorBrush  *g_brush      = nullptr;   /* recolored per draw */
static HWND                   g_hwnd       = nullptr;
static unsigned               g_w, g_h;
static BOOL                   g_transparent;

/* Column descriptor — width <= 0 means "take the rest." */
struct Col {
    const wchar_t *label;
    float          width;
    bool           right_align;
};

enum { COL_PID, COL_MEM, COL_THR, COL_HND, COL_CPU_MS, COL_NAME, COL_COUNT };

static const Col g_cols[COL_COUNT] = {
    { L"PID",     70, true  },
    { L"MEM",     90, true  },
    { L"THR",     50, true  },
    { L"HND",     60, true  },
    { L"CPU(ms)", 90, true  },
    { L"IMAGE",   -1, false },
};

static const float COL_PAD = 12.0f;
static const float COL_GAP = 16.0f;

static D2D1_COLOR_F to_d2d(COLORREF c, float a)
{
    D2D1_COLOR_F out;
    out.r = GetRValue(c) / 255.0f;
    out.g = GetGValue(c) / 255.0f;
    out.b = GetBValue(c) / 255.0f;
    out.a = a;
    return out;
}

static void compute_columns(float client_w, float *xs, float *ws)
{
    float used = 0;
    int   flex = 0;
    for (int i = 0; i < COL_COUNT; ++i) {
        if (g_cols[i].width > 0) used += g_cols[i].width + COL_GAP;
        else                     ++flex;
    }
    float rest = client_w - 2 * COL_PAD - used;
    if (rest < 100.0f) rest = 100.0f;

    float x = COL_PAD;
    for (int i = 0; i < COL_COUNT; ++i) {
        xs[i] = x;
        ws[i] = g_cols[i].width > 0 ? g_cols[i].width : rest;
        x += ws[i] + COL_GAP;
    }
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
    g_rt->CreateSolidColorBrush(white, &g_brush);
}

static IDWriteTextFormat *make_format(const wchar_t *family, DWRITE_FONT_WEIGHT w, float sz)
{
    IDWriteTextFormat *f = nullptr;
    g_dw->CreateTextFormat(family, nullptr, w, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, sz, L"en-us", &f);
    return f;
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

    g_fmt_data = make_format(L"Cascadia Mono", DWRITE_FONT_WEIGHT_NORMAL, 13.0f);
    if (!g_fmt_data) g_fmt_data = make_format(L"Consolas", DWRITE_FONT_WEIGHT_NORMAL, 13.0f);

    g_fmt_chrome = make_format(L"Segoe UI Variable Display", DWRITE_FONT_WEIGHT_SEMI_BOLD, 12.0f);
    if (!g_fmt_chrome) g_fmt_chrome = make_format(L"Segoe UI", DWRITE_FONT_WEIGHT_SEMI_BOLD, 12.0f);

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

static int fmt_uint(wchar_t *buf, unsigned v) { return wsprintfW(buf, L"%u", v); }

static int fmt_mem(wchar_t *buf, uint64_t bytes)
{
    /* compact units up to GiB */
    if (bytes >= (uint64_t)1024 * 1024 * 1024)
        return wsprintfW(buf, L"%uG", (unsigned)(bytes / ((uint64_t)1024 * 1024 * 1024)));
    if (bytes >= 10ull * 1024 * 1024)
        return wsprintfW(buf, L"%uM", (unsigned)(bytes / (1024 * 1024)));
    return wsprintfW(buf, L"%uK", (unsigned)(bytes / 1024));
}

static void draw_cell(IDWriteTextFormat *fmt, const wchar_t *txt, int len,
                      float x, float w, float y, float h, bool right_align)
{
    fmt->SetTextAlignment(right_align ? DWRITE_TEXT_ALIGNMENT_TRAILING
                                       : DWRITE_TEXT_ALIGNMENT_LEADING);
    D2D1_RECT_F r = D2D1::RectF(x, y, x + w, y + h);
    g_rt->DrawText(txt, (UINT32)len, fmt, r, g_brush);
}

extern "C" void render_frame(const snapshot_t *snap)
{
    if (!g_rt) create_target();
    if (!g_rt || !g_fmt_data || !g_fmt_chrome || !g_brush) return;

    const theme_t *th = theme_current();

    D2D1_COLOR_F clear;
    if (g_transparent) { clear.r = clear.g = clear.b = clear.a = 0.0f; }
    else               clear = to_d2d(th->background, 1.0f);

    g_rt->BeginDraw();
    g_rt->Clear(clear);

    float xs[COL_COUNT], ws[COL_COUNT];
    compute_columns((float)g_w, xs, ws);

    const float row_h    = 22.0f;
    const float header_h = 28.0f;
    const float top      = 12.0f;
    wchar_t buf[256];

    /* header row in segoe, slightly muted */
    g_brush->SetColor(to_d2d(th->foreground, 0.65f));
    for (int i = 0; i < COL_COUNT; ++i) {
        int len = lstrlenW(g_cols[i].label);
        draw_cell(g_fmt_chrome, g_cols[i].label, len,
                  xs[i], ws[i], top, header_h, g_cols[i].right_align);
    }

    /* hairline divider under the header */
    g_brush->SetColor(to_d2d(th->subtle, 0.6f));
    D2D1_RECT_F divider = D2D1::RectF(COL_PAD, top + header_h - 1.0f,
                                      (float)g_w - COL_PAD, top + header_h);
    g_rt->FillRectangle(divider, g_brush);

    if (!snap) { g_rt->EndDraw(); return; }

    /* data rows in cascadia */
    g_brush->SetColor(to_d2d(th->foreground, 1.0f));
    float y = top + header_h + 4.0f;
    for (uint32_t i = 0; i < snap->n_processes; ++i) {
        if (y + row_h > (float)g_h) break;
        const process_t *p = &snap->processes[i];
        int len;

        len = fmt_uint(buf, p->pid);
        draw_cell(g_fmt_data, buf, len, xs[COL_PID], ws[COL_PID], y, row_h, true);

        len = fmt_mem(buf, p->working_set);
        draw_cell(g_fmt_data, buf, len, xs[COL_MEM], ws[COL_MEM], y, row_h, true);

        len = fmt_uint(buf, p->thread_count);
        draw_cell(g_fmt_data, buf, len, xs[COL_THR], ws[COL_THR], y, row_h, true);

        len = fmt_uint(buf, p->handle_count);
        draw_cell(g_fmt_data, buf, len, xs[COL_HND], ws[COL_HND], y, row_h, true);

        len = wsprintfW(buf, L"%u", (unsigned)(p->cpu_100ns / 10000));
        draw_cell(g_fmt_data, buf, len, xs[COL_CPU_MS], ws[COL_CPU_MS], y, row_h, true);

        const wchar_t *name = intern_get(p->image_name);
        if (!name) name = L"<unknown>";
        draw_cell(g_fmt_data, name, lstrlenW(name),
                  xs[COL_NAME], ws[COL_NAME], y, row_h, false);

        y += row_h;
    }

    HRESULT hr = g_rt->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        if (g_brush) { g_brush->Release(); g_brush = nullptr; }
        if (g_rt)    { g_rt->Release();    g_rt    = nullptr; }
    }
}

extern "C" void render_destroy()
{
    if (g_brush)      { g_brush->Release();      g_brush      = nullptr; }
    if (g_rt)         { g_rt->Release();         g_rt         = nullptr; }
    if (g_fmt_data)   { g_fmt_data->Release();   g_fmt_data   = nullptr; }
    if (g_fmt_chrome) { g_fmt_chrome->Release(); g_fmt_chrome = nullptr; }
    if (g_dw)         { g_dw->Release();         g_dw         = nullptr; }
    if (g_d2d)        { g_d2d->Release();        g_d2d        = nullptr; }
}
