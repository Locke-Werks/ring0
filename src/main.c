#define WIN32_LEAN_AND_MEAN
#define WINVER       0x0A00
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>
#include "theme.h"
#include "arena.h"
#include "ringbuf.h"
#include "intern.h"
#include "snapshot.h"
#include "render.h"
#include "filter.h"
#include "etw.h"
#include "trust.h"
#include "ntapi.h"

int stack_capture(unsigned pid);

#define IDM_END_TASK        1001
#define IDM_END_TREE        1002
#define IDM_SEND_FORENSIC   1003
#define IDM_COPY_PID        1004
#define IDM_OPEN_LOCATION   1005
#define IDM_SUSPEND_RESUME  1010
#define IDM_PRI_REALTIME    1020
#define IDM_PRI_HIGH        1021
#define IDM_PRI_ABOVE       1022
#define IDM_PRI_NORMAL      1023
#define IDM_PRI_BELOW       1024
#define IDM_PRI_LOW         1025
#define IDM_AFF_SINGLE      1030

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

static unsigned g_menu_pid;     /* pid the popup menu was opened against */

static void end_process_by_pid(unsigned pid)
{
    if (pid == 0 || pid == 4) return;     /* refuse Idle and System */
    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (!h) return;
    TerminateProcess(h, 1);
    CloseHandle(h);
}

static void end_process_tree(unsigned root_pid)
{
    if (root_pid == 0 || root_pid == 4) return;
    const snapshot_t *s = snapshot_current();
    if (!s) { end_process_by_pid(root_pid); return; }

    /* BFS: collect descendants by walking ppid each step. Cap at 512 victims;
       a runaway tree this big is unlikely outside fork-bomb territory. */
    unsigned victims[512];
    int      vn = 0;
    victims[vn++] = root_pid;
    for (int rd = 0; rd < vn; ++rd) {
        unsigned parent = victims[rd];
        for (uint32_t i = 0; i < s->n_processes && vn < 512; ++i) {
            if (s->processes[i].ppid != parent) continue;
            if (s->processes[i].pid  == parent) continue;
            unsigned cpid = s->processes[i].pid;
            int dup = 0;
            for (int k = 0; k < vn; ++k) if (victims[k] == cpid) { dup = 1; break; }
            if (!dup) victims[vn++] = cpid;
        }
    }
    /* terminate children before parent so the parent's exit doesn't strand
       grandchildren before we process them */
    for (int i = vn - 1; i >= 0; --i) end_process_by_pid(victims[i]);
}

static void copy_pid_to_clipboard(HWND hwnd, unsigned pid)
{
    wchar_t buf[16];
    int n = wsprintfW(buf, L"%u", pid);
    if (!OpenClipboard(hwnd)) return;
    EmptyClipboard();
    HGLOBAL hm = GlobalAlloc(GMEM_MOVEABLE, ((SIZE_T)n + 1) * sizeof(wchar_t));
    if (hm) {
        wchar_t *m = (wchar_t *)GlobalLock(hm);
        if (m) {
            for (int i = 0; i <= n; ++i) m[i] = buf[i];
            GlobalUnlock(hm);
            SetClipboardData(CF_UNICODETEXT, hm);
        } else {
            GlobalFree(hm);
        }
    }
    CloseClipboard();
}

static void open_process_location(unsigned pid)
{
    const snapshot_t *s = snapshot_current();
    if (!s) return;
    for (uint32_t i = 0; i < s->n_processes; ++i) {
        if (s->processes[i].pid != pid) continue;
        const wchar_t *path = intern_get(s->processes[i].image_path);
        if (!path || !*path) return;
        wchar_t cmd[1024];
        int len = 0;
        const wchar_t *prefix = L"/select,\"";
        while (*prefix && len < 1023) cmd[len++] = *prefix++;
        const wchar_t *p = path;
        while (*p && len < 1022) cmd[len++] = *p++;
        cmd[len++] = L'"';
        cmd[len]   = 0;
        ShellExecuteW(NULL, L"open", L"explorer.exe", cmd, NULL, SW_SHOWNORMAL);
        return;
    }
}

static void set_process_priority(unsigned pid, DWORD pri_class)
{
    if (pid == 0 || pid == 4) return;
    HANDLE h = OpenProcess(PROCESS_SET_INFORMATION, FALSE, pid);
    if (!h) return;
    SetPriorityClass(h, pri_class);
    CloseHandle(h);
}

