#version 450
// A mesh: its uniforms are 128 bytes of push constants (scene.h's struct bench_mesh).
layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec2 a_uv;
layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 normal[3];
    vec4 color;
} push;
layout(location = 0) out vec3 v_normal;
layout(location = 1) out vec2 v_uv;

void main() {
    v_normal = mat3(push.normal[0].xyz, push.normal[1].xyz, push.normal[2].xyz) * a_normal;
    v_uv = a_uv;
    vec4 position = push.mvp * vec4(a_position, 1.0);
    // GL's clip space, which scene.c computes, into Vulkan's: y down, depth from 0 to 1.
    gl_Position = vec4(position.x, -position.y, (position.z + position.w) * 0.5, position.w);
}
