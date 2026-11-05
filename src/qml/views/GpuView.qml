// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts
import Ring0

Item {
    id: view
    readonly property color hue: Theme.views[5].hue
    property var gpus: SnapshotBridge.seq >= 0 ? SnapshotBridge.gpus() : []

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 20
        spacing: 14

        Repeater {
            model: view.gpus.length
            delegate: ColumnLayout {
                id: gpu
                required property int index
                readonly property var g: view.gpus[index] || ({})
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 14

                RowLayout {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 150
                    spacing: 14
                    Panel {
                        Layout.preferredWidth: 300
                        Layout.fillHeight: true
                        hue: view.hue
                        heat: (gpu.g.total || 0) / 100
                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 16
                            spacing: 8
                            Text { text: gpu.g.name || ""; color: Theme.text; font.family: Theme.font; font.pixelSize: 14; elide: Text.ElideRight; Layout.fillWidth: true }
                            RowLayout {
                                spacing: 24
                                Readout { label: "utilisation"; value: gpu.g.total || 0; unit: "%"; hue: view.hue; size: 36 }
                                Readout { visible: (gpu.g.temp || -1) >= 0; label: "temp"; value: gpu.g.temp || 0; unit: "°c"; size: 22 }
                            }
                            Text {
                                text: "vram " + Theme.bytes(gpu.g.dedicatedUsed || 0) + " / " + Theme.bytes(gpu.g.dedicatedTotal || 0)
                                color: Theme.dim; font.family: Theme.font; font.pixelSize: 12
                            }
                            SegmentBar {
                                Layout.fillWidth: true
                                height: 8
                                total: gpu.g.dedicatedTotal || 1
                                segments: [{ value: gpu.g.dedicatedUsed || 0, color: view.hue }]
                            }
                        }
                    }
                    Panel {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        hue: view.hue
                        heat: (gpu.g.total || 0) / 100
                        title: "busiest engine  ·  last 2 min"
                        Sparkline { anchors.fill: parent; anchors.margins: 1; metric: "gpu:" + gpu.index; samples: 120; color: view.hue; maxValue: 100; lineWidth: 2 }
                    }
                }

                // One lane per engine: which part of the chip is working, not just how much.
                Panel {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    hue: view.hue
                    title: "engines"
                    GridView {
                        id: lanes
                        anchors.fill: parent
                        anchors.margins: 6
                        clip: true
                        interactive: false
                        readonly property var engines: gpu.g.engines || []
                        model: engines.length
                        readonly property int cols: Math.max(1, Math.floor(width / 260))
                        cellWidth: Math.floor(width / cols)
                        cellHeight: 72
                        delegate: Item {
                            id: lane
                            required property int index
                            readonly property var e: lanes.engines[index] || ({})
                            width: lanes.cellWidth
                            height: lanes.cellHeight
                            Rectangle {
                                anchors.fill: parent; anchors.margins: 4
                                radius: 4; color: Theme.surface2
                                Sparkline {
                                    anchors.fill: parent; anchors.topMargin: 22
                                    metric: "gpu:" + gpu.index + ":" + lane.index
                                    samples: 60; color: view.hue; maxValue: 100
                                    opacity: 0.35 + 0.65 * Math.min(1, (lane.e.pct || 0) / 30)
                                    Behavior on opacity { NumberAnimation { duration: 400 } }
                                }
                                Text { x: 8; y: 5; text: (lane.e.name || "").toLowerCase(); color: Theme.dim; font.family: Theme.font; font.pixelSize: 11 }
                                Text { anchors.right: parent.right; anchors.rightMargin: 8; y: 5; text: (lane.e.pct || 0).toFixed(0) + "%"; color: Theme.text; font.family: Theme.mono; font.pixelSize: 11 }
                            }
                        }
                    }
                }
            }
        }

        Text {
            visible: view.gpus.length === 0
            Layout.alignment: Qt.AlignCenter
            text: "no gpu adapters reported"
            color: Theme.faint
            font.family: Theme.font
        }
    }
}
