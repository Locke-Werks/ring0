// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import Ring0

// Time strip for the last five minutes. Drag anywhere to pin every view to that
// moment; double-click to return to live. CPU leads in the current view's hue,
// with memory and GPU behind it so a spike can be matched to its cause at a glance.
Rectangle {
    id: strip
    color: Theme.surface
    property color hue: Theme.accent
    readonly property int span: 300
    readonly property int labelBand: 16

    Rectangle { width: parent.width; height: 1; color: Theme.line }

    Item {
        id: plot
        anchors.fill: parent
        anchors.topMargin: 6
        anchors.bottomMargin: strip.labelBand

        Sparkline { anchors.fill: parent; metric: "gpu";  samples: strip.span; color: Theme.views[5].hue; maxValue: 100; fill: false; lineWidth: 1; opacity: 0.35 }
        Sparkline { anchors.fill: parent; metric: "mem";  samples: strip.span; color: Theme.views[2].hue; maxValue: 100; fill: false; lineWidth: 1; opacity: 0.35 }
        Sparkline { anchors.fill: parent; metric: "cpu";  samples: strip.span; color: strip.hue; maxValue: 100; lineWidth: 1.5 }

        // Minute ticks give the strip a scale.
        Repeater {
            model: 6
            delegate: Rectangle {
                required property int index
                x: plot.width - index * plot.width / 5
                width: 1
                height: plot.height
                color: Theme.line
                opacity: index === 0 ? 0 : 0.6
            }
        }
    }

    Repeater {
        model: ["now", "-1m", "-2m", "-3m", "-4m", "-5m"]
        delegate: Text {
            required property int index
            required property string modelData
            y: strip.height - strip.labelBand + 1
            x: Math.min(strip.width - width - 6, Math.max(6, strip.width - index * strip.width / 5 - width / 2))
            text: modelData
            color: Theme.faint
            font.family: Theme.mono
            font.pixelSize: 10
        }
    }

    // Pinned position.
    Rectangle {
        visible: SnapshotBridge.scrubBack > 0
        width: 2
        height: plot.height + 6
        radius: 1
        color: Theme.text
        x: strip.width - 1 - SnapshotBridge.scrubBack * strip.width / (strip.span - 1)
        Behavior on x { NumberAnimation { duration: 90; easing.type: Easing.OutCubic } }
    }
    Rectangle {
        visible: SnapshotBridge.scrubBack > 0
        anchors.right: parent.right
        anchors.rightMargin: 10
        y: 8
        width: pinned.implicitWidth + 16
        height: 20
        radius: 10
        color: Theme.surface2
        border.color: Theme.line
        Text {
            id: pinned
            anchors.centerIn: parent
            text: "-" + SnapshotBridge.scrubBack + "s  ·  double-click for live"
            color: Theme.text
            font.family: Theme.mono
            font.pixelSize: 11
        }
    }

    MouseArea {
        anchors.fill: parent
        cursorShape: Qt.SizeHorCursor
        function pin(mx) {
            SnapshotBridge.scrubBack = Math.round((strip.width - mx) * (strip.span - 1) / strip.width)
        }
        onPressed: (m) => pin(m.x)
        onPositionChanged: (m) => { if (pressed) pin(m.x) }
        onDoubleClicked: SnapshotBridge.scrubBack = 0
    }
}
