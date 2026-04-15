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
#include "history.h"
#include "filter.h"

static ID2D1Factory          *g_d2d        = nullptr;
static IDWriteFactory        *g_dw         = nullptr;
static ID2D1HwndRenderTarget *g_rt         = nullptr;
static IDWriteTextFormat     *g_fmt_data   = nullptr;   /* cascadia mono, tabular */
static IDWriteTextFormat     *g_fmt_chrome = nullptr;   /* segoe ui variable, header/chrome */
static ID2D1SolidColorBrush  *g_brush      = nullptr;   /* recolored per draw */
static HWND                   g_hwnd       = nullptr;
static unsigned               g_w, g_h;
static BOOL                   g_transparent;
static unsigned               g_selected_pid = 0xFFFFFFFFu;
static int                    g_data_top_y, g_data_row_h;
static int                    g_scroll_rows;
static int                    g_visible_rows;
static int                    g_header_top, g_header_h;
static float                  g_col_xs[8], g_col_ws[8];      /* cached from last frame */
static int                    g_sort_col = -1;               /* -1 = NtQSI order */
static int                    g_sort_desc = 1;
static uint32_t               g_sort_idx[1024];
static int                    g_density = 1;                 /* 0 compact, 1 normal, 2 comfy */
static wchar_t                g_filter_text[256];
static int                    g_filter_len;
static int                    g_filter_caret;

/* Column descriptor — width <= 0 means "take the rest." */
struct Col {
    const wchar_t *label;
    float          width;
    bool           right_align;
};

enum { COL_PID, COL_CPU, COL_MEM, COL_THR, COL_HND, COL_NAME, COL_COUNT };

