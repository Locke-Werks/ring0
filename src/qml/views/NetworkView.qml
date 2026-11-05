// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts
import Ring0

Item {
    id: view
    readonly property color hue: Theme.views[4].hue
    property var ifaces: SnapshotBridge.seq >= 0 ? SnapshotBridge.ifaces() : []

    GridLayout {
        anchors.fill: parent
        anchors.margins: 20
        columns: Math.max(1, Math.min(view.ifaces.length > 2 ? 2 : 1, Math.floor(width / 520)))
        rowSpacing: 14
        columnSpacing: 14
        Repeater {
            model: view.ifaces.length
            delegate: IoCard {
                required property int index
                readonly property var n: view.ifaces[index] || ({})
                Layout.fillWidth: true
                Layout.fillHeight: true
                hue: view.hue
                name: n.name || ""
                metricA: "net:" + index + ":rx"
                metricB: "net:" + index + ":tx"
                labelA: "receive"
                labelB: "send"
                valueA: n.rx || 0
                valueB: n.tx || 0
                minScale: 256 * 1024
                bits: true
                // Link speed is bits/s; traffic is bytes/s. The meter is link utilisation.
                extra: n.link ? Theme.bitrate(n.link / 8) + " link" : ""
                extraLevel: n.link ? ((n.rx || 0) + (n.tx || 0)) * 8 / n.link : -1
            }
        }
    }
    Text {
        visible: view.ifaces.length === 0
        anchors.centerIn: parent
        text: "no connected adapters"
        color: Theme.faint
        font.family: Theme.font
    }
}
