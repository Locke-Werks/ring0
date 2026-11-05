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
    property real minScale: 1048576  // auto-scale floor, bytes/s
    property bool bits: false        // show rates as bits/s (network) instead of bytes/s
    function fmt(v) { return bits ? Theme.bitrate(v) : Theme.rate(v) }
    // Axis ticks share the unit of the scale's top, so one graph never mixes KB and MB.
    function tick(v) {
        const top = sharedTop * (bits ? 8 : 1)
        const base = bits ? 1000 : 1024
        const units = bits ? ["bps", "Kbps", "Mbps", "Gbps", "Tbps"] : ["B/s", "KB/s", "MB/s", "GB/s", "TB/s"]
        let i = 0, div = 1
        while (top >= div * base && i < units.length - 1) { div *= base; ++i }
        const n = v * (bits ? 8 : 1) / div
        return (Math.round(n * 10) / 10).toString() + " " + units[i]
    }

    readonly property color hueB: Qt.hsla((hue.hslHue + 0.11) % 1.0, Math.min(1, hue.hslSaturation * 1.1), Math.min(0.78, hue.hslLightness * 1.25), 1)
    heat: ceiling > 0 ? (valueA + valueB) / ceiling : Math.min(1, (valueA + valueB) / (64 * 1048576))

    // One scale for both directions, set by whichever is larger.
    // Rounded up to 1, 2 or 5 x 10^n (in binary units) so the gridlines land on round values.
    readonly property real sharedTop: ceiling > 0 ? ceiling : niceCeil(Math.max(sa.dataPeak, sb.dataPeak, minScale))
    function niceCeil(v) {
        const steps = [1, 2, 4, 5, 8, 10]
        // Bits are quoted in decimal steps, bytes in binary ones.
        const base = bits ? 1000 : 1024
        if (bits) v *= 8
        let unit = 1
        while (v > unit * base) unit *= base
        const x = v / unit
        let mag = Math.pow(10, Math.floor(Math.log10(Math.max(x, 1e-9))))
        const scale = bits ? 1 / 8 : 1
        for (const st of steps) if (st * mag >= x) return st * mag * unit * scale
        return 10 * mag * unit * scale
    }

    Item {
        id: plot
        anchors.fill: parent
        anchors.margins: 1
        anchors.topMargin: 64

        // Axis: three gridlines with their values, following the eased scale.
        Repeater {
            model: [0.25, 0.5, 0.75]
            delegate: Item {
                required property real modelData
                width: plot.width
                y: plot.height * (1 - modelData)
                Rectangle { width: parent.width; height: 1; color: Theme.line; opacity: 0.55 }
                Text {
                    // Left edge holds the oldest data, so labels stay clear of the live line.
                    x: 10
                    anchors.bottom: parent.top
                    anchors.bottomMargin: 1
                    text: card.tick(card.sharedTop * modelData)
                    color: Theme.faint
                    font.family: Theme.mono
                    font.pixelSize: 10
                }
            }
        }

        Sparkline { id: sa; anchors.fill: parent; metric: card.metricA; samples: 120; color: card.hue; maxValue: card.sharedTop; minScale: card.minScale; lineWidth: 1.75 }
        Sparkline { id: sb; anchors.fill: parent; metric: card.metricB; samples: 120; color: card.hueB; maxValue: card.sharedTop; minScale: card.minScale; fill: false; lineWidth: 1.5 }
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
                    text: modelData.label + "  " + card.fmt(shown)
                    color: Theme.text
                    font.family: Theme.font
                    font.pixelSize: 14
                    font.features: { "tnum": 1 }
                }
            }
        }
    }
}
