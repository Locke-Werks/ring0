// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts
import Ring0

Item {
    id: view
    readonly property color hue: Theme.views[4].hue
    property var ifaces: SnapshotBridge.seq >= 0 ? SnapshotBridge.ifaces() : []

    function speed(bps) {
        const bits = bps * 8;
        if (bits >= 1e9) return (bits / 1e9).toFixed(bits % 1e9 ? 1 : 0) + " gbit";
        if (bits >= 1e6) return (bits / 1e6).toFixed(0) + " mbit";
        return (bits / 1e3).toFixed(0) + " kbit";
    }

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
                // link speed is in bits/s; the graph is bytes/s
                ceiling: 0
                extra: n.link ? view.speed(n.link / 8) : ""
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
