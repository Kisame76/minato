#version 450
// The fill-heavy load (M6's MetalFX plan, step 0): a full-screen layer, alpha-blended, whose fragments make 16 reads of
// the sphere texture, each address from the one before, so that the work grows with the pixels and not with the
// geometry. A layer differs from the next by its number.
layout(push_constant) uniform Push {
    float time;
    float aspect;
    float layer;
} push;
layout(set = 0, binding = 0) uniform sampler2D u_texture;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 color;

void main() {
    vec2 uv = v_uv;
    vec3 sum = vec3(0.0);
    for (int i = 0; i < 16; ++i) {
        vec4 texel = textureLod(u_texture, fract(uv * 3.0 + push.layer * 0.13), 0.0);
        uv = fract(uv + texel.rg * 0.07 + 0.013 * float(i) + push.time * 0.01);
        sum += texel.rgb;
    }
    color = vec4(sum * (1.0 / 16.0), 0.3);
}
