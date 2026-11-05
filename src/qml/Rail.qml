// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts
import Ring0

// Left navigation. Each entry carries a live readout in its view's hue so the
// rail doubles as an at-a-glance summary.
Rectangle {
    id: rail
    color: Theme.surface
    property int current: 0
    signal selected(int index)

    readonly property var readouts: [
        () => SnapshotBridge.processCount + " procs",
        () => SnapshotBridge.cpuTotal.toFixed(0) + "%",
        () => SnapshotBridge.memPct.toFixed(0) + "%",
        () => Theme.rate(SnapshotBridge.diskBps),
        () => Theme.rate(SnapshotBridge.netBps),
        () => SnapshotBridge.gpuTotal.toFixed(0) + "%",
        () => ""
    ]
    readonly property var metrics: ["", "cpu", "mem", "disk", "net", "gpu", ""]

    Rectangle { anchors.right: parent.right; width: 1; height: parent.height; color: Theme.line }

    ColumnLayout {
        anchors.fill: parent
        anchors.topMargin: 14
        spacing: 2

        Text {
            Layout.leftMargin: 18
            Layout.bottomMargin: 14
            text: "ring0"
            color: Theme.accent
            font.family: Theme.font
            font.pixelSize: 20
            font.weight: Font.DemiBold
        }

        Repeater {
            model: Theme.views
            delegate: Item {
                id: entry
                required property int index
                required property var modelData
                readonly property bool active: rail.current === index
                Layout.fillWidth: true
                Layout.preferredHeight: 52

                Rectangle {
                    anchors.fill: parent
                    anchors.leftMargin: 8
                    anchors.rightMargin: 8
                    radius: Theme.radius
                    color: entry.active ? Theme.surface2 : (hover.hovered ? "#121212" : "transparent")
                    Behavior on color { ColorAnimation { duration: 120 } }
                }
                Rectangle {
                    x: 8; width: 3; radius: 1.5
                    height: entry.active ? 28 : 0
                    anchors.verticalCenter: parent.verticalCenter
                    color: entry.modelData.hue
                    Behavior on height { NumberAnimation { duration: 160; easing.type: Easing.OutCubic } }
                }

                Sparkline {
                    visible: rail.metrics[entry.index] !== ""
                    anchors.fill: parent
                    anchors.leftMargin: 8
                    anchors.rightMargin: 8
                    anchors.topMargin: 26
                    metric: rail.metrics[entry.index]
                    samples: 60
                    color: entry.modelData.hue
                    opacity: entry.active ? 0.9 : 0.45
                    maxValue: (metric === "disk" || metric === "net") ? -1 : 100
                }

                Text {
                    x: 22; y: 8
                    text: entry.modelData.key
                    color: entry.active ? Theme.text : Theme.dim
                    font.family: Theme.font
                    font.pixelSize: 14
                }
                Text {
                    anchors.right: parent.right
                    anchors.rightMargin: 20
                    y: 9
                    text: SnapshotBridge.seq >= 0 ? rail.readouts[entry.index]() : ""
                    color: entry.modelData.hue
                    font.family: Theme.mono
                    font.pixelSize: 11
                }

                HoverHandler { id: hover }
                TapHandler { onTapped: rail.selected(entry.index) }
            }
        }

        Item { Layout.fillHeight: true }
    }
}
