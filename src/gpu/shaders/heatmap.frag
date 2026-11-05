// SPDX-License-Identifier: GPL-3.0-or-later
#version 440

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    float rows;
    float cols;     // visible columns; the texture holds cols + 1
    float phase;    // columns the render time sits past the newest sample (<= 0)
    vec4 hue;       // linear-light rgb
    float rowPx;
    float gap;
};

layout(binding = 1) uniform sampler2D values;

vec3 toLinear(vec3 c)
{
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c));
}

vec3 toSrgb(vec3 c)
{
    c = max(c, vec3(0.0));
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c));
}

// OKLab keeps equal steps in load looking like equal steps in brightness,
// which a raw sRGB lerp through a saturated hue does not.
vec3 toLab(vec3 c)
{
    vec3 lms = mat3(0.4122214708, 0.2119034982, 0.0883024619,
                    0.5363325363, 0.6806995451, 0.2817188376,
                    0.0514459929, 0.1073969566, 0.6299787005) * c;
    lms = sign(lms) * pow(abs(lms), vec3(1.0 / 3.0));
    return mat3(0.2104542553, 1.9779984951, 0.0259040371,
                0.7936177850, -2.4285922050, 0.7827717662,
                -0.0040720468, 0.4505937099, -0.8086757660) * lms;
}

vec3 fromLab(vec3 lab)
{
    vec3 lms = mat3(1.0, 1.0, 1.0,
                    0.3963377774, -0.1055613458, -0.0894841775,
                    0.2158037573, -0.0638541728, -1.2914855480) * lab;
    lms = lms * lms * lms;
    return mat3(4.0767416621, -1.2684380046, -0.0041960863,
                -3.3077115913, 2.6097574011, -0.7034186147,
                0.2309699292, -0.3413193965, 1.7076147010) * lms;
}

void main()
{
    float row = min(floor(v_uv.y * rows), rows - 1.0);
    // Inside a row the value must not bleed into its neighbours, so v snaps to
    // the row centre while u keeps linear filtering for a smooth time axis.
    float v = (row + 0.5) / rows;
    // Texture holds cols + 2 columns, newest last; phase 0 lands it on the right edge.
    float u = (v_uv.x * cols + 1.0 + phase + 0.5) / (cols + 2.0);
    float t = clamp(texture(values, vec2(u, v)).r, 0.0, 1.0);

    vec3 cold = toLab(toLinear(vec3(0.0549)));
    vec3 mid = toLab(hue.rgb);
    // Peak load reads as heat: brighter than the hue but holding a trace of it.
    vec3 hot = mix(mid, toLab(vec3(1.0)), 0.82);
    hot.yz *= 0.6;
    vec3 lab = t < 0.7 ? mix(cold, mid, t / 0.7) : mix(mid, hot, (t - 0.7) / 0.3);
    vec3 rgb = toSrgb(fromLab(lab));

    // Row separators: a clear gap, softened by one pixel so it survives scaling.
    float yIn = (v_uv.y * rows - row) * rowPx;
    float keep = gap > 0.0 && row < rows - 1.0 ? 1.0 - smoothstep(rowPx - gap - 0.5, rowPx - gap + 0.5, yIn) : 1.0;

    float a = qt_Opacity * keep;
    fragColor = vec4(rgb * a, a);
}
