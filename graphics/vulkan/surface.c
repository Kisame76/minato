/* libvulkan's surfaces (ADR-0030, decision 12): VK_KHR_android_surface on MoltenVK's VK_EXT_metal_surface on
   the window's CAMetalLayer, which graphics/host finds by the host window's ID, and what a surface answers,
   as Android 15's loader does where MoltenVK allows it: every queue supports it; its current extent is the
   window's buffer geometry once set, else the window's size in pixels (ADR-0028), never MoltenVK's drawable
   size; 3 to 4 images, the identity transform, alpha INHERIT; Android's formats, then B8G8R8A8 (first with
   MINATO_VULKAN_BGRA8_FIRST=1), and FIFO. A surface is its host window's producer from its making to its destroy
   (minato_ANativeWindow_connectProducer, ADR-0060 e2c3), as Android's loader connects at CreateAndroidSurfaceKHR: a
   window a decoder draws into, or the CPU since its first lock, gets none (VK_ERROR_NATIVE_WINDOW_IN_USE_KHR, as
   Android 15's), and a decoder's claim or the CPU's lock of a window with one is refused. */
#include <android/native_window.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "libvulkan.h"

MINATO_HOST(vkCreateAndroidSurfaceKHR);
MINATO_HOST(vkDestroySurfaceKHR);

/* libnativewindow's private functions (ADR-0028, decision 7), which its NDK stub lacks: found once. */
static int (*get_info)(struct ANativeWindow *, struct minato_window_info *);
static int (*connect_producer)(struct ANativeWindow *);
static void (*disconnect_producer)(struct ANativeWindow *);
static pthread_once_t found = PTHREAD_ONCE_INIT;

static void find_info(void) {
    void *library = dlopen("libnativewindow.so", RTLD_NOW);
    if (library != NULL) {
        get_info = (int (*)(struct ANativeWindow *, struct minato_window_info *))dlsym(library,
                                                                                    "minato_ANativeWindow_getInfo");
        connect_producer = (int (*)(struct ANativeWindow *))dlsym(library, "minato_ANativeWindow_connectProducer");
        disconnect_producer =
            (void (*)(struct ANativeWindow *))dlsym(library, "minato_ANativeWindow_disconnectProducer");
    }
}

static int window_info(struct ANativeWindow *window, struct minato_window_info *info) {
    pthread_once(&found, find_info);
    return get_info != NULL && get_info(window, info) == 0 ? 0 : -1;
}

static void disconnect(struct ANativeWindow *window) {
    if (disconnect_producer != NULL) disconnect_producer(window);
}

VkResult vkCreateAndroidSurfaceKHR(VkInstance instance, const VkAndroidSurfaceCreateInfoKHR *info,
                                   const VkAllocationCallbacks *allocator, VkSurfaceKHR *surface) {
    (void)allocator;
    struct minato_window_info window;
    if (window_info(info->window, &window) != 0) {
        return VK_ERROR_NATIVE_WINDOW_IN_USE_KHR;  /* as Android's answer when it cannot connect to it */
    }
    if (connect_producer != NULL && connect_producer(info->window) != 0) {
        /* a decoder's (ADR-0060 e2c3) [verified: Android 15, media-window-test], or the CPU's since its first lock, as
           Android's swapchain.cpp answers any failed native_window_api_connect */
        return VK_ERROR_NATIVE_WINDOW_IN_USE_KHR;
    }
    struct minato_surface *made = calloc(1, sizeof *made);
    if (made == NULL) {
        disconnect(info->window);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    /* graphics/host turns the window's ID into its layer and makes MoltenVK's surface on it. */
    const struct minato_windows windows = {MINATO_STRUCTURE_TYPE_WINDOWS, NULL, 1, &window.id, 0};
    VkAndroidSurfaceCreateInfoKHR ours = *info;
    ours.pNext = &windows;
    const VkResult result = minato_host_vkCreateAndroidSurfaceKHR(instance, &ours, NULL, &made->metal);
    if (result != VK_SUCCESS) {
        free(made);
        disconnect(info->window);
        return result;
    }
    ANativeWindow_acquire(info->window);
    made->window = info->window;
    made->id = window.id;
    *surface = (VkSurfaceKHR)(uintptr_t)made;
    return VK_SUCCESS;
}

void vkDestroySurfaceKHR(VkInstance instance, VkSurfaceKHR surface, const VkAllocationCallbacks *allocator) {
    (void)allocator;
    struct minato_surface *ours = minato_surface(surface);
    if (ours != NULL) {
        minato_host_vkDestroySurfaceKHR(instance, ours->metal, NULL);
        disconnect(ours->window);
        ANativeWindow_release(ours->window);
        free(ours);
    }
}

/* Android's list, in its order: a list of `length` items of `size` bytes as Vulkan hands them out. */
static VkResult hand_out(const void *list, size_t size, uint32_t length, uint32_t *count, void *items) {
    if (items == NULL) {
        *count = length;
        return VK_SUCCESS;
    }
    const uint32_t copied = *count < length ? *count : length;
    memcpy(items, list, copied * size);
    *count = copied;
    return copied < length ? VK_INCOMPLETE : VK_SUCCESS;
}

VkResult vkGetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice physical, uint32_t family, VkSurfaceKHR surface,
                                              VkBool32 *supported) {
    (void)physical, (void)family, (void)surface;
    *supported = VK_TRUE;
    return VK_SUCCESS;
}

