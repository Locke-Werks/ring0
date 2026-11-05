// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import Ring0

// Time strip. Drag to pin every view to a point in history; double-click to go live.
Rectangle {
    id: strip
    color: Theme.surface
    property color hue: Theme.accent
    readonly property int span: 300

    Rectangle { width: parent.width; height: 1; color: Theme.line }

    Sparkline {
        anchors.fill: parent
        anchors.topMargin: 8
        metric: "cpu"
        samples: strip.span
        color: strip.hue
        opacity: 0.6
    }

    Rectangle {
        visible: SnapshotBridge.scrubBack > 0
        width: 2
        height: parent.height
        color: Theme.text
        x: strip.width - 1 - SnapshotBridge.scrubBack * strip.width / (strip.span - 1)
    }
    Text {
        anchors.right: parent.right
        anchors.rightMargin: 12
        y: 6
        text: SnapshotBridge.scrubBack > 0 ? "-" + SnapshotBridge.scrubBack + "s  ·  double-click for live" : "live"
        color: SnapshotBridge.scrubBack > 0 ? Theme.text : Theme.dim
        font.family: Theme.mono
        font.pixelSize: 11
    }

    MouseArea {
        anchors.fill: parent
        function pin(mx) {
            SnapshotBridge.scrubBack = Math.round((strip.width - mx) * (strip.span - 1) / strip.width)
        }
        onPressed: (m) => pin(m.x)
        onPositionChanged: (m) => { if (pressed) pin(m.x) }
        onDoubleClicked: SnapshotBridge.scrubBack = 0
    }
}
