// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Ring0

Item {
    id: view
    property string filterText
    property int filterScope
    readonly property color hue: Theme.views[0].hue
    property int selectedPid: -1
    signal inspect(int pid, var info)

    // Column scales: cpu and gpu are percent; memory scales against the largest
    // working set so the bar says "share of the biggest", not of RAM.
    // Re-read on every sample: the first evaluation runs before any snapshot exists.
    readonly property real memScale: SnapshotBridge.seq >= 0 ? Math.max(256 * 1048576, (SnapshotBridge.memory().total || 0) / 16) : 1

    // Lowest-priority columns drop first as the window narrows, so the name
    // column never gets squeezed into ellipses.
    readonly property bool showGpu:  width >= 980
    readonly property bool showNet:  width >= 860
    readonly property bool showDisk: width >= 740
    readonly property bool showPid:  width >= 620
    readonly property var columns: [
        { key: "name",   label: "name",    width: -1 },
        { key: "pid",    label: "pid",     width: 70,  show: showPid },
        { key: "cpu",    label: "cpu",     width: 96 },
        { key: "memory", label: "memory",  width: 110 },
        { key: "disk",   label: "disk",    width: 104, show: showDisk },
        { key: "net",    label: "network", width: 104, show: showNet },
        { key: "gpu",    label: "gpu",     width: 90,  show: showGpu }
    ].filter(c => c.show !== false)

    FilterProxy {
        id: proxy
        sourceModel: ProcessModel {}
        filterText: view.filterText
        scope: view.filterScope
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 20
        anchors.bottomMargin: 8
        spacing: 0

        // Header. The active column carries the hue and an arrow; the underline
        // slides between columns so the sort change is visible as it happens.
        Item {
            id: header
            Layout.fillWidth: true
            Layout.preferredHeight: 34
            property real underlineX: 0
            property real underlineW: 0
            Behavior on underlineX { NumberAnimation { duration: 220; easing.type: Easing.OutCubic } }
            Behavior on underlineW { NumberAnimation { duration: 220; easing.type: Easing.OutCubic } }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 44
                anchors.rightMargin: 10
                spacing: 0
                Repeater {
                    model: view.columns
                    delegate: Item {
                        id: col
                        required property var modelData
                        readonly property bool active: proxy.sortKey === modelData.key
                        Layout.fillWidth: modelData.width < 0
                        Layout.preferredWidth: modelData.width < 0 ? 200 : modelData.width
                        Layout.fillHeight: true
                        onActiveChanged: if (active) header.place(col)
                        Component.onCompleted: if (active) header.place(col)
                        onXChanged: if (active) header.place(col)
                        onWidthChanged: if (active) header.place(col)
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.right: col.modelData.key === "name" ? undefined : parent.right
                            anchors.rightMargin: 8
                            text: col.modelData.label + (col.active ? (proxy.descending ? "  ↓" : "  ↑") : "")
                            color: col.active ? view.hue : (ma.containsMouse ? Theme.text : Theme.dim)
                            font.family: Theme.font
                            font.pixelSize: 12
                            font.letterSpacing: 0.4
                        }
                        MouseArea {
                            id: ma
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                if (col.active) proxy.descending = !proxy.descending
                                else { proxy.sortKey = col.modelData.key; proxy.descending = col.modelData.key !== "name" }
                            }
                        }
                    }
                }
            }
            function place(c) {
                const p = c.mapToItem(header, 0, 0)
                underlineX = p.x; underlineW = c.width
            }
            Rectangle { y: parent.height - 2; x: header.underlineX; width: header.underlineW; height: 2; radius: 1; color: view.hue }
            Rectangle { y: parent.height - 1; width: parent.width; height: 1; color: Theme.line }
        }

        ListView {
            id: list
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: proxy
            boundsBehavior: Flickable.StopAtBounds
            reuseItems: true
            cacheBuffer: 400
            ScrollBar.vertical: ScrollBar {
                policy: ScrollBar.AsNeeded
                contentItem: Rectangle { implicitWidth: 4; radius: 2; color: Theme.faint }
            }

            // Arrivals fade in, exits fade out, reorders glide: the table never jumps.
            add: Transition { NumberAnimation { property: "opacity"; from: 0; to: 1; duration: 350 } }
            remove: Transition { NumberAnimation { property: "opacity"; to: 0; duration: 250 } }
            displaced: Transition { NumberAnimation { property: "y"; duration: 300; easing.type: Easing.OutCubic } }
            move: Transition { NumberAnimation { property: "y"; duration: 300; easing.type: Easing.OutCubic } }

            delegate: Item {
                id: row
                required property int index
                required property int pid
                required property string name
                required property string path
                required property string commandLine
                required property double cpu
                required property double memory
                required property double disk
                required property double net
                required property double gpu
                required property bool suspended
                required property bool isSystem
                readonly property bool selected: view.selectedPid === pid
                readonly property string trust: ProcessActions.trust(path) + (trustTick.n, "")

                width: list.width
                height: 34

                QtObject { id: trustTick; property int n: 0 }
                Connections { target: ProcessActions; function onTrustChanged() { trustTick.n++ } }

                Rectangle {
                    anchors.fill: parent
                    anchors.leftMargin: 2; anchors.rightMargin: 2
                    radius: 4
                    color: row.selected ? Qt.rgba(view.hue.r, view.hue.g, view.hue.b, 0.13)
                         : hover.hovered ? "#101010" : "transparent"
                    Behavior on color { ColorAnimation { duration: 120 } }
                }
                Rectangle {
                    x: 2; width: 3; radius: 1.5
                    anchors.verticalCenter: parent.verticalCenter
                    height: row.selected ? 20 : 0
                    color: view.hue
                    Behavior on height { NumberAnimation { duration: 160; easing.type: Easing.OutCubic } }
                }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 14
                    anchors.rightMargin: 10
                    spacing: 0
                    opacity: row.suspended ? 0.45 : 1
                    Behavior on opacity { NumberAnimation { duration: 300 } }

                    Image {
                        Layout.preferredWidth: 18; Layout.preferredHeight: 18
                        Layout.rightMargin: 12
                        source: row.path ? "image://procicon/" + encodeURIComponent(row.path) : ""
                        sourceSize: Qt.size(32, 32)
                        asynchronous: true
                        smooth: true
                        opacity: status === Image.Ready ? 1 : 0
                        Behavior on opacity { NumberAnimation { duration: 200 } }
                    }
                    Item {
                        Layout.fillWidth: true
                        Layout.preferredWidth: 200
                        Layout.fillHeight: true
                        Row {
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 8
                            Text {
                                text: row.name
                                color: row.isSystem ? Theme.dim : Theme.text
                                font.family: Theme.font
                                font.pixelSize: 13
                            }
                            // Signature mark: present only when it says something.
                            Rectangle {
                                visible: row.trust === "unsigned" || row.trust === "invalid"
                                anchors.verticalCenter: parent.verticalCenter
                                width: tag.implicitWidth + 10; height: 16; radius: 3
                                color: "transparent"
                                border.color: row.trust === "invalid" ? Theme.accent : Theme.faint
                                Text { id: tag; anchors.centerIn: parent; text: row.trust; color: row.trust === "invalid" ? Theme.accent : Theme.dim; font.family: Theme.font; font.pixelSize: 10 }
                            }
                            Text {
                                visible: row.suspended
                                anchors.verticalCenter: parent.verticalCenter
                                text: "suspended"; color: Theme.dim; font.family: Theme.font; font.pixelSize: 11
                            }
                        }
                    }
                    Text {
                        visible: view.showPid
                        Layout.preferredWidth: 70
                        horizontalAlignment: Text.AlignRight
                        rightPadding: 8
                        text: row.pid
                        color: Theme.faint
                        font.family: Theme.mono
                        font.pixelSize: 12
                    }
                    MetricCell { Layout.preferredWidth: 96;  Layout.fillHeight: true; hue: Theme.views[1].hue; value: row.cpu;    scale: 100; text: row.cpu.toFixed(1) + "%" }
                    MetricCell { Layout.preferredWidth: 110; Layout.fillHeight: true; hue: Theme.views[2].hue; value: row.memory; scale: view.memScale; text: Theme.bytes(row.memory) }
                    MetricCell { visible: view.showDisk; Layout.preferredWidth: 104; Layout.fillHeight: true; hue: Theme.views[3].hue; value: row.disk;   scale: 50 * 1048576; text: row.disk > 0 ? Theme.rate(row.disk) : "0" }
                    MetricCell { visible: view.showNet; Layout.preferredWidth: 104; Layout.fillHeight: true; hue: Theme.views[4].hue; value: row.net;    scale: 12 * 1048576; text: row.net > 0 ? Theme.rate(row.net) : "0" }
                    MetricCell { visible: view.showGpu; Layout.preferredWidth: 90;  Layout.fillHeight: true; hue: Theme.views[5].hue; value: row.gpu;    scale: 100; text: row.gpu.toFixed(1) + "%" }
                }

                HoverHandler { id: hover }
                TapHandler {
                    acceptedButtons: Qt.LeftButton
                    onTapped: view.selectedPid = row.pid
                    onDoubleTapped: view.inspect(row.pid, { name: row.name, path: row.path, commandLine: row.commandLine })
                }
                TapHandler {
                    acceptedButtons: Qt.RightButton
                    onTapped: {
                        view.selectedPid = row.pid
                        menu.pid = row.pid
                        menu.path = row.path
                        menu.name = row.name
                        menu.cmd = row.commandLine
                        menu.suspended = row.suspended
                        menu.priority = ProcessActions.priority(row.pid)
                        menu.popup()
                    }
                }
            }
        }

        // Footer: what the filter is hiding, so an empty-looking list is never a mystery.
        Text {
            Layout.topMargin: 6
            text: proxy.count + " of " + SnapshotBridge.processCount + " processes"
            color: Theme.faint
            font.family: Theme.font
            font.pixelSize: 11
        }
    }

    Menu {
        id: menu
        property int pid: -1
        property string path
        property string name
        property string cmd
        property bool suspended
        property int priority: -1

        background: Rectangle { implicitWidth: 220; color: Theme.surface2; border.color: Theme.line; radius: Theme.radius }
        enter: Transition { NumberAnimation { property: "opacity"; from: 0; to: 1; duration: 120 } }
        exit: Transition { NumberAnimation { property: "opacity"; to: 0; duration: 90 } }
        delegate: MenuItem {
            id: mi
            contentItem: Text {
                text: mi.text
                color: mi.enabled ? (mi.highlighted ? "#ffffff" : Theme.text) : Theme.faint
                font.family: Theme.font
                font.pixelSize: 13
                leftPadding: 6
            }
            background: Rectangle { color: mi.highlighted ? Qt.rgba(view.hue.r, view.hue.g, view.hue.b, 0.35) : "transparent"; radius: 4 }
        }

        MenuItem { text: menu.suspended ? "resume" : "suspend"; onTriggered: menu.suspended ? ProcessActions.resume(menu.pid) : ProcessActions.suspend(menu.pid) }
        MenuItem { text: "end process"; onTriggered: ProcessActions.terminate(menu.pid) }
        MenuSeparator { contentItem: Rectangle { implicitHeight: 1; color: Theme.line } }
        Menu {
            title: "priority"
            background: Rectangle { implicitWidth: 180; color: Theme.surface2; border.color: Theme.line; radius: Theme.radius }
            Repeater {
                model: ["idle", "below normal", "normal", "above normal", "high", "realtime"]
                delegate: MenuItem {
                    required property int index
                    required property string modelData
                    text: (menu.priority === index ? "•  " : "    ") + modelData
                    onTriggered: ProcessActions.setPriority(menu.pid, index)
                }
            }
        }
        MenuItem { text: "restrict to one core"; onTriggered: ProcessActions.singleCore(menu.pid) }
        MenuSeparator { contentItem: Rectangle { implicitHeight: 1; color: Theme.line } }
        MenuItem { text: "open file location"; enabled: menu.path !== ""; onTriggered: ProcessActions.openLocation(menu.path) }
        MenuItem { text: "copy pid"; onTriggered: ProcessActions.copyText(String(menu.pid)) }
        MenuItem { text: "inspect stacks"; onTriggered: view.inspect(menu.pid, { name: menu.name, path: menu.path, commandLine: menu.cmd }) }
    }

    Keys.onPressed: (e) => {
        if (e.key === Qt.Key_Delete && view.selectedPid > 0) { ProcessActions.terminate(view.selectedPid); e.accepted = true }
    }
}
