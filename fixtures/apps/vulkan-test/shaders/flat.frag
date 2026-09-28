#version 450
#extension GL_GOOGLE_include_directive : require
#include "push.glsl"
layout(location = 0) out vec4 result;
void main() {
    result = push.color;
}
