// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts
import Ring0

Item {
    id: view
    readonly property color hue: Theme.views[1].hue
    readonly property int cores: SnapshotBridge.coreCount

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 20
        spacing: 14

        // Summary: total, temperature, and the machine-wide graph.
        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: 150
            Layout.maximumHeight: 150
            spacing: 14

            Panel {
                Layout.preferredWidth: 220
                Layout.fillHeight: true
                hue: view.hue
                heat: SnapshotBridge.cpuTotal / 100
                Column {
                    anchors.fill: parent
                    anchors.margins: 16
                    spacing: 14
                    Readout { label: "utilisation"; value: SnapshotBridge.cpuTotal; unit: "%"; hue: view.hue; size: 44 }
                    Readout {
                        label: "package"
                        value: SnapshotBridge.cpuTempC
                        visible: SnapshotBridge.cpuTempC >= 0
                        unit: "°c"; size: 20
                    }
                }
            }

            Panel {
                Layout.fillWidth: true
                Layout.fillHeight: true
                hue: view.hue
                heat: SnapshotBridge.cpuTotal / 100
                title: "total  ·  last 2 min"
                Sparkline {
                    anchors.fill: parent
                    anchors.margins: 1
                    metric: "cpu"
                    samples: 120
                    color: view.hue
                    maxValue: 100
                    lineWidth: 2
                }
            }
        }

        // Every core against time. The heat map answers "which cores, and when"
        // in one glance where the tile grid answers "how much, right now".
        Panel {
            Layout.fillWidth: true
            Layout.preferredHeight: Math.max(48, Math.min(view.cores * 4 + 36, 160))
            Layout.maximumHeight: Layout.preferredHeight
            hue: view.hue
            title: "cores × time"
            Heatmap {
                anchors.fill: parent
                anchors.margins: 1
                source: "cores"
                samples: 120
                hue: view.hue
            }
        }

        GridView {
            id: grid
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 120
            clip: true
            model: view.cores
            readonly property int cols: Math.max(2, Math.floor(width / 200))
            cellWidth: Math.floor(width / cols)
            cellHeight: 104
            boundsBehavior: Flickable.StopAtBounds
            add: Transition { NumberAnimation { property: "opacity"; from: 0; to: 1; duration: 300 } }

            delegate: Item {
                id: cell
                required property int index
                width: grid.cellWidth
                height: grid.cellHeight
                readonly property real load: SnapshotBridge.seq >= 0 ? SnapshotBridge.coreLoad(index) : 0

                Panel {
                    anchors.fill: parent
                    anchors.margins: 5
                    hue: view.hue
                    heat: cell.load / 100

                    Sparkline {
                        anchors.fill: parent
                        anchors.margins: 1
                        anchors.topMargin: 30
                        metric: "core:" + cell.index
                        samples: 60
                        color: view.hue
                        maxValue: 100
                    }
                    Text {
                        x: 12; y: 9
                        text: "core " + cell.index
                        color: Theme.dim
                        font.family: Theme.font
                        font.pixelSize: 12
                    }
                    Text {
                        id: pct
                        anchors.right: parent.right
                        anchors.rightMargin: 12
                        y: 7
                        property real shown: cell.load
                        Behavior on shown { NumberAnimation { duration: 950; easing.type: Easing.InOutSine } }
                        text: shown.toFixed(0) + "%"
                        color: Qt.tint(Theme.dim, Qt.rgba(view.hue.r, view.hue.g, view.hue.b, Math.min(1, cell.load / 60)))
                        font.family: Theme.font
                        font.pixelSize: 14
                        font.features: { "tnum": 1 }
                    }
                }
            }
        }
    }
}
