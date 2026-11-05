// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Ring0

Item {
    id: view
    readonly property color hue: Theme.views[5].hue
    readonly property color vramHue: Qt.hsla((hue.hslHue + 0.06) % 1.0, 0.75, 0.62, 1)
    property var gpus: SnapshotBridge.seq >= 0 ? SnapshotBridge.gpus() : []

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
            spacing: 24

            Repeater {
                model: view.gpus.length
                delegate: ColumnLayout {
                    id: gpu
                    required property int index
                    readonly property var g: view.gpus[index] || ({})
                    readonly property var allEngines: g.engines || []
                    // Lanes for 3D and anything working; idle engines (spare copy queues,
                    // security, optical flow) collapse into one line instead of a wall of zeros.
                    // Each lane keeps its engine's position in the full list: that index is
                    // what the history metric is keyed on, and objects are rebuilt every sample.
                    readonly property var engines: allEngines.map((e, i) => ({ name: e.name, pct: e.pct, idx: i }))
                                                             .filter(e => (e.name || "").toLowerCase() === "3d" || (e.pct || 0) >= 0.5)
                    readonly property var idleNames: allEngines.filter(e => !((e.name || "").toLowerCase() === "3d" || (e.pct || 0) >= 0.5))
                                                       .map(e => (e.name || "").toLowerCase())
                    Layout.fillWidth: true
                    // A single GPU fills the window; with several, each gets a fixed block.
                    Layout.preferredHeight: view.gpus.length === 1 ? Math.max(560, view.height - 40) : 620
                    spacing: 14

                    // Header: identity, the three numbers that matter, VRAM split.
                    Panel {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 128
                        Layout.maximumHeight: 128
                        hue: view.hue
                        heat: (gpu.g.total || 0) / 100
                        RowLayout {
                            anchors.fill: parent
                            anchors.margins: 16
                            spacing: 40
                            ColumnLayout {
                                spacing: 6
                                Text { text: gpu.g.name || ""; color: Theme.text; font.family: Theme.font; font.pixelSize: 15 }
                                Row {
                                    spacing: 32
                                    Readout { label: "utilisation"; value: gpu.g.total || 0; unit: "%"; hue: view.hue; size: 40 }
                                    Readout { visible: (gpu.g.temp || -1) >= 0; label: "temp"; value: gpu.g.temp || 0; unit: "°C"; size: 26 }
                                }
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 8
                                RowLayout {
                                    Layout.fillWidth: true
                                    Text { text: "dedicated vram"; color: Theme.dim; font.family: Theme.font; font.pixelSize: 12 }
                                    Item { Layout.fillWidth: true }
                                    Text {
                                        text: Theme.bytes(gpu.g.dedicatedUsed || 0) + " / " + Theme.bytes(gpu.g.dedicatedTotal || 0)
                                        color: Theme.text; font.family: Theme.font; font.pixelSize: 14
                                        font.features: { "tnum": 1 }
                                    }
                                }
                                SegmentBar {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 10
                                    total: gpu.g.dedicatedTotal || 1
                                    segments: [{ value: gpu.g.dedicatedUsed || 0, color: view.vramHue }]
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    Text { text: "shared"; color: Theme.dim; font.family: Theme.font; font.pixelSize: 12 }
                                    Item { Layout.fillWidth: true }
                                    Text {
                                        text: Theme.bytes(gpu.g.sharedUsed || 0) + " / " + Theme.bytes(gpu.g.sharedTotal || 0)
                                        color: Theme.dim; font.family: Theme.font; font.pixelSize: 12
                                        font.features: { "tnum": 1 }
                                    }
                                }
                            }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        spacing: 14

                        ColumnLayout {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            spacing: 14

                            // Busiest engine with VRAM traced over it: a frame-rate stall
                            // that coincides with VRAM pinning at the ceiling reads at a glance.
                            Panel {
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                Layout.minimumHeight: 140
                                hue: view.hue
                                heat: (gpu.g.total || 0) / 100
                                title: "utilisation  ·  vram outlined  ·  last 2 min"
                                Sparkline { anchors.fill: parent; anchors.margins: 1; metric: "gpu:" + gpu.index; samples: 120; color: view.hue; maxValue: 100; lineWidth: 2 }
                                Sparkline {
                                    anchors.fill: parent; anchors.margins: 1
                                    metric: "vram:" + gpu.index; samples: 120
                                    color: view.vramHue; fill: false; lineWidth: 1.25
                                    maxValue: Math.max(1, gpu.g.dedicatedTotal || 1)
                                }
                            }

                            // One lane per engine: which part of the chip is working.
                            Panel {
                                Layout.fillWidth: true
                                Layout.preferredHeight: Math.ceil(gpu.engines.length / lanes.cols) * lanes.cellHeight + 40 + (gpu.idleNames.length ? 24 : 0)
                                Layout.maximumHeight: Layout.preferredHeight
                                hue: view.hue
                                title: "engines"
                                Text {
                                    visible: gpu.idleNames.length > 0
                                    anchors.left: parent.left; anchors.leftMargin: 12
                                    anchors.right: parent.right; anchors.rightMargin: 12
                                    anchors.bottom: parent.bottom; anchors.bottomMargin: 8
                                    elide: Text.ElideRight
                                    text: "idle  ·  " + [...new Set(gpu.idleNames)].map(n => {
                                        const c = gpu.idleNames.filter(x => x === n).length
                                        return c > 1 ? n + " ×" + c : n
                                    }).join(",  ")
                                    color: Theme.faint
                                    font.family: Theme.font
                                    font.pixelSize: 11
                                }
                                GridView {
                                    id: lanes
                                    anchors.fill: parent
                                    anchors.margins: 6
                                    anchors.topMargin: 0
                                    anchors.bottomMargin: gpu.idleNames.length ? 30 : 6
                                    interactive: false
                                    model: gpu.engines.length
                                    readonly property int cols: Math.max(1, Math.floor(width / 230))
                                    cellWidth: Math.floor(width / cols)
                                    cellHeight: 70
                                    delegate: Item {
                                        id: lane
                                        required property int index
                                        readonly property var e: gpu.engines[index] || ({})
                                        readonly property real pct: e.pct || 0
                                        width: lanes.cellWidth
                                        height: lanes.cellHeight
                                        Rectangle {
                                            anchors.fill: parent; anchors.margins: 4
                                            radius: 4
                                            color: Theme.surface2
                                            // An idle engine recedes; a busy one comes forward.
                                            border.color: Qt.tint(Theme.line, Qt.rgba(view.hue.r, view.hue.g, view.hue.b, Math.min(1, lane.pct / 80)))
                                            Behavior on border.color { ColorAnimation { duration: 400 } }
                                            Sparkline {
                                                anchors.fill: parent; anchors.margins: 1; anchors.topMargin: 24
                                                metric: "gpu:" + gpu.index + ":" + (lane.e.idx === undefined ? 0 : lane.e.idx)
                                                samples: 60; color: view.hue; maxValue: 100
                                                opacity: 0.35 + 0.65 * Math.min(1, lane.pct / 30)
                                                Behavior on opacity { NumberAnimation { duration: 400 } }
                                            }
                                            Text { x: 9; y: 5; text: (lane.e.name || "").toLowerCase(); color: Theme.dim; font.family: Theme.font; font.pixelSize: 11 }
                                            Text {
                                                anchors.right: parent.right; anchors.rightMargin: 9; y: 5
                                                text: lane.pct.toFixed(0) + "%"
                                                color: lane.pct > 1 ? Theme.text : Theme.faint
                                                font.family: Theme.mono; font.pixelSize: 11
                                            }
                                        }
                                    }
                                }
                            }
                        }

                        // Who is using the GPU.
                        Panel {
                            Layout.preferredWidth: Math.min(340, view.width * 0.3)
                            Layout.fillHeight: true
                            hue: view.hue
                            title: "top gpu processes"
                            property bool byVram: false
                            id: topPanel
                            FilterProxy {
                                id: top
                                sourceModel: ProcessModel {}
                                sortKey: topPanel.byVram ? "vram" : "gpu"
                                descending: true
                            }
                            // Utilisation answers "what is drawing", VRAM "what is holding memory";
                            // they are often different processes.
                            Row {
                                anchors.right: parent.right
                                anchors.rightMargin: 10
                                y: -22
                                spacing: 4
                                Repeater {
                                    model: [{ k: false, t: "util" }, { k: true, t: "vram" }]
                                    delegate: Rectangle {
                                        required property var modelData
                                        readonly property bool on: topPanel.byVram === modelData.k
                                        width: lbl.implicitWidth + 16; height: 20; radius: 10
                                        color: on ? view.hue : "transparent"
                                        border.color: on ? view.hue : Theme.faint
                                        Behavior on color { ColorAnimation { duration: 120 } }
                                        Text { id: lbl; anchors.centerIn: parent; text: modelData.t; color: parent.on ? "#000000" : Theme.dim; font.family: Theme.font; font.pixelSize: 11 }
                                        TapHandler { onTapped: topPanel.byVram = parent.modelData.k }
                                    }
                                }
                            }
                            ListView {
                                id: topList
                                anchors.fill: parent
                                anchors.margins: 10
                                anchors.topMargin: 2
                                clip: true
                                interactive: false
                                model: top
                                spacing: 2
                                // Largest VRAM holder sets the rail scale in vram mode.
                                readonly property real maxVram: Math.max(1, SnapshotBridge.maxProcessVram)
                                delegate: Item {
                                    id: tr
                                    required property int index
                                    required property string name
                                    required property string path
                                    required property double gpu
                                    required property double vram
                                    readonly property real metric: topPanel.byVram ? vram : gpu
                                    readonly property real peak: topPanel.byVram ? Math.max(1, topList.maxVram) : 100
                                    // Only processes actually using the GPU; the list ends where usage does.
                                    visible: index < Math.floor(topList.height / 32) && (topPanel.byVram ? vram > 0 : gpu >= 0.1)
                                    width: topList.width
                                    height: visible ? 30 : 0
                                    Image {
                                        id: ico
                                        width: 16; height: 16
                                        anchors.verticalCenter: parent.verticalCenter
                                        source: tr.path ? "image://procicon/" + encodeURIComponent(tr.path) : ""
                                        sourceSize: Qt.size(32, 32)
                                        asynchronous: true
                                    }
                                    Text {
                                        anchors.left: ico.right; anchors.leftMargin: 10
                                        anchors.right: amt.left; anchors.rightMargin: 10
                                        anchors.verticalCenter: parent.verticalCenter
                                        anchors.verticalCenterOffset: -2
                                        text: tr.name; elide: Text.ElideRight
                                        color: Theme.text; font.family: Theme.font; font.pixelSize: 13
                                    }
                                    Text {
                                        id: amt
                                        anchors.right: parent.right
                                        anchors.verticalCenter: parent.verticalCenter
                                        anchors.verticalCenterOffset: -2
                                        text: topPanel.byVram ? Theme.bytes(tr.vram) : tr.gpu.toFixed(1) + "%"
                                        color: Theme.dim; font.family: Theme.font; font.pixelSize: 12
                                        font.features: { "tnum": 1 }
                                    }
                                    Rectangle {
                                        anchors.left: ico.right; anchors.leftMargin: 10
                                        anchors.bottom: parent.bottom
                                        height: 2; radius: 1
                                        width: (parent.width - 26) * Math.min(1, tr.metric / tr.peak)
                                        color: view.hue; opacity: 0.8
                                        Behavior on width { NumberAnimation { duration: 1000; easing.type: Easing.Linear } }
                                    }
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
}
