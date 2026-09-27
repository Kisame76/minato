/* The functions written by hand that later steps of ADR-0030 fill in, for now straight to MoltenVK: the
   queue's submits and waits (V4a2), the surfaces and swapchains (V4b). The debug callbacks (V4a2) never
   reach MoltenVK (decision 5): until graphics/host passes their messages on, they get handles of their
   own and no message. */
#include "libvulkan.h"

static uint64_t last_handle;

static uint64_t new_handle(void) {
    return __atomic_add_fetch(&last_handle, 1, __ATOMIC_RELAXED);
}

#define PASS(result, name, parameters, arguments) \
    MINATO_HOST(name);                            \
    result name parameters { return minato_host_##name arguments; }
#define PASS_VOID(name, parameters, arguments) \
    MINATO_HOST(name);                         \
    void name parameters { minato_host_##name arguments; }

/* The queue (V4a2). */
PASS(VkResult, vkQueueSubmit, (VkQueue queue, uint32_t count, const VkSubmitInfo *submits, VkFence fence),
     (queue, count, submits, fence))
PASS(VkResult, vkQueueSubmit2, (VkQueue queue, uint32_t count, const VkSubmitInfo2 *submits, VkFence fence),
     (queue, count, submits, fence))
PASS(VkResult, vkQueueBindSparse, (VkQueue queue, uint32_t count, const VkBindSparseInfo *binds, VkFence fence),
     (queue, count, binds, fence))
PASS(VkResult, vkQueueWaitIdle, (VkQueue queue), (queue))
PASS(VkResult, vkDeviceWaitIdle, (VkDevice device), (device))
PASS(VkResult, vkWaitForFences,
     (VkDevice device, uint32_t count, const VkFence *fences, VkBool32 all, uint64_t timeout),
     (device, count, fences, all, timeout))
PASS(VkResult, vkWaitSemaphores, (VkDevice device, const VkSemaphoreWaitInfo *info, uint64_t timeout),
     (device, info, timeout))
PASS(VkResult, vkGetQueryPoolResults,
     (VkDevice device, VkQueryPool pool, uint32_t first, uint32_t count, size_t size, void *data,
      VkDeviceSize stride, VkQueryResultFlags flags),
     (device, pool, first, count, size, data, stride, flags))
PASS_VOID(vkGetDeviceQueue, (VkDevice device, uint32_t family, uint32_t index, VkQueue *queue),
          (device, family, index, queue))
PASS_VOID(vkGetDeviceQueue2, (VkDevice device, const VkDeviceQueueInfo2 *info, VkQueue *queue),
          (device, info, queue))

/* The debug callbacks (V4a2). */
VkResult vkCreateDebugReportCallbackEXT(VkInstance instance, const VkDebugReportCallbackCreateInfoEXT *info,
                                        const VkAllocationCallbacks *allocator, VkDebugReportCallbackEXT *callback) {
    (void)instance, (void)info, (void)allocator;
    *callback = (VkDebugReportCallbackEXT)new_handle();
    return VK_SUCCESS;
}

void vkDestroyDebugReportCallbackEXT(VkInstance instance, VkDebugReportCallbackEXT callback,
                                     const VkAllocationCallbacks *allocator) {
    (void)instance, (void)callback, (void)allocator;
}

VkResult vkCreateDebugUtilsMessengerEXT(VkInstance instance, const VkDebugUtilsMessengerCreateInfoEXT *info,
                                        const VkAllocationCallbacks *allocator, VkDebugUtilsMessengerEXT *messenger) {
    (void)instance, (void)info, (void)allocator;
    *messenger = (VkDebugUtilsMessengerEXT)new_handle();
    return VK_SUCCESS;
}

void vkDestroyDebugUtilsMessengerEXT(VkInstance instance, VkDebugUtilsMessengerEXT messenger,
                                     const VkAllocationCallbacks *allocator) {
    (void)instance, (void)messenger, (void)allocator;
}

/* Surfaces and swapchains (V4b). */
PASS(VkResult, vkCreateAndroidSurfaceKHR,
     (VkInstance instance, const VkAndroidSurfaceCreateInfoKHR *info, const VkAllocationCallbacks *allocator,
      VkSurfaceKHR *surface),
     (instance, info, allocator, surface))
PASS_VOID(vkDestroySurfaceKHR, (VkInstance instance, VkSurfaceKHR surface, const VkAllocationCallbacks *allocator),
          (instance, surface, allocator))
PASS(VkResult, vkGetPhysicalDeviceSurfaceSupportKHR,
     (VkPhysicalDevice physical, uint32_t family, VkSurfaceKHR surface, VkBool32 *supported),
     (physical, family, surface, supported))
PASS(VkResult, vkGetPhysicalDeviceSurfaceCapabilitiesKHR,
     (VkPhysicalDevice physical, VkSurfaceKHR surface, VkSurfaceCapabilitiesKHR *capabilities),
     (physical, surface, capabilities))
PASS(VkResult, vkGetPhysicalDeviceSurfaceFormatsKHR,
     (VkPhysicalDevice physical, VkSurfaceKHR surface, uint32_t *count, VkSurfaceFormatKHR *formats),
     (physical, surface, count, formats))
PASS(VkResult, vkGetPhysicalDeviceSurfacePresentModesKHR,
     (VkPhysicalDevice physical, VkSurfaceKHR surface, uint32_t *count, VkPresentModeKHR *modes),
     (physical, surface, count, modes))
PASS(VkResult, vkGetPhysicalDevicePresentRectanglesKHR,
     (VkPhysicalDevice physical, VkSurfaceKHR surface, uint32_t *count, VkRect2D *rectangles),
     (physical, surface, count, rectangles))
PASS(VkResult, vkGetDeviceGroupPresentCapabilitiesKHR,
     (VkDevice device, VkDeviceGroupPresentCapabilitiesKHR *capabilities), (device, capabilities))
PASS(VkResult, vkGetDeviceGroupSurfacePresentModesKHR,
     (VkDevice device, VkSurfaceKHR surface, VkDeviceGroupPresentModeFlagsKHR *modes), (device, surface, modes))
PASS(VkResult, vkGetPhysicalDeviceSurfaceCapabilities2KHR,
     (VkPhysicalDevice physical, const VkPhysicalDeviceSurfaceInfo2KHR *info,
      VkSurfaceCapabilities2KHR *capabilities),
     (physical, info, capabilities))
PASS(VkResult, vkGetPhysicalDeviceSurfaceFormats2KHR,
     (VkPhysicalDevice physical, const VkPhysicalDeviceSurfaceInfo2KHR *info, uint32_t *count,
      VkSurfaceFormat2KHR *formats),
     (physical, info, count, formats))
PASS(VkResult, vkCreateSwapchainKHR,
     (VkDevice device, const VkSwapchainCreateInfoKHR *info, const VkAllocationCallbacks *allocator,
      VkSwapchainKHR *swapchain),
     (device, info, allocator, swapchain))
PASS_VOID(vkDestroySwapchainKHR, (VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks *allocator),
          (device, swapchain, allocator))
PASS(VkResult, vkGetSwapchainImagesKHR,
     (VkDevice device, VkSwapchainKHR swapchain, uint32_t *count, VkImage *images), (device, swapchain, count, images))
PASS(VkResult, vkAcquireNextImageKHR,
     (VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout, VkSemaphore semaphore, VkFence fence,
      uint32_t *index),
     (device, swapchain, timeout, semaphore, fence, index))
PASS(VkResult, vkAcquireNextImage2KHR, (VkDevice device, const VkAcquireNextImageInfoKHR *info, uint32_t *index),
     (device, info, index))
PASS(VkResult, vkQueuePresentKHR, (VkQueue queue, const VkPresentInfoKHR *info), (queue, info))
