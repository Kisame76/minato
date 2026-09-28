#version 450
#extension GL_GOOGLE_include_directive : require
// An RGBA8 image of 0 and 1 drawn 1:1 (shift 0) or four times as large (shift 2).
#include "push.glsl"
layout(set = 0, binding = 0) uniform sampler2D image;
layout(location = 0) out vec4 result;
void main() {
    result = texelFetch(image, (ivec2(gl_FragCoord.xy) - push.origin) >> push.shift, 0);
}
