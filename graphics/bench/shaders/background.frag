#version 450
// The background: a gradient and a band that move with the scene's time.
layout(push_constant) uniform Push {
    float time;
    float aspect;
} push;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 color;

void main() {
    float y = 1.0 - v_uv.y;  // Vulkan's rows run down, GL's up
    vec3 top = vec3(0.10, 0.16, 0.32) + 0.08 * vec3(sin(push.time * 0.30), sin(push.time * 0.23 + 1.0),
                                                    sin(push.time * 0.17 + 2.0));
    float band = 0.04 * sin(y * 24.0 + push.time * 1.7);
    color = vec4(mix(vec3(0.02, 0.03, 0.06), top, y) + band, 1.0);
}
