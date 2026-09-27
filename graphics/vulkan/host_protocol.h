/* What libvulkan and graphics/host agree on beyond MoltenVK's API (ADR-0030, decisions 12 and 13): libvulkan
   includes it as guest code, graphics/host as host code. */
#ifndef MINATO_VULKAN_HOST_PROTOCOL_H
#define MINATO_VULKAN_HOST_PROTOCOL_H

#include <stdint.h>
#include <vulkan/vulkan_core.h>

/* graphics/host's answer from a call that makes MoltenVK commit while the process is in the background: it
   backed out without calling MoltenVK, so libvulkan waits at the window's gate of the background kind and
   calls again. No VkResult has this value. */
#define MINATO_VK_BACKGROUND ((VkResult)0x6d696e61)

/* The host windows a call is for, first in the pNext chain of what libvulkan hands graphics/host's
   vkCreateAndroidSurfaceKHR (one window), vkCreateSwapchainKHR (one) and vkQueuePresentKHR (one per
   swapchain, which always gets pResults). graphics/host takes it out before MoltenVK sees the chain. */
#define MINATO_STRUCTURE_TYPE_WINDOWS ((VkStructureType)0x6d696e62)
struct minato_windows {
    VkStructureType sType;
    const void *pNext;
    uint32_t count;
    const uint64_t *windows;
};

#endif
