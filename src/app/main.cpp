// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/snapshot_bridge.hpp"

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
    const int mica = 2;   // DWMSBT_MAINWINDOW
    DwmSetWindowAttribute(hwnd, kDwmSystemBackdropType, &mica, sizeof mica);
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

    SnapshotBridge::instance();  // start sampling before the first frame

    QQmlApplicationEngine engine;
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app,
                     [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    engine.loadFromModule(QStringLiteral("Ring0"), QStringLiteral("Main"));
    if (engine.rootObjects().isEmpty()) return 1;

    if (auto* w = qobject_cast<QWindow*>(engine.rootObjects().first()))
        style_native_window(w);

    return QGuiApplication::exec();
}
