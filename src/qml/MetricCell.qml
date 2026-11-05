// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import Ring0

// Number with an underlay bar. The bar's length and the text's saturation both
// track the share of the column's scale, so a hot row stands out while the
// table scrolls past.
Item {
    id: cell
    property real value: 0
    property real scale: 100          // value at which the bar is full
    property string text: ""
    property color hue: Theme.accent

    readonly property real frac: scale > 0 ? Math.min(1, value / scale) : 0
    property real shown: frac
    Behavior on shown { NumberAnimation { duration: 1000; easing.type: Easing.Linear } }

    Rectangle {
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        height: parent.height - 10
        width: (parent.width - 4) * cell.shown
        radius: 2
        color: Qt.rgba(cell.hue.r, cell.hue.g, cell.hue.b, 0.10 + 0.35 * cell.shown)
    }
    Text {
        anchors.right: parent.right
        anchors.rightMargin: 8
        anchors.verticalCenter: parent.verticalCenter
        text: cell.text
        color: cell.frac > 0.005 ? Qt.tint(Theme.dim, Qt.rgba(1, 1, 1, Math.min(1, cell.frac * 3))) : Theme.faint
        font.family: Theme.font
        font.pixelSize: 13
        font.features: { "tnum": 1 }
    }
}
