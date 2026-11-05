// SPDX-License-Identifier: GPL-3.0-or-later
#version 440

layout(location = 0) in float vKind;
layout(location = 1) in vec2 vD;

layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    float age;
    float pxPerMs;
    float halfWidth;
    vec4 color;
    vec2 size;
    float top;
    float fillOn;
    float glow;
    float dotRadius;
    float glowRadius;
    float edgeValue;
};

void main()
{
    float a;
    if (vKind < 0.5) {
        // vD.x runs 0 at the baseline to 1 at the line.
        a = 0.45 * vD.x * fillOn;
    } else if (vKind < 1.5) {
        a = clamp(halfWidth + 0.5 - abs(vD.x), 0.0, 1.0);
    } else if (vKind < 2.5) {
        // Quadratic falloff reads as light rather than a second, blurry stroke.
        float k = clamp(1.0 - abs(vD.x) / (glowRadius + 1.0), 0.0, 1.0);
        a = glow * k * k;
    } else {
        a = clamp(dotRadius + 0.5 - length(vD), 0.0, 1.0);
    }
    a *= color.a * qt_Opacity;
    fragColor = vec4(color.rgb * a, a);
}
