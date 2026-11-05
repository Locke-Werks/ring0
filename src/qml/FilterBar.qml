// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts
import Ring0

Rectangle {
    id: bar
    color: Theme.canvas
    property color hue: Theme.accent
    property int scope: 0
    signal textEdited(string text)
    signal scopePicked(int scope)
    function focusInput() { input.forceActiveFocus(); input.selectAll() }

    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.line }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 16
        anchors.rightMargin: 16
        spacing: 8

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 34
            radius: Theme.radius
            color: Theme.surface
            border.color: input.activeFocus ? bar.hue : Theme.line
            Behavior on border.color { ColorAnimation { duration: 120 } }

            TextInput {
                id: input
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 12
                verticalAlignment: TextInput.AlignVCenter
                color: Theme.text
                selectionColor: bar.hue
                font.family: Theme.font
                font.pixelSize: 14
                onTextChanged: bar.textEdited(text)
                Keys.onEscapePressed: { text = ""; focus = false }
            }
            Text {
                anchors.fill: input
                verticalAlignment: Text.AlignVCenter
                visible: !input.text && !input.activeFocus
                text: "filter  ·  name, pid:N, session:N"
                color: Theme.faint
                font: input.font
            }
        }

        Repeater {
            model: ["all", "user", "system"]
            delegate: Rectangle {
                id: pill
                required property int index
                required property string modelData
                readonly property bool on: bar.scope === index
                Layout.preferredWidth: 72
                Layout.preferredHeight: 30
                radius: 15
                color: on ? bar.hue : "transparent"
                border.color: on ? bar.hue : Theme.faint
                Behavior on color { ColorAnimation { duration: 120 } }
                Text {
                    anchors.centerIn: parent
                    text: pill.modelData
                    color: pill.on ? "#ffffff" : Theme.dim
                    font.family: Theme.font
                    font.pixelSize: 13
                }
                TapHandler { onTapped: { bar.scope = pill.index; bar.scopePicked(pill.index) } }
            }
        }
    }
}
