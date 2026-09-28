#version 450
// An 8 x 8 square, moved per instance by an attribute with the instance rate.
layout(location = 0) in ivec2 position;
layout(location = 1) in ivec2 offset;
layout(location = 0) flat out uint instance;
void main() {
    instance = uint(gl_InstanceIndex);
    gl_Position = vec4(vec2(position + offset) / 32.0 - 1.0, 0.0, 1.0);
}
