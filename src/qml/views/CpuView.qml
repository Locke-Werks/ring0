// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Ring0

Item {
    id: view
    readonly property color hue: Theme.views[1].hue
    // Kernel time is drawn in a deeper shade of the same hue, so the split reads
    // as one quantity divided rather than two unrelated series.
    readonly property color kernelHue: Qt.darker(hue, 1.9)
    readonly property int cores: SnapshotBridge.coreCount

    function uptime(sec) {
        const d = Math.floor(sec / 86400), h = Math.floor(sec % 86400 / 3600), m = Math.floor(sec % 3600 / 60)
        return (d > 0 ? d + "d " : "") + h + "h " + (m < 10 ? "0" : "") + m + "m"
    }
    function count(n) { return n >= 10000 ? (n / 1000).toFixed(1) + "k" : Math.round(n).toString() }

    // Core selection for the evacuate menu. Reassigned on every change so tile
    // bindings re-evaluate; a sparse object keyed by index.
    property var selected: ({})
    property int anchorCore: -1
    readonly property var evacuated: CoreControl.evacuated
    function isSelected(i) { return selected[i] === true }
    function isEvacuated(i) { return evacuated.indexOf(i) >= 0 }
    function selectedList() { return Object.keys(selected).map(Number).sort((a, b) => a - b) }
    function selectOnly(i) { const s = {}; s[i] = true; selected = s; anchorCore = i }
    function toggle(i) { const s = Object.assign({}, selected); if (s[i]) delete s[i]; else s[i] = true; selected = s; anchorCore = i }
    function clearSelection() { selected = ({}); anchorCore = -1 }
    function selectRange(a, b, add) {
        const s = add ? Object.assign({}, selected) : {}
        for (let i = Math.min(a, b); i <= Math.max(a, b); ++i) s[i] = true
        selected = s
    }
    function selectBox(a, b) {
        const c = grid.cols
        const r0 = Math.min(Math.floor(a / c), Math.floor(b / c)), r1 = Math.max(Math.floor(a / c), Math.floor(b / c))
        const c0 = Math.min(a % c, b % c), c1 = Math.max(a % c, b % c)
        const s = {}
        for (let r = r0; r <= r1; ++r)
            for (let k = c0; k <= c1; ++k)
                if (r * c + k < cores) s[r * c + k] = true
        selected = s
    }
    // Evacuating has to leave at least one core for ordinary work.
    function canEvacuate(list) {
        if (!list.length) return false
        const union = new Set(evacuated)
        list.forEach(i => union.add(i))
        return union.size < cores
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 20
        spacing: 14

        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: 210
            Layout.maximumHeight: 210
            spacing: 14

            Panel {
                Layout.preferredWidth: 380
                Layout.fillHeight: true
                hue: view.hue
                heat: SnapshotBridge.cpuTotal / 100
                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 16
                    spacing: 10
                    RowLayout {
                        spacing: 22
                        Readout { label: "utilisation"; value: SnapshotBridge.cpuTotal; unit: "%"; hue: view.hue; size: 44 }
                        Column {
                            Layout.alignment: Qt.AlignBottom
                            spacing: 4
                            Row {
                                spacing: 6
                                Rectangle { width: 8; height: 8; radius: 2; color: view.hue; anchors.verticalCenter: parent.verticalCenter }
                                Text { text: "user " + Math.max(0, SnapshotBridge.cpuTotal - SnapshotBridge.cpuKernel).toFixed(0) + "%"; color: Theme.dim; font.family: Theme.font; font.pixelSize: 12 }
                            }
                            Row {
                                spacing: 6
                                Rectangle { width: 8; height: 8; radius: 2; color: view.kernelHue; anchors.verticalCenter: parent.verticalCenter }
                                Text { text: "kernel " + SnapshotBridge.cpuKernel.toFixed(0) + "%"; color: Theme.dim; font.family: Theme.font; font.pixelSize: 12 }
                            }
                        }
                    }
                    Item { Layout.fillHeight: true }
                    GridLayout {
                        columns: 4
                        columnSpacing: 18
                        rowSpacing: 6
                        Repeater {
                            model: [
                                { k: "threads", v: () => view.count(SnapshotBridge.threadCount) },
                                { k: "handles", v: () => view.count(SnapshotBridge.handleCount) },
                                { k: "up", v: () => view.uptime(SnapshotBridge.uptimeSeconds) },
                                { k: "temp", v: () => SnapshotBridge.cpuTempC >= 0 ? SnapshotBridge.cpuTempC.toFixed(0) + "°C" : "n/a" },
                                { k: "power", v: () => SnapshotBridge.cpuPowerW >= 0 ? SnapshotBridge.cpuPowerW.toFixed(0) + " W" : "n/a" },
                                { k: "clock", v: () => SnapshotBridge.avgClockMhz > 0 ? (SnapshotBridge.avgClockMhz / 1000).toFixed(2) + " GHz" : "n/a" },
                                { k: "chiplets", v: () => SnapshotBridge.ccdTemps.length ? SnapshotBridge.ccdTemps.map(t => t.toFixed(0) + "°").join("  ") : "n/a" }
                            ]
                            delegate: Column {
                                required property var modelData
                                spacing: 1
                                Text { text: modelData.k; color: Theme.faint; font.family: Theme.font; font.pixelSize: 11 }
                                Text {
                                    text: SnapshotBridge.seq >= 0 ? modelData.v() : ""
                                    color: Theme.text; font.family: Theme.font; font.pixelSize: 14
                                    font.features: { "tnum": 1 }
                                }
                            }
                        }
                    }
                }
            }

            Panel {
                Layout.fillWidth: true
                Layout.fillHeight: true
                hue: view.hue
                heat: SnapshotBridge.cpuTotal / 100
                title: "total  ·  kernel beneath  ·  last 2 min"
                Sparkline { anchors.fill: parent; anchors.margins: 1; metric: "cpu";  samples: 120; color: view.hue; maxValue: 100; lineWidth: 2 }
                Sparkline { anchors.fill: parent; anchors.margins: 1; metric: "cpuk"; samples: 120; color: view.kernelHue; maxValue: 100; lineWidth: 1 }
            }
        }

        // Every core against time: "which cores, and when" in one glance, where
        // the tiles below answer "how much, right now".
        Panel {
            Layout.fillWidth: true
            Layout.preferredHeight: Math.max(56, Math.min(view.cores * 4 + 38, 170))
            Layout.maximumHeight: Layout.preferredHeight
            hue: view.hue
            title: "cores × time"
            Heatmap {
                anchors.fill: parent
                anchors.margins: 1
                source: "cores"
                samples: 120
                hue: view.hue
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 12

            // Power cap. The far right of the track means uncapped; the cap only
            // engages when the handle is released, not on every pixel of a drag.
            Text { text: "power cap"; color: Theme.faint; font.family: Theme.font; font.pixelSize: 12 }
            Slider {
                id: capSlider
                readonly property int uncapped: 205
                Layout.preferredWidth: 240
                enabled: PowerCap.available
                from: 40; to: uncapped; stepSize: 5
                value: PowerCap.target > 0 ? PowerCap.target : uncapped
                onPressedChanged: if (!pressed) PowerCap.setTarget(value >= uncapped ? 0 : value)
                background: Rectangle {
                    x: capSlider.leftPadding
                    y: capSlider.topPadding + capSlider.availableHeight / 2 - height / 2
                    width: capSlider.availableWidth; height: 4; radius: 2
                    color: Theme.line
                    Rectangle {
                        width: capSlider.visualPosition * parent.width; height: parent.height; radius: 2
                        color: capSlider.value >= capSlider.uncapped ? Theme.faint : view.hue
                    }
                }
                handle: Rectangle {
                    x: capSlider.leftPadding + capSlider.visualPosition * (capSlider.availableWidth - width)
                    y: capSlider.topPadding + capSlider.availableHeight / 2 - height / 2
                    width: 14; height: 14; radius: 7
                    color: capSlider.pressed ? "#ffffff" : Theme.text
                }
            }
            Text {
                text: capSlider.value >= capSlider.uncapped ? "uncapped"
                      : capSlider.value.toFixed(0) + " W"
                        + (PowerCap.target > 0 && !capSlider.pressed
                           ? "  ·  now " + (SnapshotBridge.cpuPowerW >= 0 ? SnapshotBridge.cpuPowerW.toFixed(0) + " W" : "n/a")
                             + "  ·  max state " + PowerCap.maxState + "%" + (PowerCap.boost ? "" : ", boost off")
                             + (PowerCap.atFloor && SnapshotBridge.cpuPowerW > PowerCap.target * 1.03 ? "  ·  lowest step, cannot go lower" : "")
                           : "")
                color: PowerCap.target > 0 ? Theme.text : Theme.dim
                font.family: Theme.font
                font.pixelSize: 12
                font.features: { "tnum": 1 }
            }
            Item { Layout.fillWidth: true }
            Text {
                visible: view.evacuated.length > 0
                text: view.evacuated.length + (view.evacuated.length === 1 ? " core" : " cores") + " evacuated  ·  "
                      + CoreControl.moved + " processes moved  ·  " + CoreControl.refused + " refused (protected)"
                      + "  ·  kernel work still runs there"
                color: Theme.dim
                font.family: Theme.font
                font.pixelSize: 12
            }
        }

        GridView {
            id: grid
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 90
            clip: true
            model: view.cores
            readonly property int cols: Math.max(2, Math.floor(width / 190))
            readonly property int rows: Math.ceil(view.cores / cols)
            cellWidth: Math.floor(width / cols)
            // Fit every core on screen when there is room; never shrink below a
            // readable tile, and scroll beyond that.
            cellHeight: Math.max(78, Math.min(120, rows > 0 ? Math.floor(height / rows) : 120))
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {
                policy: grid.contentHeight > grid.height ? ScrollBar.AsNeeded : ScrollBar.AlwaysOff
                contentItem: Rectangle { implicitWidth: 4; radius: 2; color: Theme.faint }
            }

            delegate: Item {
                id: cell
                required property int index
                width: grid.cellWidth
                height: grid.cellHeight
                readonly property real load: SnapshotBridge.seq >= 0 ? SnapshotBridge.coreLoad(index) : 0

                Panel {
                    anchors.fill: parent
                    anchors.margins: 4
                    hue: view.hue
                    heat: cell.load / 100

                    Sparkline {
                        anchors.fill: parent
                        anchors.margins: 1
                        anchors.topMargin: 28
                        metric: "core:" + cell.index
                        samples: 60
                        color: view.hue
                        maxValue: 100
                    }
                    Sparkline {
                        anchors.fill: parent
                        anchors.margins: 1
                        anchors.topMargin: 28
                        metric: "corek:" + cell.index
                        samples: 60
                        color: view.kernelHue
                        maxValue: 100
                        lineWidth: 1
                    }
                    Text {
                        // Effective clock, shown only when the CPU reports one.
                        readonly property real mhz: SnapshotBridge.seq >= 0 ? SnapshotBridge.coreClockMhz(cell.index) : -1
                        visible: mhz > 0
                        x: 30; y: 8
                        text: (mhz / 1000).toFixed(2) + " GHz"
                        color: Theme.faint
                        font.family: Theme.mono
                        font.pixelSize: 11
                    }
                    Text {
                        x: 11; y: 8
                        text: cell.index
                        color: Theme.dim
                        font.family: Theme.mono
                        font.pixelSize: 11
                    }
                    Text {
                        anchors.right: parent.right
                        anchors.rightMargin: 11
                        y: 6
                        property real shown: cell.load
                        Behavior on shown { NumberAnimation { duration: 1000; easing.type: Easing.Linear } }
                        text: shown.toFixed(0) + "%"
                        color: Qt.tint(Theme.dim, Qt.rgba(view.hue.r, view.hue.g, view.hue.b, Math.min(1, cell.load / 60)))
                        font.family: Theme.font
                        font.pixelSize: 13
                        font.features: { "tnum": 1 }
                    }
                }

                // Evacuated: dimmed, so the near-idle graph reads as intended.
                Rectangle {
                    anchors.fill: parent
                    anchors.margins: 4
                    radius: Theme.radius
                    visible: view.isEvacuated(cell.index)
                    color: Qt.rgba(0, 0, 0, 0.55)
                    Text {
                        anchors.centerIn: parent
                        text: "evacuated"
                        color: Theme.dim
                        font.family: Theme.font
                        font.pixelSize: 12
                    }
                }
                Rectangle {
                    anchors.fill: parent
                    anchors.margins: 4
                    radius: Theme.radius
                    visible: view.isSelected(cell.index)
                    color: Qt.rgba(view.hue.r, view.hue.g, view.hue.b, 0.10)
                    border.width: 2
                    border.color: view.hue
                }

                MouseArea {
                    anchors.fill: parent
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    // Keeps a drag-select from turning into a flick of the grid.
                    preventStealing: true
                    property bool dragged: false

                    onPressed: (mouse) => {
                        dragged = false
                        if (mouse.button === Qt.RightButton) {
                            if (!view.isSelected(cell.index)) view.selectOnly(cell.index)
                            coreMenu.popup()
                        } else if (mouse.modifiers & Qt.ControlModifier) {
                            view.toggle(cell.index)
                        } else if ((mouse.modifiers & Qt.ShiftModifier) && view.anchorCore >= 0) {
                            view.selectRange(view.anchorCore, cell.index, false)
                        }
                    }
                    onPositionChanged: (mouse) => {
                        if (!(pressedButtons & Qt.LeftButton) || (mouse.modifiers & (Qt.ControlModifier | Qt.ShiftModifier))) return
                        const p = mapToItem(grid.contentItem, mouse.x, mouse.y)
                        const i = grid.indexAt(p.x, p.y)
                        if (i < 0 || (!dragged && i === cell.index)) return
                        dragged = true
                        view.anchorCore = cell.index
                        view.selectBox(cell.index, i)
                    }
                    onClicked: (mouse) => {
                        if (dragged || mouse.button !== Qt.LeftButton || (mouse.modifiers & (Qt.ControlModifier | Qt.ShiftModifier))) return
                        const list = view.selectedList()
                        if (list.length === 1 && list[0] === cell.index) view.clearSelection()
                        else view.selectOnly(cell.index)
                    }
                }
            }
        }
    }

    Menu {
        id: coreMenu
        readonly property var picked: view.selectedList()
        readonly property var pickedEvacuated: picked.filter(i => view.isEvacuated(i))
        readonly property var pickedLive: picked.filter(i => !view.isEvacuated(i))
        readonly property string noun: pickedLive.length === 1 ? "core " + pickedLive[0] : pickedLive.length + " cores"

        background: Rectangle { implicitWidth: 240; color: Theme.surface2; border.color: Theme.line; radius: Theme.radius }
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

        MenuItem {
            text: view.canEvacuate(coreMenu.pickedLive) || !coreMenu.pickedLive.length
                  ? "evacuate " + coreMenu.noun
                  : "evacuate " + coreMenu.noun + " (needs one core left)"
            enabled: view.canEvacuate(coreMenu.pickedLive)
            onTriggered: CoreControl.evacuate(coreMenu.pickedLive)
        }
        MenuItem {
            text: "restore " + (coreMenu.pickedEvacuated.length === 1 ? "core " + coreMenu.pickedEvacuated[0] : coreMenu.pickedEvacuated.length + " cores")
            enabled: coreMenu.pickedEvacuated.length > 0
            onTriggered: CoreControl.restore(coreMenu.pickedEvacuated)
        }
        MenuItem {
            text: "restore all cores"
            enabled: view.evacuated.length > 0
            onTriggered: CoreControl.restoreAll()
        }
        MenuSeparator { contentItem: Rectangle { implicitHeight: 1; color: Theme.line } }
        MenuItem { text: "select all"; onTriggered: view.selectRange(0, view.cores - 1, false) }
        MenuItem { text: "clear selection"; enabled: coreMenu.picked.length > 0; onTriggered: view.clearSelection() }
    }
}
