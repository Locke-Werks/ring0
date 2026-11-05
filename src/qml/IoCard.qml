// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts
import Ring0

// One device with two directions of traffic: read/write for disks, rx/tx for
// adapters. The graph fills the card behind the numbers. Both series share a
// scale so their heights compare honestly; the second direction takes a shifted
// hue and an outline so the two stay distinct where they overlap.
Panel {
    id: card
    property string name: ""
    property string detail: ""
    property string metricA: ""
    property string metricB: ""
    property string labelA: ""
    property string labelB: ""
    property real valueA: 0
    property real valueB: 0
    property real ceiling: 0         // full-scale value, or 0 to auto-scale
    property string extra: ""        // right-aligned status (active %, link speed)
    property real extraLevel: -1     // 0..1 drives a small meter beside `extra`; -1 hides it

    readonly property color hueB: Qt.hsla((hue.hslHue + 0.11) % 1.0, Math.min(1, hue.hslSaturation * 1.1), Math.min(0.78, hue.hslLightness * 1.25), 1)
    heat: ceiling > 0 ? (valueA + valueB) / ceiling : Math.min(1, (valueA + valueB) / (64 * 1048576))

    Item {
        anchors.fill: parent
        anchors.margins: 1
        anchors.topMargin: 64
        Sparkline { anchors.fill: parent; metric: card.metricA; samples: 120; color: card.hue; maxValue: card.ceiling > 0 ? card.ceiling : -1; lineWidth: 1.75 }
        Sparkline { anchors.fill: parent; metric: card.metricB; samples: 120; color: card.hueB; maxValue: card.ceiling > 0 ? card.ceiling : -1; fill: false; lineWidth: 1.5 }
    }

    RowLayout {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 14
        spacing: 10
        Text { text: card.name; color: Theme.text; font.family: Theme.font; font.pixelSize: 15 }
        Text { text: card.detail; color: Theme.dim; font.family: Theme.font; font.pixelSize: 12; elide: Text.ElideRight; Layout.fillWidth: true }
        Rectangle {
            visible: card.extraLevel >= 0
            Layout.preferredWidth: 44
            Layout.preferredHeight: 4
            radius: 2
            color: Theme.surface2
            Rectangle {
                height: parent.height; radius: 2
                width: parent.width * Math.max(0, Math.min(1, card.extraLevel))
                color: card.hue
                Behavior on width { NumberAnimation { duration: 1000; easing.type: Easing.Linear } }
            }
        }
        Text { text: card.extra; color: card.hue; font.family: Theme.mono; font.pixelSize: 12 }
    }

    Row {
        x: 14; y: 40
        spacing: 26
        Repeater {
            model: [
                { label: card.labelA, value: card.valueA, c: card.hue },
                { label: card.labelB, value: card.valueB, c: card.hueB }
            ]
            delegate: Row {
                required property var modelData
                spacing: 7
                Rectangle { width: 8; height: 8; radius: 2; color: modelData.c; anchors.verticalCenter: parent.verticalCenter }
                Text {
                    property real shown: modelData.value
                    Behavior on shown { NumberAnimation { duration: 1000; easing.type: Easing.Linear } }
                    text: modelData.label + "  " + Theme.rate(shown)
                    color: Theme.text
                    font.family: Theme.font
                    font.pixelSize: 14
                    font.features: { "tnum": 1 }
                }
            }
        }
    }
}
