// SPDX-License-Identifier: GPL-3.0-or-later
#version 440

layout(location = 0) in vec4 qt_Vertex;
layout(location = 1) in vec2 qt_MultiTexCoord0;

layout(location = 0) out vec2 v_uv;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    float rows;
    float cols;
    float phase;
    vec4 hue;
    float rowPx;
    float gap;
};

void main()
{
    v_uv = qt_MultiTexCoord0;
    gl_Position = qt_Matrix * qt_Vertex;
}
