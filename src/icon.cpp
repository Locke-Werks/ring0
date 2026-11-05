#include "icon.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <wincodec.h>
#include "render.h"
#include "intern.h"
#include "snapshot.h"

/* ── slot table ──────────────────────────────────────────────────────────── */

#define ICON_SLOTS 256u

struct icon_slot {
    volatile LONG  path_handle;   /* intern handle for image_path, 0 = empty */
    volatile LONG  state;         /* 0 empty, 1 pending, 2 ready, 3 failed */
    HICON          hicon;
    ID2D1Bitmap   *bitmap;
};

static icon_slot         g_icons[ICON_SLOTS];
static IWICImagingFactory *g_wic;
static HANDLE            g_thread;
static HANDLE            g_exit_event;

static icon_slot *slot_find(uint32_t handle)
{
    unsigned h = (unsigned)(handle * 2654435761u) & (ICON_SLOTS - 1);
    for (unsigned i = 0; i < 32; ++i) {
        icon_slot *s = &g_icons[(h + i) & (ICON_SLOTS - 1)];
        if ((uint32_t)s->path_handle == handle) return s;
        if (s->path_handle == 0) return nullptr;
    }
    return nullptr;
}

static icon_slot *slot_insert(uint32_t handle)
{
    unsigned h = (unsigned)(handle * 2654435761u) & (ICON_SLOTS - 1);
    for (unsigned i = 0; i < 32; ++i) {
        icon_slot *s = &g_icons[(h + i) & (ICON_SLOTS - 1)];
        if ((uint32_t)s->path_handle == handle) return s;
        if (s->path_handle == 0) {
            /* try to claim this slot atomically */
            if (InterlockedCompareExchange(&s->path_handle, (LONG)handle, 0) == 0)
                return s;
            /* someone else claimed it; keep probing */
        }
    }
    return nullptr;
}

/* ── worker thread ───────────────────────────────────────────────────────── */

static HICON extract_icon(const wchar_t *path)
{
    /* primary: ExtractIconExW — extracts the small icon resource directly
       from the exe/dll. No COM or shell initialisation needed. */
    HICON small_icon = NULL;
    if (ExtractIconExW(path, 0, NULL, &small_icon, 1) > 0 && small_icon)
        return small_icon;

    /* fallback: SHGetFileInfoW — handles UWP, shell associations, .lnk */
    SHFILEINFOW sfi = {};
    DWORD_PTR ok = SHGetFileInfoW(path, 0, &sfi, sizeof sfi,
                                  SHGFI_ICON | SHGFI_SMALLICON);
    if (ok && sfi.hIcon) return sfi.hIcon;

    return NULL;
}

static DWORD WINAPI icon_worker(LPVOID)
{
    CoInitializeEx(NULL, COINIT_MULTITHREADED);

    for (;;) {
        DWORD wait = WaitForSingleObject(g_exit_event, 100);
        if (wait != WAIT_TIMEOUT) break;

        const snapshot_t *snap = snapshot_current();
        if (!snap) continue;

        int batch = 0;
        for (uint32_t i = 0; i < snap->n_processes && batch < 16; ++i) {
            uint32_t ph = snap->processes[i].image_path;
            if (ph == 0) continue;

            icon_slot *s = slot_insert(ph);
            if (!s || s->state != 0) continue;

            InterlockedExchange(&s->state, 1);

            const wchar_t *path = intern_get(ph);
            if (!path || !*path) {
                InterlockedExchange(&s->state, 3);
                continue;
            }

            HICON h = extract_icon(path);
            if (h) {
                s->hicon = h;
                InterlockedExchange(&s->state, 2);
            } else {
                InterlockedExchange(&s->state, 3);
            }
            ++batch;
        }
    }

    CoUninitialize();
    return 0;
}

/* ── public API ──────────────────────────────────────────────────────────── */

extern "C" void icon_start(void)
{
    if (g_thread) return;

    /* WIC factory for HICON → D2D bitmap conversion */
    CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                     IID_IWICImagingFactory, (void **)&g_wic);

    g_exit_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_thread     = CreateThread(NULL, 0, icon_worker, NULL, 0, NULL);
}

extern "C" void icon_stop(void)
{
    if (g_thread) {
        SetEvent(g_exit_event);
        WaitForSingleObject(g_thread, 2000);
        CloseHandle(g_thread);
        CloseHandle(g_exit_event);
        g_thread = NULL;
    }
    for (unsigned i = 0; i < ICON_SLOTS; ++i) {
        if (g_icons[i].bitmap) { g_icons[i].bitmap->Release(); g_icons[i].bitmap = nullptr; }
        if (g_icons[i].hicon)  { DestroyIcon(g_icons[i].hicon); g_icons[i].hicon = NULL; }
        g_icons[i].path_handle = 0;
        g_icons[i].state = 0;
    }
    if (g_wic) { g_wic->Release(); g_wic = nullptr; }
}

extern "C" ID2D1Bitmap *icon_query(uint32_t image_path_handle)
{
    if (image_path_handle == 0) return nullptr;
    icon_slot *s = slot_find(image_path_handle);
    if (!s || s->state != 2) return nullptr;   /* not ready */

    /* lazy D2D bitmap creation on the render thread */
    if (!s->bitmap && s->hicon && g_wic) {
        ID2D1HwndRenderTarget *rt = (ID2D1HwndRenderTarget *)render_get_target_raw();
        if (!rt) return nullptr;

        IWICBitmap *wic_bmp = nullptr;
        g_wic->CreateBitmapFromHICON(s->hicon, &wic_bmp);
        if (wic_bmp) {
            rt->CreateBitmapFromWicBitmap(wic_bmp, &s->bitmap);
            wic_bmp->Release();
        }
    }
    return s->bitmap;
}

extern "C" void icon_invalidate_bitmaps(void)
{
    for (unsigned i = 0; i < ICON_SLOTS; ++i) {
        if (g_icons[i].bitmap) { g_icons[i].bitmap->Release(); g_icons[i].bitmap = nullptr; }
    }
}
