// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/window_chrome.hpp"

#include <QGuiApplication>

#include <windows.h>
#include <dwmapi.h>
#include <windowsx.h>

namespace {

int frame_thickness(HWND hwnd)
{
    const UINT dpi = GetDpiForWindow(hwnd);
    return GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
}

bool is_maximized(HWND hwnd)
{
    WINDOWPLACEMENT wp{sizeof wp};
    return GetWindowPlacement(hwnd, &wp) && wp.showCmd == SW_SHOWMAXIMIZED;
}

} // namespace

WindowChrome* WindowChrome::instance()
{
    static auto* self = new WindowChrome;
    return self;
}

void WindowChrome::attach(QQuickWindow* w)
{
    window_ = w;
    w->setFlag(Qt::FramelessWindowHint, true);
    const auto hwnd = reinterpret_cast<HWND>(w->winId());
    hwnd_ = hwnd;

    // Frameless strips these; without them there is no snap, no maximize
    // animation and no shadow. The visible frame is removed in WM_NCCALCSIZE.
    auto style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    style |= WS_THICKFRAME | WS_CAPTION | WS_MAXIMIZEBOX | WS_MINIMIZEBOX | WS_SYSMENU;
    SetWindowLongPtrW(hwnd, GWL_STYLE, style);

    // One pixel of extended frame is what keeps the DWM shadow.
    const MARGINS m{0, 0, 1, 0};
    DwmExtendFrameIntoClientArea(hwnd, &m);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);

    QCoreApplication::instance()->installNativeEventFilter(this);
    connect(w, &QWindow::windowStateChanged, this, &WindowChrome::refreshMaximized);
    connect(w, &QWindow::activeChanged, this, [this] {
        const bool a = window_ && window_->isActive();
        if (a != active_) { active_ = a; emit activeChanged(); }
    });
    refreshMaximized();
}

void WindowChrome::setCaptionHeight(int h)
{
    if (h == caption_) return;
    caption_ = h;
    emit captionHeightChanged();
}

void WindowChrome::addInteractive(QQuickItem* item)
{
    if (item && !interactive_.contains(item)) interactive_.push_back(item);
}

void WindowChrome::minimize() { if (hwnd_) ShowWindow(static_cast<HWND>(hwnd_), SW_MINIMIZE); }
void WindowChrome::close() { if (window_) window_->close(); }

void WindowChrome::toggleMaximize()
{
    if (!hwnd_) return;
    const auto hwnd = static_cast<HWND>(hwnd_);
    ShowWindow(hwnd, is_maximized(hwnd) ? SW_RESTORE : SW_MAXIMIZE);
}

void WindowChrome::refreshMaximized()
{
    const bool m = hwnd_ && is_maximized(static_cast<HWND>(hwnd_));
    if (m != maximized_) { maximized_ = m; emit maximizedChanged(); }
}

void WindowChrome::setMaxHovered(bool h)
{
    if (h != max_hovered_) { max_hovered_ = h; emit maxHoveredChanged(); }
}

bool WindowChrome::hits(QQuickItem* item, QPointF logical) const
{
    if (!item || !item->isVisible() || !window_) return false;
    const QPointF local = item->mapFromScene(logical);
    return item->contains(local);
}

bool WindowChrome::nativeEventFilter(const QByteArray& type, void* message, qintptr* result)
{
    if (type != "windows_generic_MSG" || !hwnd_) return false;
    auto* msg = static_cast<MSG*>(message);
    const auto hwnd = static_cast<HWND>(hwnd_);
    if (msg->hwnd != hwnd) return false;

    switch (msg->message) {
    case WM_NCCALCSIZE: {
        if (!msg->wParam) return false;
        // Client area = whole window. Maximized windows hang their frame off
        // screen, so pull the client rect back in by that amount.
        if (is_maximized(hwnd)) {
            auto* p = reinterpret_cast<NCCALCSIZE_PARAMS*>(msg->lParam);
            const int f = frame_thickness(hwnd);
            p->rgrc[0].left += f;
            p->rgrc[0].top += f;
            p->rgrc[0].right -= f;
            p->rgrc[0].bottom -= f;
        }
        *result = 0;
        return true;
    }
    case WM_NCHITTEST: {
        POINT pt{GET_X_LPARAM(msg->lParam), GET_Y_LPARAM(msg->lParam)};
        RECT wr;
        GetWindowRect(hwnd, &wr);
        const int f = frame_thickness(hwnd);
        if (!is_maximized(hwnd)) {
            const bool l = pt.x < wr.left + f, r = pt.x >= wr.right - f;
            const bool t = pt.y < wr.top + f, b = pt.y >= wr.bottom - f;
            if (t && l) { *result = HTTOPLEFT; return true; }
            if (t && r) { *result = HTTOPRIGHT; return true; }
            if (b && l) { *result = HTBOTTOMLEFT; return true; }
            if (b && r) { *result = HTBOTTOMRIGHT; return true; }
            if (t) { *result = HTTOP; return true; }
            if (b) { *result = HTBOTTOM; return true; }
            if (l) { *result = HTLEFT; return true; }
            if (r) { *result = HTRIGHT; return true; }
        }
        ScreenToClient(hwnd, &pt);
        const qreal dpr = window_ ? window_->devicePixelRatio() : 1.0;
        const QPointF logical(pt.x / dpr, pt.y / dpr);
        if (logical.y() >= caption_) { setMaxHovered(false); *result = HTCLIENT; return true; }
        if (hits(max_button_, logical)) { setMaxHovered(true); *result = HTMAXBUTTON; return true; }
        setMaxHovered(false);
        for (const auto& item : interactive_)
            if (hits(item, logical)) { *result = HTCLIENT; return true; }
        *result = HTCAPTION;
        return true;
    }
    // HTMAXBUTTON routes the button's mouse traffic through the non-client
    // path, so the click has to be completed here rather than in QML.
    case WM_NCLBUTTONDOWN:
        if (msg->wParam == HTMAXBUTTON) { *result = 0; return true; }
        return false;
    case WM_NCLBUTTONUP:
        if (msg->wParam == HTMAXBUTTON) { toggleMaximize(); *result = 0; return true; }
        return false;
    case WM_NCMOUSELEAVE:
    case WM_MOUSELEAVE:
        setMaxHovered(false);
        return false;
    case WM_SIZE:
        refreshMaximized();
        return false;
    default:
        return false;
    }
}
