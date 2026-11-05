// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import Ring0

// Surface for every card. The border brightens toward the view hue as `heat`
// rises, so a busy card is visible before its number is read.
Rectangle {
    id: panel
    property color hue: Theme.accent
    property real heat: 0            // 0..1
    property string title: ""
    default property alias content: body.data

    radius: Theme.radius
    color: Theme.surface
    border.width: 1
    border.color: Qt.tint(Theme.line, Qt.rgba(hue.r, hue.g, hue.b, Math.min(1, heat) * 0.7))
    Behavior on border.color { ColorAnimation { duration: 400; easing.type: Easing.OutCubic } }

    Text {
        id: titleText
        visible: panel.title !== ""
        x: 12; y: 9
        text: panel.title
        color: Theme.dim
        font.family: Theme.font
        font.pixelSize: 12
        font.letterSpacing: 0.4
    }

    Item {
        id: body
        anchors.fill: parent
        anchors.topMargin: panel.title !== "" ? 28 : 0
    }
}
