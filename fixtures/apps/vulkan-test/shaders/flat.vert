#version 450
#extension GL_GOOGLE_include_directive : require
// Positions in units of the 64 x 64 grid, so every edge lies on a pixel boundary; the depth is the window's.
#include "push.glsl"
layout(location = 0) in ivec2 position;
void main() {
    gl_Position = vec4(vec2(position) / 32.0 - 1.0, push.depth, 1.0);
}
