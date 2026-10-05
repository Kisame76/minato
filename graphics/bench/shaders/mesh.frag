#version 450
// A mesh's light: its texture tinted, diffuse and specular from one direction.
layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 normal[3];
    vec4 color;
} push;
layout(set = 0, binding = 0) uniform sampler2D u_texture;
// The window's height in pixels, which vulkan.c sets when it makes the pipeline.
layout(constant_id = 0) const float height = 1.0;
layout(location = 0) in vec3 v_normal;
layout(location = 1) in vec2 v_uv;
layout(location = 0) out vec4 color;

void main() {
    vec3 normal = normalize(v_normal);
    vec3 light = normalize(vec3(0.4, 0.7, 0.6));
    float diffuse = max(dot(normal, light), 0.0);
    float specular = pow(max(dot(normal, normalize(light + vec3(0.0, 0.0, 1.0))), 0.0), 32.0);
    // The texture at one level for the whole mesh, as derivatives would give it at the sphere's middle: 512 / pi
    // texels a radian (256 texels, 4 times around) over the sphere's radius on the window, from its scale (the length
    // of w's row in mvp's first three columns, scene.c) and its depth (w of the fourth). Derivatives at triangle
    // edges vary with the GPU's state, and so would the frame's pixels (ADR-0037).
    float scale = length(vec3(push.mvp[0][3], push.mvp[1][3], push.mvp[2][3]));
    float radius = 1.7320508 * scale / push.mvp[3][3] * 0.5 * height;
    float level = clamp(log2(512.0 / 3.14159265 / radius), 0.0, 8.0);
    vec3 albedo = textureLod(u_texture, v_uv * vec2(4.0, 2.0), level).rgb * push.color.rgb;
    color = vec4(albedo * (0.2 + 0.8 * diffuse) + vec3(0.35 * specular), 1.0);
}
