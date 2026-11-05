#version 450
#extension GL_EXT_nonuniform_qualifier : require
// vulkan-indexing-test's indexed pass: pixel x samples texture x of a runtime array, a non-uniform index.
layout(set = 0, binding = 0) uniform sampler2D textures[];
layout(location = 0) out vec4 color;
void main() {
    color = texture(textures[nonuniformEXT(int(gl_FragCoord.x))], vec2(0.5));
}
