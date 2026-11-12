// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Ring0

// Where the VRAM went for anything running a network: which process, which weights,
// and how each process's share splits into weights, kv cache and compute. Every
// segment says whether the runtime reported it or it was worked out from the files.
Item {
    id: view
    readonly property color hue: Theme.views[7].hue
    property var procs: SnapshotBridge.seq >= 0 ? SnapshotBridge.inference() : []
    property var gpus: SnapshotBridge.seq >= 0 ? SnapshotBridge.gpus() : []
    readonly property real vramTotal: gpus.reduce((a, g) => a + (g.dedicatedTotal || 0), 0)
    readonly property real vramUsed: gpus.reduce((a, g) => a + (g.dedicatedUsed || 0), 0)
    readonly property real inferUsed: procs.reduce((a, p) => a + (p.vram || 0), 0)

    function shade(i) {
        return Qt.hsla((hue.hslHue + 0.09 * i) % 1.0, 0.7, Math.max(0.35, 0.62 - 0.06 * i), 1)
    }
    function segColor(label) {
        if (label === "weights") return hue
        if (label === "kv cache") return shade(2)
        if (label === "compute" || label === "compute + runtime") return shade(4)
        return Theme.faint
    }

    Flickable {
        anchors.fill: parent
        contentHeight: column.implicitHeight + 40
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar { contentItem: Rectangle { implicitWidth: 4; radius: 2; color: Theme.faint } }

        ColumnLayout {
            id: column
            x: 20; y: 20
            width: parent.width - 40
            spacing: 16

            Panel {
                Layout.fillWidth: true
                Layout.preferredHeight: 200
                hue: view.hue
                heat: view.vramTotal > 0 ? view.inferUsed / view.vramTotal : 0

                Item {
                    anchors.fill: parent
                    anchors.topMargin: 92
                    anchors.margins: 1
                    Sparkline {
                        id: trend
                        anchors.fill: parent
                        metric: "infer"
                        samples: 120
                        color: view.hue
                        maxValue: view.vramTotal
                        lineWidth: 1.75
                    }
                    Text {
                        visible: trend.peakVisible
                        text: Theme.bytes(trend.peakValue)
                        color: view.hue
                        font.family: Theme.mono
                        font.pixelSize: 10
                        x: Math.max(0, Math.min(parent.width - width, trend.peakX - width / 2))
                        y: Math.max(0, trend.peakY - height - 3)
                    }
                }

                ColumnLayout {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: 16
                    spacing: 8
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 32
                        Readout { label: "inference vram"; value: view.inferUsed; format: v => Theme.bytes(v); hue: view.hue; size: 32 }
                        Readout { label: "all vram"; value: view.vramUsed; format: v => Theme.bytes(v); size: 22 }
                        Readout { label: "processes"; value: view.procs.length; unit: ""; size: 22 }
                        Item { Layout.fillWidth: true }
                    }
                    SegmentBar {
                        Layout.fillWidth: true
                        total: Math.max(1, view.vramTotal)
                        segments: view.procs.map((p, i) => ({ value: p.vram || 0, color: view.shade(i), label: p.name }))
                            .concat([{ value: Math.max(0, view.vramUsed - view.inferUsed), color: Theme.faint, label: "other" }])
                    }
                }
            }

            Text {
                visible: view.procs.length === 0
                Layout.fillWidth: true
                Layout.topMargin: 24
                horizontalAlignment: Text.AlignHCenter
                text: "nothing is running a network on the gpu"
                color: Theme.faint
                font.family: Theme.font
                font.pixelSize: 14
            }

            Repeater {
                model: view.procs.length
                delegate: Panel {
                    id: card
                    required property int index
                    readonly property var p: view.procs[index] || ({})
                    readonly property var segs: p.segments || []
                    readonly property var files: p.files || []
                    readonly property var warns: p.warnings || []
                    Layout.fillWidth: true
                    Layout.preferredHeight: body.implicitHeight + 32
                    hue: view.shade(index)
                    heat: view.vramTotal > 0 ? (p.vram || 0) / view.vramTotal * 2 : 0

                    ColumnLayout {
                        id: body
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.margins: 16
                        spacing: 10

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 12
                            Text { text: card.p.name || ""; color: Theme.text; font.family: Theme.font; font.pixelSize: 15 }
                            Text { text: "pid " + (card.p.pid || 0); color: Theme.faint; font.family: Theme.mono; font.pixelSize: 11 }
                            Rectangle {
                                radius: 3
                                color: Theme.surface2
                                implicitWidth: kindText.implicitWidth + 12
                                implicitHeight: kindText.implicitHeight + 4
                                Text { id: kindText; anchors.centerIn: parent; text: card.p.kind || ""; color: card.hue; font.family: Theme.font; font.pixelSize: 11 }
                            }
                            Text {
                                text: card.p.runtimes || ""
                                color: Theme.dim; font.family: Theme.font; font.pixelSize: 12
                                elide: Text.ElideRight; Layout.fillWidth: true
                            }
                            Text {
                                visible: (card.p.tokensPerS || -1) >= 0
                                text: (card.p.tokensPerS || 0).toFixed(1) + " tok/s"
                                color: card.hue; font.family: Theme.mono; font.pixelSize: 12
                            }
                            Text {
                                visible: (card.p.busySlots === undefined ? -1 : card.p.busySlots) >= 0
                                text: card.p.busySlots + " busy"
                                color: Theme.dim; font.family: Theme.mono; font.pixelSize: 12
                            }
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 28
                            Repeater {
                                model: [
                                    { label: "vram", v: card.p.vram || 0 },
                                    { label: "shared", v: card.p.shared || 0 },
                                    { label: "working set", v: card.p.workingSet || 0 }
                                ]
                                delegate: Column {
                                    required property var modelData
                                    spacing: 2
                                    Text { text: modelData.label; color: Theme.dim; font.family: Theme.font; font.pixelSize: 11 }
                                    Text { text: Theme.bytes(modelData.v); color: Theme.text; font.family: Theme.font; font.pixelSize: 14; font.features: { "tnum": 1 } }
                                }
                            }
                            Item { Layout.fillWidth: true }
                        }

                        SegmentBar {
                            visible: card.segs.length > 0
                            Layout.fillWidth: true
                            total: Math.max(1, card.p.vram || 0)
                            segments: card.segs.map(s => ({ value: s.bytes, color: view.segColor(s.label), label: s.label }))
                        }

                        Flow {
                            Layout.fillWidth: true
                            spacing: 18
                            Repeater {
                                model: card.segs
                                delegate: Row {
                                    required property var modelData
                                    spacing: 6
                                    Rectangle { width: 8; height: 8; radius: 2; anchors.verticalCenter: parent.verticalCenter; color: view.segColor(modelData.label) }
                                    Text { text: modelData.label; color: Theme.text; font.family: Theme.font; font.pixelSize: 12 }
                                    Text { text: Theme.bytes(modelData.bytes); color: Theme.text; font.family: Theme.mono; font.pixelSize: 12 }
                                    Text {
                                        text: modelData.source
                                        color: modelData.source === "exact" ? card.hue : Theme.faint
                                        font.family: Theme.font; font.pixelSize: 11
                                    }
                                }
                            }
                        }

                        Repeater {
                            model: card.files
                            delegate: RowLayout {
                                required property var modelData
                                Layout.fillWidth: true
                                spacing: 12
                                Text {
                                    text: modelData.name || modelData.path
                                    color: Theme.text; font.family: Theme.font; font.pixelSize: 12
                                    elide: Text.ElideMiddle; Layout.fillWidth: true
                                    ToolTip.visible: hover.hovered && modelData.path !== ""
                                    ToolTip.text: modelData.path
                                    HoverHandler { id: hover }
                                }
                                Text { text: modelData.evidence; color: Theme.faint; font.family: Theme.font; font.pixelSize: 11 }
                                Text {
                                    visible: modelData.bytes > 0
                                    text: Theme.bytes(modelData.bytes)
                                    color: Theme.dim; font.family: Theme.mono; font.pixelSize: 11
                                }
                                Text {
                                    visible: modelData.resident > 0
                                    text: Theme.bytes(modelData.resident) + " in ram"
                                    color: Theme.dim; font.family: Theme.mono; font.pixelSize: 11
                                }
                            }
                        }

                        Repeater {
                            model: card.warns
                            delegate: Text {
                                required property var modelData
                                text: modelData
                                color: Theme.accent
                                font.family: Theme.font
                                font.pixelSize: 12
                            }
                        }
                    }
                }
            }
        }
    }
}
