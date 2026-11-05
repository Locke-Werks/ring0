// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import Ring0

Item {
    property string filterText
    property int filterScope
    Text { anchors.centerIn: parent; text: "Processes".toLowerCase(); color: Theme.faint; font.pixelSize: 18 }
}
