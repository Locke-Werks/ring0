// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import Ring0

// Proportional bar. Segments slide rather than jump so a shift in composition
// (cache handing pages back, commit growing) reads as motion, not flicker.
Item {
    id: bar
    property var segments: []        // [{ value, color, label }]
    property real total: 1
    implicitHeight: 10

    Rectangle { anchors.fill: parent; radius: height / 2; color: Theme.surface2 }

    Item {
        anchors.fill: parent
        clip: true
        Row {
            height: parent.height
            Repeater {
                model: bar.segments
                delegate: Rectangle {
                    required property var modelData
                    height: bar.height
                    width: bar.total > 0 ? bar.width * Math.max(0, modelData.value) / bar.total : 0
                    color: modelData.color
                    Behavior on width { NumberAnimation { duration: 950; easing.type: Easing.InOutSine } }
                }
            }
        }
    }
}