static const Col g_cols[COL_COUNT] = {
    { L"PID",   70,  true  },
    { L"CPU%",  140, true  },     /* widened to fit inline sparkline */
    { L"MEM",   90,  true  },
    { L"THR",   50,  true  },
    { L"HND",   60,  true  },
    { L"IMAGE", -1,  false },
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

extern "C" void render_set_selected(unsigned pid) { g_selected_pid = pid; }

extern "C" void render_get_data_layout(int *top_y, int *row_h)
{
    if (top_y) *top_y = g_data_top_y;
    if (row_h) *row_h = g_data_row_h;
}

extern "C" void render_set_scroll(int rows) { g_scroll_rows = rows < 0 ? 0 : rows; }
extern "C" int  render_get_scroll(void)     { return g_scroll_rows; }
extern "C" int  render_get_visible_rows(void) { return g_visible_rows; }

extern "C" void render_get_header_layout(int *top_y, int *h)
{
    if (top_y) *top_y = g_header_top;
    if (h)     *h     = g_header_h;
}

extern "C" int render_col_at_x(int x)
{
    for (int i = 0; i < COL_COUNT; ++i) {
        if ((float)x >= g_col_xs[i] && (float)x < g_col_xs[i] + g_col_ws[i])
            return i;
    }
    return -1;
}

extern "C" void render_cycle_density(int delta)
{
    g_density += delta;
    if (g_density < 0) g_density = 0;
    if (g_density > 2) g_density = 2;
}

extern "C" void render_set_filter(const wchar_t *text, int caret)
{
    g_filter_len = 0;
    if (text) {
        while (text[g_filter_len] && g_filter_len < 255) {
            g_filter_text[g_filter_len] = text[g_filter_len];
            ++g_filter_len;
        }
    }
    g_filter_text[g_filter_len] = 0;
    if (caret < 0) caret = 0;
    if (caret > g_filter_len) caret = g_filter_len;
    g_filter_caret = caret;
}

static void draw_command_bar(const theme_t *th, float width, float height)
{
    /* subtle backdrop */
    g_brush->SetColor(to_d2d(th->subtle, 0.20f));
    D2D1_RECT_F bg = D2D1::RectF(0, 0, width, height);
    g_rt->FillRectangle(bg, g_brush);

    const float input_x = 14.0f;
    const float input_y = 10.0f;
    const float input_w = width - 28.0f;
    const float input_h = height - 20.0f;

    /* rounded rect input */
    D2D1_ROUNDED_RECT rr;
    rr.rect    = D2D1::RectF(input_x, input_y, input_x + input_w, input_y + input_h);
    rr.radiusX = 8.0f;
    rr.radiusY = 8.0f;
    g_brush->SetColor(to_d2d(th->background, 0.45f));
    g_rt->FillRoundedRectangle(rr, g_brush);

    /* 1px subtle border */
    g_brush->SetColor(to_d2d(th->subtle, 0.6f));
    g_rt->DrawRoundedRectangle(rr, g_brush, 1.0f);

    const float text_x = input_x + 14.0f;
    const float text_w = input_w - 28.0f;

    g_fmt_chrome->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    D2D1_RECT_F tr = D2D1::RectF(text_x, input_y, text_x + text_w, input_y + input_h);

    if (g_filter_len == 0) {
        const wchar_t *ph = L"type to filter \u2014 pid:N session:N";
        g_brush->SetColor(to_d2d(th->foreground, 0.35f));
        g_rt->DrawText(ph, lstrlenW(ph), g_fmt_chrome, tr, g_brush);
    } else {
        g_brush->SetColor(to_d2d(th->foreground, 1.0f));
        g_rt->DrawText(g_filter_text, (UINT32)g_filter_len, g_fmt_chrome, tr, g_brush);
    }

    /* caret — a 1 DIP vertical bar. blinks at ~2 Hz off GetTickCount. */
    if ((GetTickCount() / 500u) % 2 == 0) {
        /* measure the substring up to caret to place the blinker accurately */
        float caret_x = text_x;
        if (g_filter_caret > 0) {
            IDWriteTextLayout *layout = nullptr;
            g_dw->CreateTextLayout(g_filter_text, (UINT32)g_filter_caret,
                                   g_fmt_chrome, text_w, input_h, &layout);
            if (layout) {
                DWRITE_HIT_TEST_METRICS m{};
                float x = 0, y = 0;
                layout->HitTestTextPosition(
                    (UINT32)g_filter_caret, FALSE, &x, &y, &m);
                caret_x = text_x + x;
                layout->Release();
            }
        }
        g_brush->SetColor(to_d2d(th->accent, 0.95f));
        D2D1_RECT_F caret = D2D1::RectF(caret_x, input_y + 6.0f,
                                        caret_x + 1.0f, input_y + input_h - 6.0f);
        g_rt->FillRectangle(caret, g_brush);
    }
}

extern "C" void render_toggle_sort(int col)
{
    if (col < 0 || col >= COL_COUNT) return;
    if (col == g_sort_col) { g_sort_desc = !g_sort_desc; return; }
    g_sort_col  = col;
    g_sort_desc = (col == COL_NAME) ? 0 : 1;   /* name asc, everything else desc */
}

static int cmp_proc(const process_t *a, const process_t *b, int col)
{
    switch (col) {
    case COL_PID: return (a->pid < b->pid) ? -1 : (a->pid > b->pid ? 1 : 0);
    case COL_CPU: return (int)a->cpu_pct - (int)b->cpu_pct;
    case COL_MEM: return (a->working_set < b->working_set) ? -1 :
                         (a->working_set > b->working_set ? 1 : 0);
    case COL_THR: return (int)a->thread_count - (int)b->thread_count;
    case COL_HND: return (int)a->handle_count - (int)b->handle_count;
    case COL_NAME: {
        const wchar_t *na = intern_get(a->image_name);
        const wchar_t *nb = intern_get(b->image_name);
        return lstrcmpiW(na ? na : L"", nb ? nb : L"");
    }
    }
    return 0;
}

static int proc_order(const process_t *a, const process_t *b)
{
    int r = cmp_proc(a, b, g_sort_col);
    if (g_sort_desc) r = -r;
    if (r == 0) r = (a->pid < b->pid) ? -1 : (a->pid > b->pid ? 1 : 0);
    return r;
}

static void sort_indices(uint32_t *idx, int n, const process_t *procs)
{
    /* insertion sort — n is ~300 and the array is mostly already sorted by pid */
    for (int i = 1; i < n; ++i) {
        uint32_t cur = idx[i];
        int j = i - 1;
        while (j >= 0 && proc_order(&procs[idx[j]], &procs[cur]) > 0) {
            idx[j + 1] = idx[j];
            --j;
        }
        idx[j + 1] = cur;
    }
}

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
    for (int i = 0; i < COL_COUNT; ++i) { g_col_xs[i] = xs[i]; g_col_ws[i] = ws[i]; }

    static const float density_row[3]    = { 20.0f, 24.0f, 28.0f };
    static const float density_header[3] = { 26.0f, 28.0f, 32.0f };
    const float row_h    = density_row[g_density];
    const float header_h = density_header[g_density];
    const float bar_h    = 48.0f;
    const float top      = bar_h + 12.0f;

    draw_command_bar(th, (float)g_w, bar_h);

    g_header_top = (int)top;
    g_header_h   = (int)header_h;
    wchar_t buf[256];

    /* header row in segoe, slightly muted */
    g_brush->SetColor(to_d2d(th->foreground, 0.65f));
    for (int i = 0; i < COL_COUNT; ++i) {
        wchar_t hbuf[64];
        const wchar_t *arrow = L"";
        if (i == g_sort_col) arrow = g_sort_desc ? L"  v" : L"  ^";
        int len = wsprintfW(hbuf, L"%s%s", g_cols[i].label, arrow);
        draw_cell(g_fmt_chrome, hbuf, len,
                  xs[i], ws[i], top, header_h, g_cols[i].right_align);
    }

    /* hairline divider under the header */
    g_brush->SetColor(to_d2d(th->subtle, 0.6f));
    D2D1_RECT_F divider = D2D1::RectF(COL_PAD, top + header_h - 1.0f,
                                      (float)g_w - COL_PAD, top + header_h);
    g_rt->FillRectangle(divider, g_brush);

    if (!snap) { g_rt->EndDraw(); return; }

    /* data rows in cascadia */
    float y = top + header_h + 4.0f;
    g_data_top_y   = (int)y;
    g_data_row_h   = (int)row_h;
    g_visible_rows = (int)(((float)g_h - y) / row_h);

    /* clamp scroll so we never blank the grid past the end */
    int max_scroll = (int)snap->n_processes - g_visible_rows;
    if (max_scroll < 0) max_scroll = 0;
    if (g_scroll_rows > max_scroll) g_scroll_rows = max_scroll;
    uint32_t start = (uint32_t)g_scroll_rows;

    /* visibility pass (filter) + sort */
    filter_query_t q;
    filter_parse(g_filter_len > 0 ? g_filter_text : L"", &q);

    uint32_t n = 0;
    for (uint32_t i = 0; i < snap->n_processes && n < 1024; ++i) {
        const process_t *pp = &snap->processes[i];
        const wchar_t *name = intern_get(pp->image_name);
        if (!filter_query_match(&q, pp->pid, pp->session_id, name)) continue;
        g_sort_idx[n++] = i;
    }
    if (g_sort_col >= 0) sort_indices(g_sort_idx, (int)n, snap->processes);

    for (uint32_t i = start; i < n; ++i) {
        if (y + row_h > (float)g_h) break;
        const process_t *p = &snap->processes[g_sort_idx[i]];
        int len;

        if (p->pid == g_selected_pid) {
            /* subtle accent fill across the row */
            g_brush->SetColor(to_d2d(th->accent, 0.10f));
            D2D1_RECT_F sel = D2D1::RectF(0.0f, y, (float)g_w, y + row_h);
            g_rt->FillRectangle(sel, g_brush);
            /* 2px accent left border */
            g_brush->SetColor(to_d2d(th->accent, 1.0f));
            D2D1_RECT_F bar = D2D1::RectF(0.0f, y, 2.0f, y + row_h);
            g_rt->FillRectangle(bar, g_brush);
        }

        g_brush->SetColor(to_d2d(th->foreground, 1.0f));
        len = fmt_uint(buf, p->pid);
        draw_cell(g_fmt_data, buf, len, xs[COL_PID], ws[COL_PID], y, row_h, true);

        /* sparkline takes the left ~60 DIPs of the CPU% column, numeric value
           pinned right. */
        const float spark_w = 60.0f;
        const float spark_h = row_h - 8.0f;
        const float spark_x = xs[COL_CPU];
        const float spark_y = y + 4.0f;
        uint16_t hist[HIST_SAMPLES];
        int hn = history_copy(p->pid, p->create_time, hist, HIST_SAMPLES);
        if (hn >= 2) {
            g_brush->SetColor(to_d2d(th->accent, 0.85f));
            float dx = spark_w / (float)(hn - 1);
            for (int s = 1; s < hn; ++s) {
                float a = (float)hist[s - 1] / 1000.0f;
                float b = (float)hist[s]     / 1000.0f;
                if (a > 1.0f) a = 1.0f;
                if (b > 1.0f) b = 1.0f;
                float y1 = spark_y + spark_h - a * spark_h;
                float y2 = spark_y + spark_h - b * spark_h;
                g_rt->DrawLine(D2D1::Point2F(spark_x + (s - 1) * dx, y1),
                               D2D1::Point2F(spark_x + s * dx,       y2),
                               g_brush, 1.0f);
            }
            g_brush->SetColor(to_d2d(th->foreground, 1.0f));
        }

        unsigned pct_int = p->cpu_pct / 10;
        unsigned pct_dec = p->cpu_pct % 10;
        len = wsprintfW(buf, L"%u.%u", pct_int, pct_dec);
        draw_cell(g_fmt_data, buf, len, xs[COL_CPU], ws[COL_CPU], y, row_h, true);

        len = fmt_mem(buf, p->working_set);
        draw_cell(g_fmt_data, buf, len, xs[COL_MEM], ws[COL_MEM], y, row_h, true);

        len = fmt_uint(buf, p->thread_count);
        draw_cell(g_fmt_data, buf, len, xs[COL_THR], ws[COL_THR], y, row_h, true);

        len = fmt_uint(buf, p->handle_count);
        draw_cell(g_fmt_data, buf, len, xs[COL_HND], ws[COL_HND], y, row_h, true);

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
