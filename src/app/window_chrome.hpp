// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QAbstractNativeEventFilter>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QtQml/qqmlregistration.h>

// Custom title bar on a window that keeps every native behavior: edge resize,
// Aero snap, the Windows 11 snap-layout flyout on the maximize button, the DWM
// shadow and rounded corners. Qt is told the window is frameless; the frame
// styles are restored underneath it and hit-testing is answered here.
class WindowChrome : public QObject, public QAbstractNativeEventFilter {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    Q_PROPERTY(int captionHeight READ captionHeight WRITE setCaptionHeight NOTIFY captionHeightChanged)
    Q_PROPERTY(bool maximized READ maximized NOTIFY maximizedChanged)
    Q_PROPERTY(bool maxHovered READ maxHovered NOTIFY maxHoveredChanged)
    Q_PROPERTY(bool active READ active NOTIFY activeChanged)

public:
    static WindowChrome* instance();
    static WindowChrome* create(QQmlEngine*, QJSEngine*) { return instance(); }

    void attach(QQuickWindow* w);

    int captionHeight() const { return caption_; }
    void setCaptionHeight(int h);
    bool maximized() const { return maximized_; }
    bool maxHovered() const { return max_hovered_; }
    bool active() const { return active_; }

    // The maximize button must be reported as HTMAXBUTTON for snap layouts.
    Q_INVOKABLE void setMaximizeButton(QQuickItem* item) { max_button_ = item; }
    // Items inside the caption strip that take clicks (buttons, the filter box).
    Q_INVOKABLE void addInteractive(QQuickItem* item);
    Q_INVOKABLE void minimize();
    Q_INVOKABLE void toggleMaximize();
    Q_INVOKABLE void close();

    bool nativeEventFilter(const QByteArray& type, void* message, qintptr* result) override;

signals:
    void captionHeightChanged();
    void maximizedChanged();
    void maxHoveredChanged();
    void activeChanged();

private:
    WindowChrome() = default;
    bool hits(QQuickItem* item, QPointF logical) const;
    void setMaxHovered(bool h);
    void refreshMaximized();

    QPointer<QQuickWindow> window_;
    void* hwnd_ = nullptr;
    int caption_ = 40;
    bool maximized_ = false;
    bool max_hovered_ = false;
    bool active_ = true;
    QPointer<QQuickItem> max_button_;
    QList<QPointer<QQuickItem>> interactive_;
};
