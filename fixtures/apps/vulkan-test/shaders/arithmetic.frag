#version 450
// Integer arithmetic without division, with its constants from a uniform buffer: gles-test's scene 5.
layout(set = 0, binding = 2, std140) uniform Values {
    uvec4 seed;
    uvec4 factor;
};
layout(location = 0) out uvec4 result;
void main() {
    uvec2 p = uvec2(gl_FragCoord.xy);
    uint h = (p.x * factor.x + p.y * factor.y) ^ seed.x;
    h ^= h >> 13;
    h *= factor.z;
    h ^= (h >> 15) + seed.y;
    h = (h << 3) | (h >> 29);
    result = uvec4(h, h >> 8, h >> 16, h >> 24) & 0xffu;
}
