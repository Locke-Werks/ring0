// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Ring0

Item {
    id: view
    readonly property color hue: Theme.views[1].hue
    // Kernel time is drawn in a deeper shade of the same hue, so the split reads
    // as one quantity divided rather than two unrelated series.
    readonly property color kernelHue: Qt.darker(hue, 1.9)
    readonly property int cores: SnapshotBridge.coreCount

    function uptime(sec) {
        const d = Math.floor(sec / 86400), h = Math.floor(sec % 86400 / 3600), m = Math.floor(sec % 3600 / 60)
        return (d > 0 ? d + "d " : "") + h + "h " + (m < 10 ? "0" : "") + m + "m"
    }
    function count(n) { return n >= 10000 ? (n / 1000).toFixed(1) + "k" : Math.round(n).toString() }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 20
        spacing: 14

        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: 170
            Layout.maximumHeight: 170
            spacing: 14

            Panel {
                Layout.preferredWidth: 300
                Layout.fillHeight: true
                hue: view.hue
                heat: SnapshotBridge.cpuTotal / 100
                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 16
                    spacing: 10
                    RowLayout {
                        spacing: 22
                        Readout { label: "utilisation"; value: SnapshotBridge.cpuTotal; unit: "%"; hue: view.hue; size: 44 }
                        Column {
                            Layout.alignment: Qt.AlignBottom
                            spacing: 4
                            Row {
                                spacing: 6
                                Rectangle { width: 8; height: 8; radius: 2; color: view.hue; anchors.verticalCenter: parent.verticalCenter }
                                Text { text: "user " + Math.max(0, SnapshotBridge.cpuTotal - SnapshotBridge.cpuKernel).toFixed(0) + "%"; color: Theme.dim; font.family: Theme.font; font.pixelSize: 12 }
                            }
                            Row {
                                spacing: 6
                                Rectangle { width: 8; height: 8; radius: 2; color: view.kernelHue; anchors.verticalCenter: parent.verticalCenter }
                                Text { text: "kernel " + SnapshotBridge.cpuKernel.toFixed(0) + "%"; color: Theme.dim; font.family: Theme.font; font.pixelSize: 12 }
                            }
                        }
                    }
                    Item { Layout.fillHeight: true }
                    GridLayout {
                        columns: 4
                        columnSpacing: 18
                        rowSpacing: 2
                        Repeater {
                            model: [
                                { k: "threads", v: () => view.count(SnapshotBridge.threadCount) },
                                { k: "handles", v: () => view.count(SnapshotBridge.handleCount) },
                                { k: "up", v: () => view.uptime(SnapshotBridge.uptimeSeconds) },
                                { k: "temp", v: () => SnapshotBridge.cpuTempC >= 0 ? SnapshotBridge.cpuTempC.toFixed(0) + "°c" : "n/a" }
                            ]
                            delegate: Column {
                                required property var modelData
                                spacing: 1
                                Text { text: modelData.k; color: Theme.faint; font.family: Theme.font; font.pixelSize: 11 }
                                Text {
                                    text: SnapshotBridge.seq >= 0 ? modelData.v() : ""
                                    color: Theme.text; font.family: Theme.font; font.pixelSize: 14
                                    font.features: { "tnum": 1 }
                                }
                            }
                        }
                    }
                }
            }

            Panel {
                Layout.fillWidth: true
                Layout.fillHeight: true
                hue: view.hue
                heat: SnapshotBridge.cpuTotal / 100
                title: "total  ·  kernel beneath  ·  last 2 min"
                Sparkline { anchors.fill: parent; anchors.margins: 1; metric: "cpu";  samples: 120; color: view.hue; maxValue: 100; lineWidth: 2 }
                Sparkline { anchors.fill: parent; anchors.margins: 1; metric: "cpuk"; samples: 120; color: view.kernelHue; maxValue: 100; lineWidth: 1 }
            }
        }

        // Every core against time: "which cores, and when" in one glance, where
        // the tiles below answer "how much, right now".
        Panel {
            Layout.fillWidth: true
            Layout.preferredHeight: Math.max(56, Math.min(view.cores * 4 + 38, 170))
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
            Layout.minimumHeight: 90
            clip: true
            model: view.cores
            readonly property int cols: Math.max(2, Math.floor(width / 190))
            readonly property int rows: Math.ceil(view.cores / cols)
            cellWidth: Math.floor(width / cols)
            // Fit every core on screen when there is room; never shrink below a
            // readable tile, and scroll beyond that.
            cellHeight: Math.max(78, Math.min(120, rows > 0 ? Math.floor(height / rows) : 120))
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {
                policy: grid.contentHeight > grid.height ? ScrollBar.AsNeeded : ScrollBar.AlwaysOff
                contentItem: Rectangle { implicitWidth: 4; radius: 2; color: Theme.faint }
            }

            delegate: Item {
                id: cell
                required property int index
                width: grid.cellWidth
                height: grid.cellHeight
                readonly property real load: SnapshotBridge.seq >= 0 ? SnapshotBridge.coreLoad(index) : 0

                Panel {
                    anchors.fill: parent
                    anchors.margins: 4
                    hue: view.hue
                    heat: cell.load / 100

                    Sparkline {
                        anchors.fill: parent
                        anchors.margins: 1
                        anchors.topMargin: 28
                        metric: "core:" + cell.index
                        samples: 60
                        color: view.hue
                        maxValue: 100
                    }
                    Sparkline {
                        anchors.fill: parent
                        anchors.margins: 1
                        anchors.topMargin: 28
                        metric: "corek:" + cell.index
                        samples: 60
                        color: view.kernelHue
                        maxValue: 100
                        lineWidth: 1
                    }
                    Text {
                        x: 11; y: 8
                        text: cell.index
                        color: Theme.dim
                        font.family: Theme.mono
                        font.pixelSize: 11
                    }
                    Text {
                        anchors.right: parent.right
                        anchors.rightMargin: 11
                        y: 6
                        property real shown: cell.load
                        Behavior on shown { NumberAnimation { duration: 1000; easing.type: Easing.Linear } }
                        text: shown.toFixed(0) + "%"
                        color: Qt.tint(Theme.dim, Qt.rgba(view.hue.r, view.hue.g, view.hue.b, Math.min(1, cell.load / 60)))
                        font.family: Theme.font
                        font.pixelSize: 13
                        font.features: { "tnum": 1 }
                    }
                }
            }
        }
    }
}
