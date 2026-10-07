/* graphics/host's functions in front of MoltenVK's for libvulkan (vulkan.m), for the "vulkan" provider
   (providers.c). */
#ifndef MINATO_VULKAN_HOST_H
#define MINATO_VULKAN_HOST_H

#include <stdint.h>

#include <vulkan/vulkan.h>

/* Puts graphics/host's functions into the slots of libvulkan's table whose MoltenVK functions they stand
   in front of, and keeps those to call. The table holds MoltenVK's functions. */
void minato_graphics_vulkan_wrap(void **table);

/* "Scheduled" for the runtime (apkrt_add_scheduled_provider): minato_graphics_vulkan_scheduled. */
int minato_graphics_vulkan_provider(void *context, uint64_t deadline);

/* For ANGLE's Vulkan backend (angle_vulkan.m), the same count and devices as libvulkan's: a call that makes MoltenVK
   commit enters, and leaves once MoltenVK returned; enter answers 0 in the background, where it counts nothing. */
int minato_graphics_vulkan_enter(void);
void minato_graphics_vulkan_leave(void);

/* Keeps a new device's queues' MTLCommandQueues, which `function_of`'s vkExportMetalObjectsEXT gives, for
   "scheduled"; 0, or -1 without memory. Forgetting lets them go, before the device is destroyed. */
int minato_graphics_vulkan_keep_device(VkDevice device, const VkDeviceCreateInfo *info, PFN_vkGetDeviceQueue2 queue_of,
                                       PFN_vkGetDeviceProcAddr function_of);
void minato_graphics_vulkan_forget_device(VkDevice device);

/* The MoltenVK that minato_graphics_set_providers named (providers.c), "" for none. */
const char *minato_graphics_moltenvk_path(void);

#endif
