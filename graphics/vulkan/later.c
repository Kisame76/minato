/* The functions written by hand that a later step of ADR-0030 fills in, for now straight to MoltenVK: the
   surfaces and swapchains (V4b). */
#include "libvulkan.h"

#define PASS(result, name, parameters, arguments) \
    MINATO_HOST(name);                            \
    result name parameters { return minato_host_##name arguments; }
#define PASS_VOID(name, parameters, arguments) \
    MINATO_HOST(name);                         \
    void name parameters { minato_host_##name arguments; }

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
