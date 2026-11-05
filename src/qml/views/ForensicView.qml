// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Ring0

// Who a process is (image, signer, command line) and what its threads are
// doing right now. Stacks are folded by top frame so the hot path is the first
// thing read, not the hundredth.
Item {
    id: view
    readonly property color hue: Theme.views[6].hue
    property int pid: -1
    property var info: ({})
    property var threads: []
    property string error: ""
    property bool busy: false

    function inspect(p, row) {
        pid = p
        info = row || {}
        threads = []
        error = ""
        if (p > 0) { busy = true; ProcessActions.captureStacks(p) }
    }

    Connections {
        target: ProcessActions
        function onStacksReady(p, t) {
            if (p !== view.pid) return
            view.busy = false
            // Group threads by their top resolved frame, busiest group first.
            const groups = {}
            for (const th of t) {
                const top = th.frames.length ? (th.frames[0].module + "!" + (th.frames[0].symbol || th.frames[0].address)) : "(no frames)"
                ;(groups[top] = groups[top] || { top: top, tids: [], frames: th.frames }).tids.push(th.tid)
            }
            view.threads = Object.values(groups).sort((a, b) => b.tids.length - a.tids.length)
        }
        function onStacksFailed(p, msg) { if (p === view.pid) { view.busy = false; view.error = msg } }
    }

    Text {
        visible: view.pid <= 0
        anchors.centerIn: parent
        text: "double-click a process, or choose “inspect stacks”"
        color: Theme.faint
        font.family: Theme.font
        font.pixelSize: 14
    }

    ColumnLayout {
        visible: view.pid > 0
        anchors.fill: parent
        anchors.margins: 20
        spacing: 14

        Panel {
            Layout.fillWidth: true
            Layout.preferredHeight: 112
            hue: view.hue
            readonly property string trust: ProcessActions.trust(view.info.path || "") + (tick.n, "")
            QtObject { id: tick; property int n: 0 }
            Connections { target: ProcessActions; function onTrustChanged() { tick.n++ } }
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 16
                spacing: 6
                RowLayout {
                    spacing: 12
                    Text { text: view.info.name || ("pid " + view.pid); color: Theme.text; font.family: Theme.font; font.pixelSize: 20 }
                    Text { text: "pid " + view.pid; color: Theme.dim; font.family: Theme.mono; font.pixelSize: 12 }
                    Text {
                        text: parent.parent.parent.trust === "" ? "verifying…" : parent.parent.parent.trust + (ProcessActions.signer(view.info.path || "") ? "  ·  " + ProcessActions.signer(view.info.path || "") : "")
                        color: parent.parent.parent.trust === "invalid" ? Theme.accent : Theme.dim
                        font.family: Theme.font; font.pixelSize: 12
                    }
                }
                Text { Layout.fillWidth: true; text: view.info.path || ""; color: Theme.dim; font.family: Theme.mono; font.pixelSize: 11; elide: Text.ElideMiddle }
                Text { Layout.fillWidth: true; text: view.info.commandLine || ""; color: Theme.faint; font.family: Theme.mono; font.pixelSize: 11; elide: Text.ElideRight }
            }
        }

        Panel {
            Layout.fillWidth: true
            Layout.fillHeight: true
            hue: view.hue
            title: view.busy ? "capturing stacks…" : view.error ? "stacks unavailable: " + view.error : view.threads.length + " distinct stacks"
            ListView {
                anchors.fill: parent
                anchors.margins: 8
                clip: true
                spacing: 8
                model: view.threads
                ScrollBar.vertical: ScrollBar { contentItem: Rectangle { implicitWidth: 4; radius: 2; color: Theme.faint } }
                add: Transition { NumberAnimation { property: "opacity"; from: 0; to: 1; duration: 250 } }
                delegate: Rectangle {
                    id: grp
                    required property var modelData
                    property bool open: false
                    width: ListView.view.width
                    height: col.implicitHeight + 16
                    radius: 4
                    color: Theme.surface2
                    Behavior on height { NumberAnimation { duration: 200; easing.type: Easing.OutCubic } }
                    clip: true
                    Column {
                        id: col
                        x: 10; y: 8
                        width: parent.width - 20
                        spacing: 3
                        Row {
                            spacing: 10
                            Text { text: grp.modelData.tids.length + "×"; color: view.hue; font.family: Theme.mono; font.pixelSize: 12 }
                            Text { text: grp.modelData.top; color: Theme.text; font.family: Theme.mono; font.pixelSize: 12 }
                        }
                        Repeater {
                            model: grp.open ? grp.modelData.frames : []
                            delegate: Text {
                                required property var modelData
                                leftPadding: 28
                                text: modelData.module + "!" + (modelData.symbol || modelData.address) + (modelData.offset ? "+0x" + modelData.offset.toString(16) : "")
                                color: Theme.dim
                                font.family: Theme.mono
                                font.pixelSize: 11
                            }
                        }
                    }
                    TapHandler { onTapped: grp.open = !grp.open }
                }
            }
        }
    }
}
