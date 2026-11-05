// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts
import QtQuick.Window
import Ring0

Window {
    id: win
    width: 1440
    height: 900
    minimumWidth: 900
    minimumHeight: 560
    visible: true
    color: Theme.canvas
    title: "ring0"

    property int view: 0
    property string filterText: ""
    property int filterScope: 0   // 0 all, 1 user, 2 system

    Shortcut { sequence: "Ctrl+F"; onActivated: filter.focusInput() }
    Repeater {
        model: 7
        delegate: Item {
            required property int index
            Shortcut { sequence: "Ctrl+" + (index + 1); onActivated: win.view = index }
        }
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        Rail {
            Layout.fillHeight: true
            Layout.preferredWidth: Theme.railWidth
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

                ProcessesView { filterText: win.filterText; filterScope: win.filterScope }
                CpuView {}
                MemoryView {}
                DiskView {}
                NetworkView {}
                GpuView {}
                ForensicView {}
            }

            Scrubber {
                Layout.fillWidth: true
                Layout.preferredHeight: 64
                hue: Theme.views[win.view].hue
            }
        }
    }
}
