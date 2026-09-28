#version 450
layout(set = 0, binding = 0) uniform sampler2D image;
layout(location = 0) out uvec4 result;
void main() {
    result = uvec4(round(texelFetch(image, ivec2(gl_FragCoord.xy) >> 2, 0) * 255.0));
}
