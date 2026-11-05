// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts
import Ring0

Item {
    id: view
    readonly property color hue: Theme.views[3].hue
    property var disks: SnapshotBridge.seq >= 0 ? SnapshotBridge.disks() : []

    GridLayout {
        anchors.fill: parent
        anchors.margins: 20
        columns: Math.max(1, Math.min(view.disks.length > 2 ? 2 : 1, Math.floor(width / 520)))
        rowSpacing: 14
        columnSpacing: 14
        Repeater {
            model: view.disks.length
            delegate: IoCard {
                required property int index
                readonly property var d: view.disks[index] || ({})
                Layout.fillWidth: true
                Layout.fillHeight: true
                hue: view.hue
                name: "disk " + index
                detail: d.model || ""
                metricA: "disk:" + index + ":r"
                metricB: "disk:" + index + ":w"
                labelA: "read"
                labelB: "write"
                valueA: d.read || 0
                valueB: d.write || 0
                extra: (d.active || 0).toFixed(0) + "% active"
                extraLevel: (d.active || 0) / 100
            }
        }
    }
    Text {
        visible: view.disks.length === 0
        anchors.centerIn: parent
        text: "no physical disks reported"
        color: Theme.faint
        font.family: Theme.font
    }
}
