#version 450
// A particle's soft disc, alpha-blended.
layout(location = 0) in vec2 v_corner;
layout(location = 1) in vec4 v_color;
layout(location = 0) out vec4 color;

void main() {
    float fade = 1.0 - dot(v_corner, v_corner);
    if (fade < 0.0) discard;
    color = vec4(v_color.rgb, fade * fade * v_color.a);
}
