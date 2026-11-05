// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts
import Ring0

Item {
    id: view
    readonly property color hue: Theme.views[2].hue
    readonly property color hue2: Qt.lighter(hue, 1.45)
    property var m: SnapshotBridge.seq >= 0 ? SnapshotBridge.memory() : ({})

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 20
        spacing: 14

        RowLayout {
            Layout.fillWidth: true
            // Wrapped readouts need more height on narrow windows.
            Layout.preferredHeight: view.width < 1100 ? 190 : 120
            spacing: 14
            Panel {
                Layout.fillWidth: true
                Layout.fillHeight: true
                hue: view.hue
                heat: (view.m.used || 0) / (view.m.total || 1)
                Flow {
                    anchors.fill: parent
                    anchors.margins: 16
                    spacing: 36
                    Readout { label: "in use"; value: view.m.used || 0; format: Theme.bytes; hue: view.hue; size: 40 }
                    Readout { label: "available"; value: view.m.available || 0; format: Theme.bytes; size: 24 }
                    Readout { label: "cached"; value: view.m.cached || 0; format: Theme.bytes; size: 24 }
                    Readout { label: "committed"; value: view.m.commitUsed || 0; format: (v) => Theme.bytes(v) + " / " + Theme.bytes(view.m.commitLimit || 0); size: 24 }
                    Readout { label: "page faults/s"; value: view.m.faults || 0; size: 24 }
                }
            }
        }

        Panel {
            Layout.fillWidth: true
            Layout.preferredHeight: 64
            title: "composition"
            hue: view.hue
            SegmentBar {
                anchors.left: parent.left; anchors.right: parent.right
                anchors.margins: 12
                anchors.verticalCenter: parent.verticalCenter
                height: 12
                total: view.m.total || 1
                segments: [
                    // in use, then the standby cache that is still reclaimable
                    { value: view.m.used || 0, color: view.hue },
                    { value: Math.min(view.m.cached || 0, view.m.available || 0), color: Qt.darker(view.hue, 2.2) }
                ]
            }
        }

        Panel {
            Layout.fillWidth: true
            Layout.fillHeight: true
            hue: view.hue
            title: "physical  ·  commit  ·  last 5 min"
            Sparkline { anchors.fill: parent; anchors.margins: 1; metric: "mem"; samples: 300; color: view.hue; maxValue: 100; lineWidth: 2 }
            Sparkline { anchors.fill: parent; anchors.margins: 1; metric: "commit"; samples: 300; color: view.hue2; maxValue: 100; fill: false; lineWidth: 1.25; opacity: 0.8 }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: 84
            spacing: 14
            Panel {
                Layout.fillWidth: true; Layout.fillHeight: true; hue: view.hue
                Readout { anchors.fill: parent; anchors.margins: 14; label: "paged pool"; value: view.m.paged || 0; format: Theme.bytes; size: 22 }
            }
            Panel {
                Layout.fillWidth: true; Layout.fillHeight: true; hue: view.hue
                Readout { anchors.fill: parent; anchors.margins: 14; label: "nonpaged pool"; value: view.m.nonpaged || 0; format: Theme.bytes; size: 22 }
            }
            Panel {
                Layout.fillWidth: true; Layout.fillHeight: true; hue: view.hue
                Readout { anchors.fill: parent; anchors.margins: 14; label: "installed"; value: view.m.total || 0; format: Theme.bytes; size: 22 }
            }
        }
    }
}
