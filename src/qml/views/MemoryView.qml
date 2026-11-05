// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts
import Ring0

Item {
    id: view
    readonly property color hue: Theme.views[2].hue
    readonly property color hue2: Qt.lighter(hue, 1.45)
    readonly property color cacheHue: Qt.darker(hue, 2.0)
    property var m: SnapshotBridge.seq >= 0 ? SnapshotBridge.memory() : ({})

    readonly property real used: m.used || 0
    readonly property real cached: Math.min(m.cached || 0, m.available || 0)
    readonly property real free: Math.max(0, (m.available || 0) - cached)

    function compact(n) { return n >= 1e6 ? (n / 1e6).toFixed(1) + "m" : n >= 1e4 ? (n / 1e3).toFixed(1) + "k" : Math.round(n).toString() }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 20
        spacing: 14

        // Headline numbers.
        Panel {
            Layout.fillWidth: true
            Layout.preferredHeight: flow.implicitHeight + 32
            hue: view.hue
            heat: view.used / (view.m.total || 1)
            Flow {
                id: flow
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: 16
                spacing: 40
                Readout { label: "in use"; value: view.used; format: Theme.bytes; hue: view.hue; size: 40 }
                Readout { label: "available"; value: view.m.available || 0; format: Theme.bytes; size: 24 }
                Readout { label: "standby cache"; value: view.cached; format: Theme.bytes; size: 24 }
                Readout { label: "committed"; value: view.m.commitUsed || 0; format: (v) => Theme.bytes(v) + " / " + Theme.bytes(view.m.commitLimit || 0); size: 24 }
                Readout { label: "page faults/s"; value: view.m.faults || 0; format: view.compact; size: 24 }
            }
        }

        // Where the RAM is: in use, cache the OS can hand back instantly, truly free.
        Panel {
            Layout.fillWidth: true
            Layout.preferredHeight: 78
            hue: view.hue
            title: "composition  ·  " + Theme.bytes(view.m.total || 0) + " installed"
            ColumnLayout {
                anchors.fill: parent
                anchors.leftMargin: 14
                anchors.rightMargin: 14
                anchors.bottomMargin: 10
                spacing: 8
                SegmentBar {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 10
                    total: view.m.total || 1
                    segments: [
                        { value: view.used, color: view.hue },
                        { value: view.cached, color: view.cacheHue },
                        { value: view.free, color: Theme.line }
                    ]
                }
                Row {
                    spacing: 22
                    Repeater {
                        model: [
                            { c: view.hue, t: "in use " + Theme.bytes(view.used) },
                            { c: view.cacheHue, t: "standby " + Theme.bytes(view.cached) },
                            { c: Theme.line, t: "free " + Theme.bytes(view.free) }
                        ]
                        delegate: Row {
                            required property var modelData
                            spacing: 6
                            Rectangle { width: 8; height: 8; radius: 2; color: modelData.c; anchors.verticalCenter: parent.verticalCenter }
                            Text { text: modelData.t; color: Theme.dim; font.family: Theme.font; font.pixelSize: 12 }
                        }
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 14

            Panel {
                Layout.fillWidth: true
                Layout.fillHeight: true
                hue: view.hue
                title: "physical  ·  commit outlined  ·  last 5 min"
                Sparkline { anchors.fill: parent; anchors.margins: 1; anchors.topMargin: 4; metric: "mem"; samples: 300; color: view.hue; maxValue: 100; lineWidth: 2 }
                Sparkline { anchors.fill: parent; anchors.margins: 1; anchors.topMargin: 4; metric: "commit"; samples: 300; color: view.hue2; maxValue: 100; fill: false; lineWidth: 1.25; opacity: 0.8 }
            }

            // Answers the first question anyone asks on this view.
            Panel {
                Layout.preferredWidth: Math.min(360, view.width * 0.32)
                Layout.fillHeight: true
                hue: view.hue
                title: "top consumers"
                FilterProxy {
                    id: top
                    sourceModel: ProcessModel {}
                    sortKey: "memory"
                    descending: true
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
                    readonly property real peak: Math.max(1, SnapshotBridge.maxProcessMemory)
                    delegate: Item {
                        id: tr
                        required property int index
                        required property string name
                        required property string path
                        required property double memory
                        visible: index < Math.floor(topList.height / 32)
                        width: topList.width
                        height: 30
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
                            text: tr.name
                            elide: Text.ElideRight
                            color: Theme.text
                            font.family: Theme.font
                            font.pixelSize: 13
                        }
                        Text {
                            id: amt
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.verticalCenterOffset: -2
                            text: Theme.bytes(tr.memory)
                            color: Theme.dim
                            font.family: Theme.font
                            font.pixelSize: 12
                            font.features: { "tnum": 1 }
                        }
                        Rectangle {
                            anchors.left: ico.right; anchors.leftMargin: 10
                            anchors.bottom: parent.bottom
                            height: 2; radius: 1
                            width: (parent.width - 26) * tr.memory / topList.peak
                            color: view.hue
                            opacity: 0.7
                            Behavior on width { NumberAnimation { duration: 1000; easing.type: Easing.Linear } }
                        }
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            // A layout inherits fillHeight from its children; pin it or this row
            // takes the space meant for the graph.
            Layout.fillHeight: false
            Layout.preferredHeight: 72
            Layout.maximumHeight: 72
            spacing: 14
            Repeater {
                model: [
                    { k: "paged pool", v: () => view.m.paged || 0 },
                    { k: "nonpaged pool", v: () => view.m.nonpaged || 0 },
                    { k: "commit limit", v: () => view.m.commitLimit || 0 }
                ]
                delegate: Panel {
                    required property var modelData
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    hue: view.hue
                    Readout { anchors.fill: parent; anchors.margins: 14; label: modelData.k; value: SnapshotBridge.seq >= 0 ? modelData.v() : 0; format: Theme.bytes; size: 20 }
                }
            }
        }
    }
}
