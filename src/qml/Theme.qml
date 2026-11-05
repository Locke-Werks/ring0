// SPDX-License-Identifier: GPL-3.0-or-later
pragma Singleton
import QtQuick

// Carried over from the Direct2D build: black canvas, crimson accent,
// lowercase type, one hue per view.
QtObject {
    readonly property color canvas:   "#000000"
    readonly property color surface:  "#0e0e0e"
    readonly property color surface2: "#161616"
    readonly property color line:     "#262626"
    readonly property color text:     "#ececec"
    readonly property color dim:      "#8a8a8a"
    readonly property color faint:    "#4a4a4a"
    readonly property color accent:   "#d6262a"

    readonly property var views: [
        { key: "processes", hue: "#d6262a" },
        { key: "cpu",       hue: "#e8453c" },
        { key: "memory",    hue: "#b36bff" },
        { key: "disk",      hue: "#3fb27f" },
        { key: "network",   hue: "#3a9be8" },
        { key: "gpu",       hue: "#f0a030" },
        { key: "forensic",  hue: "#9a9a9a" }
    ]

    readonly property string font: "Segoe UI Variable Text"
    readonly property string mono: "Cascadia Mono"
    readonly property int railWidth: 184
    readonly property int radius: 6

    function bytes(v) {
        const u = ["b", "kb", "mb", "gb", "tb"];
        let i = 0;
        while (v >= 1024 && i < u.length - 1) { v /= 1024; ++i; }
        return (i === 0 ? v.toFixed(0) : v.toFixed(1)) + " " + u[i];
    }
    function rate(v) { return bytes(v) + "/s"; }
}
