#version 450
// vulkan-indexing-test's plain pass: a fixed array, indexed by a push constant (dynamically uniform).
layout(set = 0, binding = 0) uniform sampler2D textures[8];
layout(push_constant) uniform Push {
    int index;
} push;
layout(location = 0) out vec4 color;
void main() {
    color = texture(textures[push.index], vec2(0.5));
}
