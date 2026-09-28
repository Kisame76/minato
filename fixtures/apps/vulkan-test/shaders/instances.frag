#version 450
layout(location = 0) flat in uint instance;
layout(location = 0) out uvec4 result;
void main() {
    uvec2 p = uvec2(gl_FragCoord.xy);
    result = uvec4(instance * 16u + 1u, (p.x ^ p.y) & 0xffu, instance ^ 0xa5u, 255u);
}
