// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import Ring0

// Action feedback. Failures stay up longer than successes and carry the
// system's own reason.
Rectangle {
    id: toast
    property bool error: false
    function show(msg, isError) {
        label.text = msg
        error = isError
        hold.interval = isError ? 4500 : 1600
        state = "up"
        hold.restart()
    }

    anchors.horizontalCenter: parent.horizontalCenter
    width: label.implicitWidth + 32
    height: 36
    radius: 18
    color: Theme.surface2
    border.color: error ? Theme.accent : Theme.line
    opacity: 0
    y: parent.height

    Text {
        id: label
        anchors.centerIn: parent
        color: Theme.text
        font.family: Theme.font
        font.pixelSize: 13
    }
    Timer { id: hold; onTriggered: toast.state = "" }

    states: State {
        name: "up"
        PropertyChanges { toast.opacity: 1; toast.y: toast.parent.height - toast.height - 18 }
    }
    transitions: Transition {
        NumberAnimation { properties: "opacity,y"; duration: 260; easing.type: Easing.OutCubic }
    }
}
