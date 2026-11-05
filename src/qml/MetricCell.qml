// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import Ring0

// Number with a thin rail beneath it. The rail grows leftward from the number's
// own right edge, so it reads as a measure of that value; its length and the
// text's brightness both track the share of the column's scale, so a hot row
// stands out while the table scrolls past. Zero draws nothing at all.
Item {
    id: cell
    property real value: 0
    property real scale: 100          // value at which the rail is full
    property string text: ""
    property color hue: Theme.accent

    readonly property real frac: scale > 0 ? Math.min(1, value / scale) : 0
    property real shown: frac
    Behavior on shown { NumberAnimation { duration: 1000; easing.type: Easing.Linear } }

    readonly property real railWidth: width - 16

    Rectangle {
        id: rail
        // Below a few percent a rail is a dot, and a column of dots is noise.
        visible: cell.shown > 0.04
        anchors.right: parent.right
        anchors.rightMargin: 8
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 6
        height: 3
        radius: 1.5
        width: cell.railWidth * cell.shown
        color: Qt.rgba(cell.hue.r, cell.hue.g, cell.hue.b, 0.35 + 0.65 * Math.min(1, cell.shown * 2))
    }
    Text {
        anchors.right: parent.right
        anchors.rightMargin: 8
        anchors.verticalCenter: parent.verticalCenter
        anchors.verticalCenterOffset: -2
        text: cell.text
        color: cell.frac > 0.002 ? Qt.tint(Theme.dim, Qt.rgba(1, 1, 1, Math.min(1, cell.frac * 3))) : Theme.faint
        font.family: Theme.font
        font.pixelSize: 13
        font.features: { "tnum": 1 }
    }
}
