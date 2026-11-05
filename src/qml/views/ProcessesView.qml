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
    // Memory rails are relative to the heaviest process, so the biggest consumer
    // reads full and the rest are proportional to it.
    readonly property real memScale: Math.max(64 * 1048576, SnapshotBridge.maxProcessMemory)
    // Same for CPU: one busy core is ~3% of a 32-thread machine, so an absolute
    // scale would never show a rail. The floor keeps idle noise from looking hot.
    readonly property real cpuScale: Math.max(5, SnapshotBridge.maxProcessCpu)

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
            cacheBuffer: 400
            ScrollBar.vertical: ScrollBar {
                policy: ScrollBar.AsNeeded
                contentItem: Rectangle { implicitWidth: 4; radius: 2; color: Theme.faint }
            }

            // No add/remove/displace transitions: with a filtered, re-sorting proxy
            // and pooled delegates they left ghost rows and overlapping text.

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

                    Item {
                        Layout.preferredWidth: 18; Layout.preferredHeight: 18
                        Layout.rightMargin: 12
                        // Pathless processes (System, Registry, protected images)
                        // get a quiet mark so names stay aligned.
                        Rectangle {
                            visible: !row.path
                            anchors.centerIn: parent
                            width: 10; height: 10; radius: 2
                            color: "transparent"
                            border.color: Theme.faint
                        }
                    Image {
                        anchors.fill: parent
                        source: row.path ? "image://procicon/" + encodeURIComponent(row.path) : ""
                        sourceSize: Qt.size(32, 32)
                        asynchronous: true
                        smooth: true
                        opacity: status === Image.Ready ? 1 : 0
                        Behavior on opacity { NumberAnimation { duration: 200 } }
                    }
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
                    MetricCell { Layout.preferredWidth: 96;  Layout.fillHeight: true; hue: Theme.views[1].hue; value: row.cpu;    scale: view.cpuScale; text: row.cpu.toFixed(1) + "%" }
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
        MenuItem { text: "end process…"; onTriggered: confirmEnd.ask(menu.pid, menu.name) }
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


    // Ending a process is irreversible, so it always takes an explicit second click.
    Rectangle {
        id: confirmEnd
        property int pid: -1
        property string name: ""
        function ask(p, n) { pid = p; name = n; visible = true }
        visible: false
        anchors.centerIn: parent
        width: 420; height: 150
        radius: Theme.radius
        color: Theme.surface2
        border.color: Theme.accent
        z: 100
        Column {
            anchors.fill: parent
            anchors.margins: 18
            spacing: 18
            Text {
                width: parent.width
                wrapMode: Text.WordWrap
                text: "end " + confirmEnd.name + " (pid " + confirmEnd.pid + ")? unsaved work in it is lost."
                color: Theme.text
                font.family: Theme.font
                font.pixelSize: 14
            }
            Row {
                anchors.right: parent.right
                spacing: 10
                Rectangle {
                    width: 96; height: 32; radius: 16
                    color: "transparent"; border.color: Theme.faint
                    Text { anchors.centerIn: parent; text: "cancel"; color: Theme.text; font.family: Theme.font; font.pixelSize: 13 }
                    TapHandler { onTapped: confirmEnd.visible = false }
                }
                Rectangle {
                    width: 96; height: 32; radius: 16
                    color: Theme.accent
                    Text { anchors.centerIn: parent; text: "end it"; color: "#ffffff"; font.family: Theme.font; font.pixelSize: 13 }
                    TapHandler { onTapped: { ProcessActions.terminate(confirmEnd.pid); confirmEnd.visible = false } }
                }
            }
        }
    }
}