static void toggle_suspend(unsigned pid)
{
    if (pid == 0 || pid == 4) return;
    const snapshot_t *s = snapshot_current();
    int is_suspended = 0;
    if (s) {
        for (uint32_t i = 0; i < s->n_processes; ++i) {
            if (s->processes[i].pid == pid) {
                is_suspended = s->processes[i].suspended;
                break;
            }
        }
    }
    HANDLE h = OpenProcess(PROCESS_SUSPEND_RESUME, FALSE, pid);
    if (!h) return;
    if (is_suspended) {
        if (nt_resume)  nt_resume(h);
    } else {
        if (nt_suspend) nt_suspend(h);
    }
    CloseHandle(h);
}

static void restrict_to_single_core(unsigned pid)
{
    if (pid == 0 || pid == 4) return;
    HANDLE h = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!h) return;
    SetProcessAffinityMask(h, 1);
    CloseHandle(h);
}

static void show_context_menu(HWND hwnd, int sx, int sy, unsigned pid)
{
    if (pid == 0xFFFFFFFFu) return;
    g_menu_pid = pid;
    g_selected_pid = pid;
    render_set_selected(pid);
    InvalidateRect(hwnd, NULL, FALSE);

    /* look up suspended state */
    int is_suspended = 0;
    const snapshot_t *s = snapshot_current();
    if (s) {
        for (uint32_t i = 0; i < s->n_processes; ++i) {
            if (s->processes[i].pid == pid) {
                is_suspended = s->processes[i].suspended;
                break;
            }
        }
    }

    HMENU m = CreatePopupMenu();
    if (!m) return;

    wchar_t lbl[64];
    wsprintfW(lbl, L"end task  (pid %u)", pid);
    AppendMenuW(m, MF_STRING, IDM_END_TASK,      lbl);
    AppendMenuW(m, MF_STRING, IDM_END_TREE,      L"end process tree");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_SUSPEND_RESUME,
                is_suspended ? L"resume process" : L"suspend process");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);

    /* priority submenu with checkmark on current level */
    HMENU pri = CreatePopupMenu();
    if (pri) {
        AppendMenuW(pri, MF_STRING, IDM_PRI_REALTIME, L"realtime");
        AppendMenuW(pri, MF_STRING, IDM_PRI_HIGH,     L"high");
        AppendMenuW(pri, MF_STRING, IDM_PRI_ABOVE,    L"above normal");
        AppendMenuW(pri, MF_STRING, IDM_PRI_NORMAL,   L"normal");
        AppendMenuW(pri, MF_STRING, IDM_PRI_BELOW,    L"below normal");
        AppendMenuW(pri, MF_STRING, IDM_PRI_LOW,      L"low");

        HANDLE qh = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        DWORD cur_pri = 0;
        if (qh) { cur_pri = GetPriorityClass(qh); CloseHandle(qh); }
        UINT check_id = 0;
        switch (cur_pri) {
        case REALTIME_PRIORITY_CLASS:     check_id = IDM_PRI_REALTIME; break;
        case HIGH_PRIORITY_CLASS:         check_id = IDM_PRI_HIGH;     break;
        case ABOVE_NORMAL_PRIORITY_CLASS: check_id = IDM_PRI_ABOVE;    break;
        case NORMAL_PRIORITY_CLASS:       check_id = IDM_PRI_NORMAL;   break;
        case BELOW_NORMAL_PRIORITY_CLASS: check_id = IDM_PRI_BELOW;    break;
        case IDLE_PRIORITY_CLASS:         check_id = IDM_PRI_LOW;      break;
        }
        if (check_id) CheckMenuItem(pri, check_id, MF_BYCOMMAND | MF_CHECKED);
        AppendMenuW(m, MF_POPUP, (UINT_PTR)pri, L"set priority");
    }
    AppendMenuW(m, MF_STRING, IDM_AFF_SINGLE, L"restrict to single core");

    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_SEND_FORENSIC, L"send to forensic tab");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_COPY_PID,      L"copy pid");
    AppendMenuW(m, MF_STRING, IDM_OPEN_LOCATION, L"open file location");

    TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_LEFTALIGN, sx, sy, 0, hwnd, NULL);
    DestroyMenu(m);
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

        int view = render_hit_view_tab(x, y);
        if (view >= 0) {
            render_set_view(view);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        int pill = render_hit_pill(x, y);
        if (pill >= 0) {
            render_set_scope(pill);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        if (render_hit_tree_pill(x, y)) {
            render_toggle_tree();
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
        if (render_hit_capture_btn(x, y)) {
            stack_capture(g_selected_pid);
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
        if (row > 0 && y >= top) {
            int row_idx = render_get_scroll() + (y - top) / row;
            select_index(hwnd, row_idx);
            /* Ctrl+click in tree mode toggles collapse for that row */
            if (render_tree_mode() && (GetKeyState(VK_CONTROL) & 0x8000)) {
                const snapshot_t *s = snapshot_current();
                if (s && row_idx >= 0 && row_idx < (int)s->n_processes) {
                    render_tree_toggle_collapse(s->processes[row_idx].pid);
                    InvalidateRect(hwnd, NULL, FALSE);
                }
            }
        }
        SetFocus(hwnd);
        return 0;
    }

    case WM_MOUSEMOVE: {
        int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
        render_set_hover(x, y);
        if (wp & MK_LBUTTON) {
            int back = render_hit_scrubber(x, y);
            if (back >= 0) {
                render_set_view_back(back);
                InvalidateRect(hwnd, NULL, FALSE);
            }
        }
        return 0;
    }

    case WM_LBUTTONUP:
        if (GetCapture() == hwnd) ReleaseCapture();
        return 0;

    case WM_RBUTTONUP: {
        int x = (short)LOWORD(lp);
        int y = (short)HIWORD(lp);
        unsigned pid = render_hit_proc_row(x, y);
        if (pid == 0xFFFFFFFFu) return 0;
        POINT pt = { x, y };
        ClientToScreen(hwnd, &pt);
        show_context_menu(hwnd, pt.x, pt.y, pid);
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_END_TASK:
            end_process_by_pid(g_menu_pid);
            return 0;
        case IDM_END_TREE:
            end_process_tree(g_menu_pid);
            return 0;
        case IDM_SEND_FORENSIC:
            render_set_view(VIEW_FORENSIC);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        case IDM_COPY_PID:
            copy_pid_to_clipboard(hwnd, g_menu_pid);
            return 0;
        case IDM_OPEN_LOCATION:
            open_process_location(g_menu_pid);
            return 0;
        case IDM_SUSPEND_RESUME:
            toggle_suspend(g_menu_pid);
            return 0;
        case IDM_PRI_REALTIME:
            set_process_priority(g_menu_pid, REALTIME_PRIORITY_CLASS);
            return 0;
        case IDM_PRI_HIGH:
            set_process_priority(g_menu_pid, HIGH_PRIORITY_CLASS);
            return 0;
        case IDM_PRI_ABOVE:
            set_process_priority(g_menu_pid, ABOVE_NORMAL_PRIORITY_CLASS);
            return 0;
        case IDM_PRI_NORMAL:
            set_process_priority(g_menu_pid, NORMAL_PRIORITY_CLASS);
            return 0;
        case IDM_PRI_BELOW:
            set_process_priority(g_menu_pid, BELOW_NORMAL_PRIORITY_CLASS);
            return 0;
        case IDM_PRI_LOW:
            set_process_priority(g_menu_pid, IDLE_PRIORITY_CLASS);
            return 0;
        case IDM_AFF_SINGLE:
            restrict_to_single_core(g_menu_pid);
            return 0;
        }
        break;

    case WM_TIMER:
        if (wp == 1) {
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        break;

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
            if (wp >= '1' && wp <= '7') {
                render_set_view((int)(wp - '1'));
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
            if (wp == 'D') {
                render_toggle_diff();
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
            if (wp == 'G') {
                render_toggle_tree();
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
    /* Wider cap so the dpi-scaled chrome (command bar, view tabs, inspector,
       column header) all fit on a typical high-dpi display. */
    if (w > 2400) w = 2400;
    if (h > 1500) h = 1500;
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

    /* 60fps repaint tick — drives the per-frame EMA and graph tween between
       250ms snapshot publishes. WM_TIMER is coalesced so the actual cadence
       is system-timer-resolution limited (~15ms by default), good enough
       for smooth visual interpolation. */
    SetTimer(hwnd, 1, 16, NULL);

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return (int)m.wParam;
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR cmdLine, int show)
{
    (void)hInst; (void)hPrev; (void)cmdLine; (void)show;
#ifdef DEBUG
    arena_selftest();
    ringbuf_selftest();
    intern_selftest();
    filter_selftest();
#endif
    return run();
}
