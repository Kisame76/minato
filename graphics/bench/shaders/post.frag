#version 450
// The post pass: the offscreen target with its colors a little apart, a vignette and an S-shaped contrast curve.
layout(set = 0, binding = 0) uniform sampler2D u_scene;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 color;

void main() {
    vec2 offset = vec2(0.0015, 0.0);
    vec3 scene = vec3(texture(u_scene, v_uv + offset).r, texture(u_scene, v_uv).g, texture(u_scene, v_uv - offset).b);
    vec2 centered = v_uv - 0.5;
    scene *= 1.0 - 0.6 * dot(centered, centered);
    color = vec4(scene * scene * (3.0 - 2.0 * scene), 1.0);
}
