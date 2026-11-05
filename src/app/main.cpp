// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/icon_provider.hpp"
#include "app/snapshot_bridge.hpp"
#include "app/window_chrome.hpp"

#include <QIcon>
#include <QImage>
#include <QPixmap>

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QSGRendererInterface>

#include <windows.h>
#include <dwmapi.h>

namespace {

// Older SDKs lack these; DWM ignores them before Windows 11 22H2.
constexpr DWORD kDwmUseImmersiveDarkMode = 20;
constexpr DWORD kDwmSystemBackdropType = 38;
constexpr DWORD kDwmWindowCornerPreference = 33;

void style_native_window(QWindow* w)
{
    const auto hwnd = reinterpret_cast<HWND>(w->winId());
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, kDwmUseImmersiveDarkMode, &dark, sizeof dark);
    const int round = 2;  // DWMWCP_ROUND
    DwmSetWindowAttribute(hwnd, kDwmWindowCornerPreference, &round, sizeof round);
    // No backdrop material: the canvas is opaque black by design, and Mica
    // would only show through as a tint during resize.
    const int none = 1;   // DWMSBT_NONE
    DwmSetWindowAttribute(hwnd, kDwmSystemBackdropType, &none, sizeof none);
}

} // namespace

int main(int argc, char* argv[])
{
    // D3D12 by default; QSG_RHI_BACKEND in the environment still overrides for testing.
    if (qEnvironmentVariableIsEmpty("QSG_RHI_BACKEND"))
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D12);

    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("ring0"));
    QGuiApplication::setOrganizationName(QStringLiteral("Locke Werks"));

    // Same multi-size icon the exe carries, so taskbar, Alt-Tab and title bar match.
    if (auto hi = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, 256, 256, 0))) {
        QIcon icon;
        for (int sz : {16, 24, 32, 48, 64, 256}) {
            if (auto h = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, sz, sz, 0))) {
                icon.addPixmap(QPixmap::fromImage(QImage::fromHICON(h)));
                DestroyIcon(h);
            }
        }
        DestroyIcon(hi);
        QGuiApplication::setWindowIcon(icon);
    }

    SnapshotBridge::instance();  // start sampling before the first frame

    QQmlApplicationEngine engine;
    register_icon_provider(engine);
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app,
                     [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    engine.loadFromModule(QStringLiteral("Ring0"), QStringLiteral("Main"));
    if (engine.rootObjects().isEmpty()) return 1;

    if (auto* w = qobject_cast<QQuickWindow*>(engine.rootObjects().first())) {
        style_native_window(w);
        WindowChrome::instance()->attach(w);
        // --view <name> opens on a given view; used for scripted screenshots.
        const auto args = QCoreApplication::arguments();
        const auto at = args.indexOf(QStringLiteral("--view"));
        if (at >= 0 && at + 1 < args.size()) {
            static const QStringList names = {
                QStringLiteral("processes"), QStringLiteral("cpu"), QStringLiteral("memory"),
                QStringLiteral("disk"), QStringLiteral("network"), QStringLiteral("gpu"),
                QStringLiteral("forensic")};
            const auto i = names.indexOf(args[at + 1]);
            if (i >= 0) w->setProperty("view", int(i));
        }
    }

    return QGuiApplication::exec();
}
