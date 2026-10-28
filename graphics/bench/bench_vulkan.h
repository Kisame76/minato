/* The scene with Vulkan (vulkan.c), through the functions of the vkGetInstanceProcAddr that the platform gives it. */
#ifndef BENCH_VULKAN_H
#define BENCH_VULKAN_H

#include <vulkan/vulkan.h>

/* What differs between the builds: where the functions come from, the instance's extensions and chain, and the
   window's surface. */
struct bench_vulkan_platform {
    PFN_vkGetInstanceProcAddr get_instance_proc_addr;
    const char *const *extensions;
    uint32_t extension_count;
    const void *next;  // vkCreateInstance's chain
    VkResult (*create_surface)(void *context, VkInstance instance, PFN_vkGetInstanceProcAddr get,
                               VkSurfaceKHR *surface);
    void *context;
};

struct bench_vulkan;

/* The instance, the window's surface, a device, a swapchain of the surface's first format at its size, and the
   scene's objects with `meshes` meshes; `check` is the frame whose CRC it logs, at its place and drawn once before
   the first frame, -1 for none; `scale` the offscreen target's size per side in percent of the swapchain's
   (bench.conf's scale, 100 for none); `fill_layers` the full-screen alpha layers of the fill-heavy load
   (bench_fill_layers, 0 for none). NULL when a step fails, which it logs. */
struct bench_vulkan *bench_vulkan_start(const struct bench_vulkan_platform *platform, int meshes, int check,
                                        int scale, int fill_layers);

/* Draws frame `frame` and presents it: VK_SUCCESS, VK_SUBOPTIMAL_KHR, or the error of the call that failed. */
VkResult bench_vulkan_frame(struct bench_vulkan *scene, int frame);

/* Waits for the device and destroys everything. */
void bench_vulkan_stop(struct bench_vulkan *scene);

#endif