VkExtent2D minato_window_size(const struct minato_surface *surface) {
    struct minato_window_info info = {0};
    window_info(surface->window, &info);
    return (VkExtent2D){(uint32_t)info.width, (uint32_t)info.height};
}

/* The window's buffer geometry once the app set one, else its size in pixels. */
static VkExtent2D window_extent(const struct minato_surface *surface) {
    struct minato_window_info info = {0};
    window_info(surface->window, &info);
    if (info.buffer_width > 0 && info.buffer_height > 0) {
        return (VkExtent2D){(uint32_t)info.buffer_width, (uint32_t)info.buffer_height};
    }
    return (VkExtent2D){(uint32_t)info.width, (uint32_t)info.height};
}

VkResult vkGetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice physical, VkSurfaceKHR surface,
                                                   VkSurfaceCapabilitiesKHR *capabilities) {
    (void)physical;
    if (surface == VK_NULL_HANDLE) {
        return VK_ERROR_SURFACE_LOST_KHR;
    }
    const VkExtent2D extent = window_extent(minato_surface(surface));
    /* 3 to 4 images: 4 - 3 + 1 = 2 held at once, which MoltenVK's 3 back without a present. A turn gives the
       window a new size rather than a transform (ADR-0038 c), so nothing needs pre-rotation. */
    *capabilities = (VkSurfaceCapabilitiesKHR){
        .minImageCount = 3, .maxImageCount = 4, .currentExtent = extent, .minImageExtent = {1, 1},
        .maxImageExtent = {extent.width > 4096 ? extent.width : 4096, extent.height > 4096 ? extent.height : 4096},
        .maxImageArrayLayers = 1, .supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
        .currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
        .supportedCompositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
        .supportedUsageFlags = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                               VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT};
    return VK_SUCCESS;
}

VkResult vkGetPhysicalDeviceSurfaceCapabilities2KHR(VkPhysicalDevice physical,
                                                    const VkPhysicalDeviceSurfaceInfo2KHR *info,
                                                    VkSurfaceCapabilities2KHR *capabilities) {
    const VkResult result =
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, info->surface, &capabilities->surfaceCapabilities);
    VkSurfaceProtectedCapabilitiesKHR *protected =
        minato_chain_find(capabilities->pNext, VK_STRUCTURE_TYPE_SURFACE_PROTECTED_CAPABILITIES_KHR);
    if (protected != NULL) {
        protected->supportsProtected = VK_FALSE;
    }
    return result;
}

