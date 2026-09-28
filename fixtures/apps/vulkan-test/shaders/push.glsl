// The push constants every graphics pipeline of vulkan-test shares (main.c's struct push).
layout(push_constant) uniform Push {
    vec4 color;
    ivec2 origin;
    float depth;
    int shift;
    uint bit;
} push;
