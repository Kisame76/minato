/* What libvulkan and graphics/host agree on beyond MoltenVK's API (ADR-0030, decisions 12 and 13; ADR-0058 decision
   7): libvulkan includes it as guest code, graphics/host as host code. */
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

/* A present's flag: graphics/host hands MoltenVK no times (0) and keeps the app's for its past timings. libvulkan
   sets it with MINATO_PRESENTS_WITHOUT_TIME=1 in the environment, ADR-0041 decision 11's measurement switch, which
   only the benchmark and Debug builds pass to the guest. */
#define MINATO_WINDOWS_WITHOUT_TIME 1u
struct minato_windows {
    VkStructureType sType;
    const void *pNext;
    uint32_t count;
    const uint64_t *windows;
    uint32_t flags;  /* MINATO_WINDOWS_*, for a present */
};

/* The hardware buffer that a dedicated allocation of an image imports, first in the pNext chain of what libvulkan
   hands graphics/host's vkAllocateMemory: the buffer's ID (AHardwareBuffer_getId). graphics/host gives the chain's
   dedicated image the buffer's IOSurface and takes the structure out before MoltenVK sees the chain. */
#define MINATO_STRUCTURE_TYPE_HARDWARE_BUFFER ((VkStructureType)0x6d696e63)
struct minato_hardware_buffer {
    VkStructureType sType;
    const void *pNext;
    uint64_t buffer;
};

#endif
