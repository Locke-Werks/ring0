// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts
import Ring0

// Window chrome. Empty space drags the window (the native side answers
// HTCAPTION there), double-click maximizes, and the maximize button is the
// real one as far as Windows is concerned, so the snap-layout flyout works.
Rectangle {
    id: bar
    property string viewName: ""
    property color hue: Theme.accent
    implicitHeight: WindowChrome.captionHeight
    color: Theme.surface

    // The pulse marks each sample landing: proof the sampler is alive, and
    // the cadence everything else in the window moves to.
    property real pulse: 0
    Connections {
        target: SnapshotBridge
        function onChanged() { if (SnapshotBridge.scrubBack === 0) pulseAnim.restart() }
    }
    NumberAnimation { id: pulseAnim; target: bar; property: "pulse"; from: 1; to: 0; duration: 900; easing.type: Easing.OutCubic }

    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.line }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 14
        spacing: 10

        Item {
            Layout.preferredWidth: 18; Layout.preferredHeight: 18
            Rectangle {
                anchors.centerIn: parent
                width: 8 + 6 * bar.pulse; height: width; radius: width / 2
                color: "transparent"
                border.color: bar.hue
                border.width: 1.5
                opacity: 0.25 + 0.75 * bar.pulse
            }
            Rectangle { anchors.centerIn: parent; width: 6; height: 6; radius: 3; color: bar.hue }
        }
        Text {
            text: "ring0"
            color: WindowChrome.active ? Theme.text : Theme.dim
            font.family: Theme.font
            font.pixelSize: 14
            font.weight: Font.DemiBold
        }
        Text {
            text: "/  " + bar.viewName
            color: Theme.dim
            font.family: Theme.font
            font.pixelSize: 13
        }
        Item { Layout.fillWidth: true }

        Repeater {
            model: [
                { kind: "min" },
                { kind: "max" },
                { kind: "close" }
            ]
            delegate: Rectangle {
                id: btn
                required property var modelData
                readonly property bool isMax: modelData.kind === "max"
                readonly property bool isClose: modelData.kind === "close"
                readonly property bool hovered: isMax ? WindowChrome.maxHovered : ma.containsMouse
                Layout.preferredWidth: 46
                Layout.fillHeight: true
                color: hovered ? (isClose ? "#c42b1c" : Theme.surface2) : "transparent"
                Behavior on color { ColorAnimation { duration: 100 } }
                Component.onCompleted: {
                    if (isMax) WindowChrome.setMaximizeButton(btn)
                    else WindowChrome.addInteractive(btn)
                }

                // Glyphs drawn from rectangles so they stay pixel-crisp at any scale.
                Item {
                    anchors.centerIn: parent
                    width: 10; height: 10
                    readonly property color ink: btn.hovered && btn.isClose ? "#ffffff" : (WindowChrome.active ? Theme.text : Theme.dim)
                    Rectangle { visible: btn.modelData.kind === "min"; anchors.centerIn: parent; width: 10; height: 1; color: parent.ink }
                    Rectangle {
                        visible: btn.isMax && !WindowChrome.maximized
                        anchors.fill: parent; color: "transparent"; border.color: parent.ink; border.width: 1; radius: 1.5
                    }
                    Item {
                        visible: btn.isMax && WindowChrome.maximized
                        anchors.fill: parent
                        Rectangle { x: 2; y: 0; width: 8; height: 8; color: "transparent"; border.color: parent.parent.ink; border.width: 1; radius: 1.5 }
                        Rectangle { x: 0; y: 2; width: 8; height: 8; color: Theme.surface; border.color: parent.parent.ink; border.width: 1; radius: 1.5 }
                    }
                    Item {
                        visible: btn.isClose
                        anchors.fill: parent
                        Rectangle { anchors.centerIn: parent; width: 13; height: 1; rotation: 45; color: parent.parent.ink; antialiasing: true }
                        Rectangle { anchors.centerIn: parent; width: 13; height: 1; rotation: -45; color: parent.parent.ink; antialiasing: true }
                    }
                }

                MouseArea {
                    id: ma
                    anchors.fill: parent
                    enabled: !btn.isMax          // maximize clicks arrive through the native path
                    hoverEnabled: true
                    onClicked: btn.isClose ? WindowChrome.close() : WindowChrome.minimize()
                }
            }
        }
    }
}
