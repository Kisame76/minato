#version 450
// texelFetch from an RGBA8 and an RGBA8UI image of 16 x 16; the normalized values go back to integers.
layout(set = 0, binding = 0) uniform sampler2D colors;
layout(set = 0, binding = 1) uniform usampler2D values;
layout(location = 0) out uvec4 result;
void main() {
    ivec2 p = ivec2(gl_FragCoord.xy) >> 2;
    uvec4 a = uvec4(round(texelFetch(colors, p, 0) * 255.0));
    uvec4 b = texelFetch(values, p ^ ivec2(3, 5), 0);
    result = (a ^ b.wzyx) & 0xffu;
}
