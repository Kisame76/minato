#version 450
#extension GL_GOOGLE_include_directive : require
// One bit of each channel of an RGBA8UI image, four times as large: how integer results reach the window.
#include "push.glsl"
layout(set = 0, binding = 0) uniform usampler2D image;
layout(location = 0) out vec4 result;
void main() {
    result = vec4((texelFetch(image, ivec2(gl_FragCoord.xy) >> 2, 0) >> push.bit) & 1u);
}
