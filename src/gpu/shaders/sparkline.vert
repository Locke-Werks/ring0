// SPDX-License-Identifier: GPL-3.0-or-later
#version 440

// a0: A slot, B slot (counted in samples from the right edge), kind, side
// a1: A prev y, A cur y, B prev y, B cur y, in item pixels
// a2: endpoint (0 A, 1 B), dot corner x, dot corner y
// kind: 0 fill, 1 line, 2 glow, 3 newest-sample dot
layout(location = 0) in vec4 a0;
layout(location = 1) in vec4 a1;
layout(location = 2) in vec3 a2;

layout(location = 0) out float vKind;
layout(location = 1) out vec2 vD;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    float t;
    float stepX;
    float halfWidth;
    vec4 color;
    vec2 size;
    float scroll;
    float fillOn;
    float glow;
    float dotRadius;
    float glowRadius;
    float pad;
};

void main()
{
    float kind = a0.z;
    float side = a0.w;
    float endB = a2.x;

    // Live scrolling: the newest sample starts one step past the right edge
    // and the whole series moves left at constant speed until the next sample
    // lands. The item clips, so new data enters from off-screen and no point
    // on screen ever changes shape. Values are final on arrival, so y only
    // morphs for non-scroll changes (scrubbing, metric switches).
    float shift = stepX * scroll * (1.0 - t);
    float ky = scroll > 0.5 ? 1.0 : t;
    vec2 A = vec2(size.x - stepX * a0.x + shift, mix(a1.x, a1.y, ky));
    vec2 B = vec2(size.x - stepX * a0.y + shift, mix(a1.z, a1.w, ky));
    vec2 P = mix(A, B, endB);
    vec2 d = vec2(side, 0.0);

    if (kind < 0.5) {
        P.y = mix(size.y, P.y, side);
    } else if (kind < 2.5) {
        vec2 seg = B - A;
        float len = length(seg);
        vec2 dir = len > 1e-4 ? seg / len : vec2(1.0, 0.0);
        vec2 n = vec2(-dir.y, dir.x);
        // One pixel past the edge leaves room for the feather; the extension
        // along the segment closes the wedge gap at each joint.
        float reach = (kind < 1.5 ? halfWidth : glowRadius) + 1.0;
        float ext = kind < 1.5 ? halfWidth : 0.0;
        P += n * side * reach + dir * (endB * 2.0 - 1.0) * ext;
        d = vec2(side * reach, 0.0);
    } else {
        float reach = dotRadius + 1.0;
        P = B + a2.yz * reach;
        d = a2.yz * reach;
    }

    vKind = kind;
    vD = d;
    gl_Position = qt_Matrix * vec4(P, 0.0, 1.0);
}
