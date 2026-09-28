#version 450
// A particle: a quad whose place, drift, size and color come from its instance's hash.
layout(push_constant) uniform Push {
    float time;
    float aspect;
} push;
layout(location = 0) out vec2 v_corner;
layout(location = 1) out vec4 v_color;

uint hash(uint x) {
    x ^= x >> 16u;
    x *= 0x7feb352du;
    x ^= x >> 15u;
    x *= 0x846ca68bu;
    x ^= x >> 16u;
    return x;
}

float unit(uint value) {
    return float(value >> 8u) / 16777216.0;
}

const vec2 corners[6] = vec2[6](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, -1.0), vec2(1.0, 1.0),
                                vec2(-1.0, 1.0));

void main() {
    uint id = uint(gl_InstanceIndex) * 4u;
    uint a = hash(id), b = hash(id + 1u), c = hash(id + 2u), d = hash(id + 3u);
    vec2 velocity = (vec2(unit(c), unit(d)) - 0.5) * 0.12;
    vec2 center = fract(vec2(unit(a), unit(b)) + velocity * push.time) * 2.2 - 1.1;
    float size = 0.006 + 0.016 * unit(hash(a));
    v_corner = corners[gl_VertexIndex];
    v_color = vec4(0.5 + 0.5 * unit(b ^ c), 0.5 + 0.5 * unit(c ^ d), 0.6 + 0.4 * unit(d ^ a), 0.4);
    vec2 position = center + v_corner * vec2(size / push.aspect, size);
    // GL's clip space into Vulkan's, as mesh.vert.
    gl_Position = vec4(position.x, -position.y, (unit(hash(b)) * 1.8 - 0.9) * 0.5 + 0.5, 1.0);
}
