// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Ring0

// One process, in depth: who it is, where it came from, what it has loaded,
// who it talks to, and what its threads are doing. Findings lead, because
// they are the reason anyone opens this page.
Item {
    id: view
    readonly property color hue: Theme.views[6].hue
    readonly property color alert: Theme.accent
    property int pid: -1
    property var info: ({})       // name/path/commandLine handed over by the processes view
    property var d: ({})          // full detail from ProcessActions.inspect
    property bool loading: false
    property var threads: []
    property string stackError: ""
    property bool stacksBusy: false
    property int tab: 0           // 0 modules, 1 connections, 2 threads

    function inspect(p, row) {
        pid = p
        info = row || {}
        d = {}
        threads = []
        stackError = ""
        if (p > 0) {
            loading = true
            stacksBusy = true
            ProcessActions.inspect(p)
            ProcessActions.captureStacks(p)
        }
    }

    function name() { return info.name || SnapshotBridge.processName(pid) || ("pid " + pid) }
    function age(ft) {
        if (!ft) return ""
        // FILETIME (100 ns since 1601) to a relative age.
        const ms = ft / 10000 - 11644473600000
        const s = Math.max(0, (Date.now() - ms) / 1000)
        if (s < 120) return Math.round(s) + "s ago"
        if (s < 7200) return Math.round(s / 60) + "m ago"
        if (s < 172800) return Math.round(s / 3600) + "h ago"
        return Math.round(s / 86400) + "d ago"
    }

    Connections {
        target: ProcessActions
        function onInspected(p, detail) { if (p === view.pid) { view.d = detail; view.loading = false } }
        function onStacksReady(p, t) {
            if (p !== view.pid) return
            view.stacksBusy = false
            const groups = {}
            for (const th of t) {
                const top = th.frames.length ? (th.frames[0].module + "!" + (th.frames[0].symbol || th.frames[0].address)) : "(no frames)"
                ;(groups[top] = groups[top] || { top: top, tids: [], frames: th.frames }).tids.push(th.tid)
            }
            view.threads = Object.values(groups).sort((a, b) => b.tids.length - a.tids.length)
        }
        function onStacksFailed(p, msg) { if (p === view.pid) { view.stacksBusy = false; view.stackError = msg } }
    }

    Column {
        visible: view.pid <= 0
        anchors.centerIn: parent
        spacing: 10
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: "no process selected"
            color: Theme.dim; font.family: Theme.font; font.pixelSize: 16
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: "double-click a process, or right-click it and choose “inspect”"
            color: Theme.faint; font.family: Theme.font; font.pixelSize: 13
        }
    }

    ColumnLayout {
        visible: view.pid > 0
        anchors.fill: parent
        anchors.margins: 20
        spacing: 14

        // Identity.
        Panel {
            Layout.fillWidth: true
            Layout.preferredHeight: idCol.implicitHeight + 32
            hue: view.hue
            ColumnLayout {
                id: idCol
                anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
                anchors.margins: 16
                spacing: 8
                RowLayout {
                    spacing: 12
                    Image {
                        Layout.preferredWidth: 28; Layout.preferredHeight: 28
                        source: (view.d.path || view.info.path) ? "image://procicon/" + encodeURIComponent(view.d.path || view.info.path) : ""
                        sourceSize: Qt.size(64, 64)
                        asynchronous: true
                    }
                    Text { text: view.name(); color: Theme.text; font.family: Theme.font; font.pixelSize: 22 }
                    Text { text: "pid " + view.pid; color: Theme.dim; font.family: Theme.mono; font.pixelSize: 12; Layout.alignment: Qt.AlignBaseline }
                    // Signature badge: signer when signed, the problem when not.
                    Rectangle {
                        visible: (view.d.trust || "") !== ""
                        Layout.preferredHeight: 22
                        Layout.preferredWidth: sig.implicitWidth + 18
                        radius: 11
                        color: "transparent"
                        border.color: view.d.trust === "signed" ? Theme.faint : view.alert
                        Text {
                            id: sig
                            anchors.centerIn: parent
                            text: view.d.trust === "signed" ? "signed · " + (view.d.signer || "unknown signer") : (view.d.trust || "")
                            color: view.d.trust === "signed" ? Theme.dim : view.alert
                            font.family: Theme.font; font.pixelSize: 11
                        }
                    }
                    Item { Layout.fillWidth: true }
                    Text { visible: view.loading; text: "inspecting…"; color: Theme.faint; font.family: Theme.font; font.pixelSize: 12 }
                }
                Text {
                    Layout.fillWidth: true
                    text: view.d.path || view.info.path || ""
                    color: Theme.dim; font.family: Theme.mono; font.pixelSize: 11; elide: Text.ElideMiddle
                }
                Text {
                    Layout.fillWidth: true
                    visible: text !== ""
                    text: view.d.commandLine || view.info.commandLine || ""
                    color: Theme.faint; font.family: Theme.mono; font.pixelSize: 11
                    wrapMode: Text.WrapAnywhere; maximumLineCount: 2; elide: Text.ElideRight
                }
                // Facts as chips; the ones that change what a process can do get the hue.
                Flow {
                    Layout.fillWidth: true
                    spacing: 6
                    Repeater {
                        model: [
                            { t: view.d.user || "", hot: false },
                            { t: view.d.integrity ? view.d.integrity + " integrity" : "", hot: view.d.integrity === "high" || view.d.integrity === "system" },
                            { t: view.d.elevated ? "elevated" : "", hot: true },
                            { t: view.d.arch || "", hot: false },
                            { t: view.d.createTime ? "started " + view.age(view.d.createTime) : "", hot: false }
                        ].filter(c => c.t !== "")
                        delegate: Rectangle {
                            required property var modelData
                            height: 22; width: chip.implicitWidth + 16; radius: 4
                            color: modelData.hot ? Qt.rgba(view.hue.r, view.hue.g, view.hue.b, 0.18) : Theme.surface2
                            Text { id: chip; anchors.centerIn: parent; text: parent.modelData.t; color: Theme.text; font.family: Theme.font; font.pixelSize: 11 }
                        }
                    }
                }
                // Where it came from, nearest parent first.
                Text {
                    Layout.fillWidth: true
                    visible: (view.d.ancestry || []).length > 0
                    text: "spawned by  " + (view.d.ancestry || []).map(p => (SnapshotBridge.processName(p) || "?") + " (" + p + ")").join("  ←  ")
                    color: Theme.dim; font.family: Theme.font; font.pixelSize: 12; elide: Text.ElideRight
                }
            }
        }

        // Findings. Empty is a result too, and says so.
        Panel {
            Layout.fillWidth: true
            Layout.preferredHeight: findings.implicitHeight + 24
            hue: (view.d.signals || []).length ? view.alert : view.hue
            heat: (view.d.signals || []).length ? 0.8 : 0
            Column {
                id: findings
                anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
                anchors.margins: 12
                spacing: 6
                Text {
                    visible: !view.loading && (view.d.signals || []).length === 0
                    text: "no findings"
                    color: Theme.dim; font.family: Theme.font; font.pixelSize: 13
                }
                Repeater {
                    model: view.d.signals || []
                    delegate: Row {
                        required property string modelData
                        spacing: 8
                        Rectangle { width: 6; height: 6; radius: 3; color: view.alert; anchors.verticalCenter: parent.verticalCenter }
                        Text { text: modelData; color: Theme.text; font.family: Theme.font; font.pixelSize: 13 }
                    }
                }
                Text {
                    visible: (view.d.error || "") !== ""
                    width: parent.width
                    text: view.d.error || ""
                    color: Theme.faint; font.family: Theme.font; font.pixelSize: 11; wrapMode: Text.Wrap
                }
            }
        }

        // This process's own history.
        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: 92
            Layout.maximumHeight: 92
            spacing: 10
            Repeater {
                model: [
                    { k: "cpu",  l: "cpu",     c: Theme.views[1].hue, max: 100, fmt: (v) => v.toFixed(1) + "%" },
                    { k: "mem",  l: "memory",  c: Theme.views[2].hue, max: -1,  fmt: (v) => Theme.bytes(v) },
                    { k: "disk", l: "disk",    c: Theme.views[3].hue, max: -1,  fmt: (v) => Theme.rate(v) },
                    { k: "net",  l: "network", c: Theme.views[4].hue, max: -1,  fmt: (v) => Theme.bitrate(v) },
                    { k: "gpu",  l: "gpu",     c: Theme.views[5].hue, max: 100, fmt: (v) => v.toFixed(1) + "%" }
                ]
                delegate: Panel {
                    id: tile
                    required property var modelData
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    hue: modelData.c
                    readonly property var series: SnapshotBridge.seq >= 0 && view.pid > 0 ? SnapshotBridge.series("proc:" + view.pid + ":" + modelData.k, 1) : []
                    Sparkline {
                        anchors.fill: parent; anchors.margins: 1; anchors.topMargin: 30
                        metric: view.pid > 0 ? "proc:" + view.pid + ":" + tile.modelData.k : ""
                        samples: 60
                        color: tile.modelData.c
                        maxValue: tile.modelData.max
                        minScale: tile.modelData.k === "mem" ? 64 * 1048576 : 1024
                    }
                    Text { x: 10; y: 8; text: tile.modelData.l; color: Theme.dim; font.family: Theme.font; font.pixelSize: 11 }
                    Text {
                        anchors.right: parent.right; anchors.rightMargin: 10; y: 7
                        text: tile.series.length ? tile.modelData.fmt(tile.series[tile.series.length - 1]) : ""
                        color: Theme.text; font.family: Theme.font; font.pixelSize: 12
                        font.features: { "tnum": 1 }
                    }
                }
            }
        }

        // Detail tabs.
        Panel {
            Layout.fillWidth: true
            Layout.fillHeight: true
            hue: view.hue

            Row {
                id: tabs
                x: 12; y: 10
                spacing: 6
                Repeater {
                    model: [
                        "modules  " + (view.d.modules || []).length,
                        "connections  " + (view.d.connections || []).length,
                        "threads  " + (view.stacksBusy ? "…" : view.threads.length)
                    ]
                    delegate: Rectangle {
                        required property int index
                        required property string modelData
                        readonly property bool on: view.tab === index
                        height: 24; width: tl.implicitWidth + 20; radius: 12
                        color: on ? view.hue : "transparent"
                        border.color: on ? view.hue : Theme.faint
                        Text { id: tl; anchors.centerIn: parent; text: parent.modelData; color: parent.on ? "#000000" : Theme.dim; font.family: Theme.font; font.pixelSize: 11 }
                        TapHandler { onTapped: view.tab = parent.index }
                    }
                }
            }

            StackLayout {
                anchors.fill: parent
                anchors.margins: 10
                anchors.topMargin: 44
                currentIndex: view.tab

                // Modules, problems first.
                ListView {
                    clip: true
                    spacing: 1
                    model: (view.d.modules || []).slice().sort((a, b) => {
                        const r = (m) => (m.trust !== "signed" ? 0 : 2) + (m.userDir ? 0 : 1)
                        return r(a) - r(b) || a.path.localeCompare(b.path)
                    })
                    ScrollBar.vertical: ScrollBar { contentItem: Rectangle { implicitWidth: 4; radius: 2; color: Theme.faint } }
                    delegate: Item {
                        required property var modelData
                        readonly property bool bad: modelData.trust !== "signed" || modelData.userDir
                        width: ListView.view.width
                        height: 26
                        Text {
                            anchors.left: parent.left; anchors.leftMargin: 4
                            anchors.right: mtrust.left; anchors.rightMargin: 10
                            anchors.verticalCenter: parent.verticalCenter
                            text: parent.modelData.path
                            elide: Text.ElideMiddle
                            color: parent.bad ? Theme.text : Theme.dim
                            font.family: Theme.mono; font.pixelSize: 11
                        }
                        Text {
                            id: mtrust
                            anchors.right: parent.right; anchors.rightMargin: 4
                            anchors.verticalCenter: parent.verticalCenter
                            text: (parent.modelData.userDir ? "user dir · " : "") + (parent.modelData.trust === "signed" ? (parent.modelData.signer || "signed") : (parent.modelData.trust || "?"))
                            color: parent.bad ? view.alert : Theme.faint
                            font.family: Theme.font; font.pixelSize: 11
                        }
                    }
                }

                // Connections.
                ListView {
                    clip: true
                    spacing: 1
                    model: view.d.connections || []
                    ScrollBar.vertical: ScrollBar { contentItem: Rectangle { implicitWidth: 4; radius: 2; color: Theme.faint } }
                    header: Text {
                        visible: (view.d.connections || []).length === 0
                        height: visible ? 30 : 0
                        text: view.loading ? "" : "no network endpoints"
                        color: Theme.faint; font.family: Theme.font; font.pixelSize: 12
                    }
                    delegate: RowLayout {
                        required property var modelData
                        width: ListView.view.width
                        height: 24
                        spacing: 12
                        Text { Layout.preferredWidth: 46; text: parent.modelData.proto; color: Theme.dim; font.family: Theme.mono; font.pixelSize: 11 }
                        Text { Layout.preferredWidth: 260; text: parent.modelData.local; color: Theme.dim; font.family: Theme.mono; font.pixelSize: 11; elide: Text.ElideRight }
                        Text { Layout.fillWidth: true; text: parent.modelData.remote ? "→  " + parent.modelData.remote : ""; color: Theme.text; font.family: Theme.mono; font.pixelSize: 11; elide: Text.ElideRight }
                        Text {
                            text: parent.modelData.state
                            color: parent.modelData.state === "established" ? Theme.views[4].hue : Theme.faint
                            font.family: Theme.font; font.pixelSize: 11
                        }
                    }
                }

                // Threads, folded by top frame so the hot path is read first.
                ListView {
                    clip: true
                    spacing: 6
                    model: view.threads
                    ScrollBar.vertical: ScrollBar { contentItem: Rectangle { implicitWidth: 4; radius: 2; color: Theme.faint } }
                    header: Text {
                        visible: view.stacksBusy || view.stackError !== ""
                        height: visible ? 28 : 0
                        text: view.stacksBusy ? "capturing stacks…" : "stacks unavailable: " + view.stackError
                        color: Theme.faint; font.family: Theme.font; font.pixelSize: 12
                    }
                    delegate: Rectangle {
                        id: grp
                        required property var modelData
                        property bool open: false
                        width: ListView.view.width
                        height: col.implicitHeight + 14
                        radius: 4
                        color: Theme.surface2
                        clip: true
                        Behavior on height { NumberAnimation { duration: 200; easing.type: Easing.OutCubic } }
                        Column {
                            id: col
                            x: 10; y: 7
                            width: parent.width - 20
                            spacing: 3
                            Row {
                                spacing: 10
                                Text { text: grp.open ? "▾" : "▸"; color: Theme.faint; font.pixelSize: 11 }
                                Text { text: grp.modelData.tids.length + "×"; color: view.hue; font.family: Theme.mono; font.pixelSize: 12 }
                                Text { text: grp.modelData.top; color: Theme.text; font.family: Theme.mono; font.pixelSize: 12 }
                            }
                            Repeater {
                                model: grp.open ? grp.modelData.frames : []
                                delegate: Text {
                                    required property var modelData
                                    leftPadding: 32
                                    text: modelData.module + "!" + (modelData.symbol || modelData.address) + (modelData.offset ? "+0x" + modelData.offset.toString(16) : "")
                                    color: Theme.dim; font.family: Theme.mono; font.pixelSize: 11
                                }
                            }
                        }
                        TapHandler { onTapped: grp.open = !grp.open }
                    }
                }
            }
        }
    }
}