static const VkSurfaceFormatKHR formats[] = {
    {VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
    {VK_FORMAT_R8G8B8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
    {VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
    {VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
    {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
    {VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
};
#define FORMATS (uint32_t)(sizeof formats / sizeof *formats)

/* The same formats with B8G8R8A8 first, which MoltenVK's drawables take without the copy (F15): only with
   MINATO_VULKAN_BGRA8_FIRST=1 in the environment (V5). */
static const VkSurfaceFormatKHR bgra8_first[] = {
    {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
    {VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
    {VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
    {VK_FORMAT_R8G8B8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
    {VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
    {VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
};
_Static_assert(sizeof bgra8_first == sizeof formats, "both orders list the same formats");

static const VkSurfaceFormatKHR *surface_formats(void) {
    const char *value = getenv("MINATO_VULKAN_BGRA8_FIRST");
    return value != NULL && strcmp(value, "1") == 0 ? bgra8_first : formats;
}

int minato_surface_format_entry(VkFormat format, VkColorSpaceKHR color_space, uint32_t *count) {
    const VkSurfaceFormatKHR *order = surface_formats();
    *count = FORMATS;
    for (uint32_t index = 0; index < FORMATS; index++) {
        if (order[index].format == format && order[index].colorSpace == color_space) {
            return (int)index;
        }
    }
    return -1;
}

/* With VK_GOOGLE_surfaceless_query, `surface` may be VK_NULL_HANDLE: the same lists. */
VkResult vkGetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice physical, VkSurfaceKHR surface, uint32_t *count,
                                              VkSurfaceFormatKHR *list) {
    (void)physical, (void)surface;
    return hand_out(surface_formats(), sizeof *formats, FORMATS, count, list);
}

VkResult vkGetPhysicalDeviceSurfaceFormats2KHR(VkPhysicalDevice physical, const VkPhysicalDeviceSurfaceInfo2KHR *info,
                                               uint32_t *count, VkSurfaceFormat2KHR *list) {
    (void)physical, (void)info;
    if (list == NULL) {
        *count = FORMATS;
        return VK_SUCCESS;
    }
    const VkSurfaceFormatKHR *order = surface_formats();
    const uint32_t copied = *count < FORMATS ? *count : FORMATS;
    for (uint32_t index = 0; index < copied; index++) {
        list[index].surfaceFormat = order[index];
    }
    *count = copied;
    return copied < FORMATS ? VK_INCOMPLETE : VK_SUCCESS;
}

VkResult vkGetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice physical, VkSurfaceKHR surface, uint32_t *count,
                                                   VkPresentModeKHR *modes) {
    (void)physical, (void)surface;
    static const VkPresentModeKHR fifo = VK_PRESENT_MODE_FIFO_KHR;
    return hand_out(&fifo, sizeof fifo, 1, count, modes);
}

VkResult vkGetPhysicalDevicePresentRectanglesKHR(VkPhysicalDevice physical, VkSurfaceKHR surface, uint32_t *count,
                                                 VkRect2D *rectangles) {
    (void)physical;
    if (surface == VK_NULL_HANDLE) {
        return VK_ERROR_SURFACE_LOST_KHR;
    }
    const VkRect2D whole = {{0, 0}, window_extent(minato_surface(surface))};
    return hand_out(&whole, sizeof whole, 1, count, rectangles);
}

VkResult vkGetDeviceGroupPresentCapabilitiesKHR(VkDevice device, VkDeviceGroupPresentCapabilitiesKHR *capabilities) {
    (void)device;
    memset(capabilities->presentMask, 0, sizeof capabilities->presentMask);
    capabilities->presentMask[0] = 1;
    capabilities->modes = VK_DEVICE_GROUP_PRESENT_MODE_LOCAL_BIT_KHR;
    return VK_SUCCESS;
}

VkResult vkGetDeviceGroupSurfacePresentModesKHR(VkDevice device, VkSurfaceKHR surface,
                                                VkDeviceGroupPresentModeFlagsKHR *modes) {
    (void)device, (void)surface;
    *modes = VK_DEVICE_GROUP_PRESENT_MODE_LOCAL_BIT_KHR;
    return VK_SUCCESS;
}
