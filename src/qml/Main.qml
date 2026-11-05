// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts
import QtQuick.Window
import Ring0

Window {
    id: win
    // Open at most 85% of the work area so the first view is never clipped.
    width: Math.min(1440, Screen.desktopAvailableWidth * 0.85)
    height: Math.min(900, Screen.desktopAvailableHeight * 0.85)
    minimumWidth: 900
    minimumHeight: 560
    visible: true
    color: Theme.canvas
    title: "ring0"

    property int view: 0
    property string filterText: ""
    property int filterScope: 0   // 0 all, 1 user, 2 system
    function inspectPid(p) { forensic.inspect(p, {}); view = 6 }

    Shortcut { sequence: "Ctrl+F"; onActivated: filter.focusInput() }
    Repeater {
        model: 7
        delegate: Item {
            required property int index
            Shortcut { sequence: "Ctrl+" + (index + 1); onActivated: win.view = index }
        }
    }

    TitleBar {
        id: titleBar
        anchors.left: parent.left
        anchors.right: parent.right
        viewName: Theme.views[win.view].key
        hue: Theme.views[win.view].hue
    }

    RowLayout {
        anchors.top: titleBar.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        spacing: 0

        Rail {
            Layout.fillHeight: true
            // Compact below ~1100 px: labels only, no readouts or sparklines.
            compact: win.width < 1100
            Layout.preferredWidth: compact ? 120 : Theme.railWidth
            Behavior on Layout.preferredWidth { NumberAnimation { duration: 220; easing.type: Easing.OutCubic } }
            current: win.view
            onSelected: (i) => win.view = i
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            FilterBar {
                id: filter
                Layout.fillWidth: true
                Layout.preferredHeight: 56
                hue: Theme.views[win.view].hue
                onTextEdited: (t) => win.filterText = t
                onScopePicked: (s) => win.filterScope = s
            }

            StackLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                currentIndex: win.view

                ProcessesView {
                    filterText: win.filterText
                    filterScope: win.filterScope
                    onInspect: (pid, info) => { forensic.inspect(pid, info); win.view = 6 }
                }
                CpuView {}
                MemoryView {}
                DiskView {}
                NetworkView {}
                GpuView {}
                ForensicView { id: forensic }
            }

            Scrubber {
                Layout.fillWidth: true
                Layout.preferredHeight: 64
                hue: Theme.views[win.view].hue
            }
        }
    }

    Toast { id: toast }
    Connections {
        target: ProcessActions
        function onFailed(action, pid, message) { toast.show(action + (pid > 0 ? " " + pid : "") + " failed: " + message, true) }
        function onSucceeded(action, pid) { if (action !== "open location") toast.show(action + (pid > 0 ? " " + pid : ""), false) }
    }
}
