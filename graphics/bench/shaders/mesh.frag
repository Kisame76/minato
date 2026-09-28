#version 450
// A mesh's light: its texture tinted, diffuse and specular from one direction.
layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 normal[3];
    vec4 color;
} push;
layout(set = 0, binding = 0) uniform sampler2D u_texture;
layout(location = 0) in vec3 v_normal;
layout(location = 1) in vec2 v_uv;
layout(location = 0) out vec4 color;

void main() {
    vec3 normal = normalize(v_normal);
    vec3 light = normalize(vec3(0.4, 0.7, 0.6));
    float diffuse = max(dot(normal, light), 0.0);
    float specular = pow(max(dot(normal, normalize(light + vec3(0.0, 0.0, 1.0))), 0.0), 32.0);
    vec3 albedo = texture(u_texture, v_uv * vec2(4.0, 2.0)).rgb * push.color.rgb;
    color = vec4(albedo * (0.2 + 0.8 * diffuse) + vec3(0.35 * specular), 1.0);
}
