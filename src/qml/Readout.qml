// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import Ring0

// Large number that eases between samples instead of snapping, so the eye can
// track direction of change between two one-second readings.
Column {
    id: r
    property string label: ""
    property real value: 0
    property int decimals: 0
    property string unit: ""
    property var format: null        // optional fn(value) -> string, overrides decimals/unit
    property color hue: Theme.text
    property int size: 34

    property real shown: value
    Behavior on shown { NumberAnimation { duration: 950; easing.type: Easing.InOutSine } }

    spacing: 2
    Text {
        text: r.label
        color: Theme.dim
        font.family: Theme.font
        font.pixelSize: 12
        font.letterSpacing: 0.4
    }
    Row {
        spacing: 3
        Text {
            text: r.format ? r.format(r.shown) : r.shown.toFixed(r.decimals)
            color: r.hue
            font.family: Theme.font
            font.pixelSize: r.size
            font.weight: Font.Light
            font.features: { "tnum": 1 }   // fixed-width digits stop the readout from shimmying
        }
        Text {
            visible: !r.format && r.unit !== ""
            anchors.baseline: parent.children[0].baseline
            text: r.unit
            color: Theme.dim
            font.family: Theme.font
            font.pixelSize: Math.round(r.size * 0.45)
        }
    }
}
