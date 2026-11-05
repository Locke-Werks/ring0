// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts
import Ring0

// One device with two directions of traffic: read/write for disks, rx/tx for
// adapters. The two series share a scale so their heights compare honestly;
// the second is drawn as an outline so overlap stays legible.
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
    property real ceiling: 0         // link speed or 0 for auto
    property string extra: ""

    readonly property color hueB: Qt.lighter(hue, 1.5)
    heat: ceiling > 0 ? (valueA + valueB) / ceiling : Math.min(1, (valueA + valueB) / (64 * 1024 * 1024))

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 14
        spacing: 8

        RowLayout {
            Layout.fillWidth: true
            spacing: 10
            Text { text: card.name; color: Theme.text; font.family: Theme.font; font.pixelSize: 15 }
            Text { text: card.detail; color: Theme.dim; font.family: Theme.font; font.pixelSize: 12; elide: Text.ElideRight; Layout.fillWidth: true }
            Text { text: card.extra; color: card.hue; font.family: Theme.mono; font.pixelSize: 12 }
        }
        RowLayout {
            spacing: 28
            Readout { label: card.labelA; value: card.valueA; format: Theme.rate; hue: card.hue; size: 22 }
            Readout { label: card.labelB; value: card.valueB; format: Theme.rate; hue: card.hueB; size: 22 }
        }
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Sparkline { id: a; anchors.fill: parent; metric: card.metricA; samples: 120; color: card.hue; maxValue: card.ceiling > 0 ? card.ceiling : -1 }
            Sparkline { anchors.fill: parent; metric: card.metricB; samples: 120; color: card.hueB; maxValue: card.ceiling > 0 ? card.ceiling : -1; fill: false; lineWidth: 1.25 }
        }
    }
}
