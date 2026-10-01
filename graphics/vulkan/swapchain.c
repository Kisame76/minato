/* libvulkan's swapchains on the surfaces of surface.c (ADR-0030, decisions 12 and 13), two ways:
   - direct: a MoltenVK format (B8G8R8A8, R16G16B16A16_SFLOAT, A2B10G10R10), at most 3 images and a usage
     MoltenVK's surface supports: MoltenVK's swapchain as it is;
   - copy, for everything else, R8G8B8A8 above all: the app's images are libvulkan's (its format, extent
     and usage with transfer source, in device memory), and MoltenVK's swapchain has 3 images in the app's
     format when it is MoltenVK's, else in B8G8R8A8 of the same encoding. Acquiring picks the least recently
     presented image the app does not hold, once the copy that last read it is done (its queue's timeline
     value), and acquires a MoltenVK image with the app's semaphore and fence. Presenting copies the image
     1:1 with vkCmdBlitImage and NEAREST, which reorders the channels, on the app's queue after the app's
     semaphores, and presents MoltenVK's image after the copy.
   A replaced swapchain answers VK_ERROR_OUT_OF_DATE_KHR, and MoltenVK's VK_SUBOPTIMAL_KHR for another size
   than the layer's becomes VK_SUCCESS: Android 15 answers it only at a present, while the swapchain's
   pre-transform differs from the window's transform hint (swapchain.cpp, QueuePresentKHR:
   "swapchain.pre_transform != window_transform_hint"), so after a turn. With the identity transform only, a
   present here answers it while the window's size differs from the one the swapchain was made at
   (ADR-0038 c): a turn by 180 degrees keeps the size and VK_SUCCESS. A present meets the
   window's gate before anything else (closed waits, gone answers VK_ERROR_SURFACE_LOST_KHR without GPU
   work, the app's semaphores left as they are); an acquire answers VK_ERROR_SURFACE_LOST_KHR for a window
   that is gone and waits in the background; MoltenVK's acquire and present, and the copies' submits, go
   through graphics/host's background gate. VK_GOOGLE_display_timing (ADR-0041 decision 11) is MoltenVK's on the
   swapchain's own; a present's times reach MoltenVK for the swapchains that go on, in their order. With
   MINATO_VULKAN_OPAQUE=1 in the environment, MoltenVK's swapchain gets OPAQUE for the app's INHERIT (V5's late
   frames). */
#include <stdlib.h>
#include <string.h>

#include "libvulkan.h"

MINATO_HOST(vkCreateSwapchainKHR);
MINATO_HOST(vkDestroySwapchainKHR);
MINATO_HOST(vkGetSwapchainImagesKHR);
MINATO_HOST(vkAcquireNextImageKHR);
MINATO_HOST(vkQueuePresentKHR);
MINATO_HOST(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
MINATO_HOST(vkGetRefreshCycleDurationGOOGLE);
MINATO_HOST(vkGetPastPresentationTimingGOOGLE);

/* An image of the copy way: the app's, with its memory and the command buffer of its copy. */
struct image {
    VkImage image;
    VkDeviceMemory memory;
    VkCommandBuffer commands;
    uint32_t target;                   /* MoltenVK's image it was acquired with */
    int held;                          /* acquired and not given back */
    uint64_t presented;                /* the swapchain's count of images given back when this one was; 0 never */
    const struct minato_queue *queue;  /* the queue of its last copy, whose timeline has `copied` once it is done */
    uint64_t copied;
};

struct swapchain {
    struct minato_surface *surface;
    VkDevice device;
    VkSwapchainKHR moltenvk;
    VkExtent2D extent;
    VkExtent2D window;  /* the window's size when it was made */
    int copy;
    /* The copy way: MoltenVK's images, each with a semaphore its present waits for, the command pool of the
       queue family the copies run on, and the app's images. */
    uint32_t targets;
    VkImage *target_images;
    VkSemaphore *copied;
    VkCommandPool pool;
    uint32_t family;
    uint64_t given_back;
    uint32_t count;
    struct image images[];
};

static struct swapchain *swapchain_of(VkSwapchainKHR handle) {
    return (struct swapchain *)(uintptr_t)handle;
}

static int replaced(const struct swapchain *swapchain) {
    return __atomic_load_n(&swapchain->surface->swapchain, __ATOMIC_ACQUIRE) != (VkSwapchainKHR)(uintptr_t)swapchain;
}

/* Whether the window has another size than when the swapchain was made: turned by 90 degrees. */
static int turned(const struct swapchain *swapchain) {
    const VkExtent2D now = minato_window_size(swapchain->surface);
    return now.width != 0 && (now.width != swapchain->window.width || now.height != swapchain->window.height);
}

static int moltenvk_format(VkFormat format) {
    return format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB ||
           format == VK_FORMAT_R16G16B16A16_SFLOAT || format == VK_FORMAT_A2B10G10R10_UNORM_PACK32;
}

/* Whether MoltenVK's swapchain serves the app as it is. */
static int direct(VkPhysicalDevice physical, const struct minato_surface *surface, const VkSwapchainCreateInfoKHR *info) {
    VkSurfaceCapabilitiesKHR moltenvk;
    return moltenvk_format(info->imageFormat) && info->minImageCount <= 3 && info->imageArrayLayers == 1 &&
           minato_host_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface->metal, &moltenvk) == VK_SUCCESS &&
           (info->imageUsage & ~moltenvk.supportedUsageFlags) == 0;
}

/* The composite alpha MoltenVK's swapchain gets: the app's, but OPAQUE for INHERIT with MINATO_VULKAN_OPAQUE=1 in
   the environment (V5's late frames). MoltenVK leaves the layer's opacity as it is for INHERIT only. */
static VkCompositeAlphaFlagBitsKHR composite_alpha(VkCompositeAlphaFlagBitsKHR asked) {
    const char *value = getenv("MINATO_VULKAN_OPAQUE");
    return asked == VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR && value != NULL && strcmp(value, "1") == 0
               ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
               : asked;
}

/* A memory type of the device's own among `bits`, else the first of them. */
static uint32_t device_memory(VkPhysicalDevice physical, uint32_t bits) {
    VkPhysicalDeviceMemoryProperties properties;
    vkGetPhysicalDeviceMemoryProperties(physical, &properties);
    for (uint32_t type = 0; type < properties.memoryTypeCount; type++) {
        if ((bits & (1u << type)) && (properties.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            return type;
        }
    }
    return bits != 0 ? (uint32_t)__builtin_ctz(bits) : 0;
}

/* The copy way's semaphores and the app's images. */
static VkResult copy_setup(struct swapchain *swapchain, VkPhysicalDevice physical, const VkSwapchainCreateInfoKHR *info) {
    const VkDevice device = swapchain->device;
    VkResult result = minato_host_vkGetSwapchainImagesKHR(device, swapchain->moltenvk, &swapchain->targets, NULL);
    if (result != VK_SUCCESS) {
        return result;
    }
    swapchain->target_images = calloc(swapchain->targets, sizeof *swapchain->target_images);
    swapchain->copied = calloc(swapchain->targets, sizeof *swapchain->copied);
    if (swapchain->target_images == NULL || swapchain->copied == NULL) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    result = minato_host_vkGetSwapchainImagesKHR(device, swapchain->moltenvk, &swapchain->targets, swapchain->target_images);
    const VkSemaphoreCreateInfo binary = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (uint32_t index = 0; index < swapchain->targets && result == VK_SUCCESS; index++) {
        result = vkCreateSemaphore(device, &binary, NULL, &swapchain->copied[index]);
    }
    const VkImageCreateInfo image = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                                     .format = info->imageFormat,
                                     .extent = {info->imageExtent.width, info->imageExtent.height, 1},
                                     .mipLevels = 1, .arrayLayers = info->imageArrayLayers,
                                     .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                     .usage = info->imageUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                     .sharingMode = info->imageSharingMode,
                                     .queueFamilyIndexCount = info->queueFamilyIndexCount,
                                     .pQueueFamilyIndices = info->pQueueFamilyIndices,
                                     .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    for (uint32_t index = 0; index < swapchain->count && result == VK_SUCCESS; index++) {
        struct image *ours = &swapchain->images[index];
        VkMemoryRequirements requirements;
        result = vkCreateImage(device, &image, NULL, &ours->image);
        if (result == VK_SUCCESS) {
            vkGetImageMemoryRequirements(device, ours->image, &requirements);
            const VkMemoryAllocateInfo memory = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                                 .allocationSize = requirements.size,
                                                 .memoryTypeIndex = device_memory(physical, requirements.memoryTypeBits)};
            result = vkAllocateMemory(device, &memory, NULL, &ours->memory);
        }
        if (result == VK_SUCCESS) {
            result = vkBindImageMemory(device, ours->image, ours->memory, 0);
        }
    }
    return result;
}

static void copy_teardown(struct swapchain *swapchain) {
    const VkDevice device = swapchain->device;
    if (swapchain->pool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device, swapchain->pool, NULL);
    }
    for (uint32_t index = 0; index < swapchain->count; index++) {
        if (swapchain->images[index].image != VK_NULL_HANDLE) {
            vkDestroyImage(device, swapchain->images[index].image, NULL);
        }
        if (swapchain->images[index].memory != VK_NULL_HANDLE) {
            vkFreeMemory(device, swapchain->images[index].memory, NULL);
        }
    }
    for (uint32_t index = 0; swapchain->copied != NULL && index < swapchain->targets; index++) {
        if (swapchain->copied[index] != VK_NULL_HANDLE) {
            vkDestroySemaphore(device, swapchain->copied[index], NULL);
        }
    }
    free(swapchain->target_images);
    free(swapchain->copied);
}

/* Until every copy that read the app's images is done: each queue's last one. */
static VkResult wait_copies(const struct swapchain *swapchain) {
    VkSemaphore *timelines = malloc(swapchain->count * (sizeof *timelines + sizeof(uint64_t)) + 1);
    if (timelines == NULL) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    uint64_t *values = (uint64_t *)(timelines + swapchain->count);
    uint32_t queues = 0;
    for (uint32_t index = 0; index < swapchain->count; index++) {
        const struct image *image = &swapchain->images[index];
        uint32_t queue = 0;
        while (image->queue != NULL && queue < queues && timelines[queue] != image->queue->timeline) {
            queue++;
        }
        if (image->queue != NULL && queue == queues) {
            timelines[queues] = image->queue->timeline;
            values[queues++] = image->copied;
        } else if (image->queue != NULL && image->copied > values[queue]) {
            values[queue] = image->copied;
        }
    }
    const VkSemaphoreWaitInfo wait = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = queues,
                                      .pSemaphores = timelines, .pValues = values};
    const VkResult result = queues == 0 ? VK_SUCCESS : vkWaitSemaphores(swapchain->device, &wait, UINT64_MAX);
    free(timelines);
    return result;
}

VkResult vkCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *info,
                              const VkAllocationCallbacks *allocator, VkSwapchainKHR *made) {
    (void)allocator;
    struct minato_surface *surface = minato_surface(info->surface);
    const struct swapchain *old = swapchain_of(info->oldSwapchain);
    if (old != NULL) {
        /* Replaced even when this one fails. */
        VkSwapchainKHR current = info->oldSwapchain;
        __atomic_compare_exchange_n(&surface->swapchain, &current, VK_NULL_HANDLE, 0, __ATOMIC_ACQ_REL,
                                    __ATOMIC_ACQUIRE);
    }
    uint32_t owned;
    struct minato_queues *queues = NULL;
    if (!minato_recall(device, &owned, &queues) || queues == NULL) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    const int copy = !direct(queues->physical, surface, info);
    const uint32_t count = copy ? info->minImageCount : 0;
    struct swapchain *swapchain = calloc(1, sizeof *swapchain + count * sizeof swapchain->images[0]);
    if (swapchain == NULL) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    swapchain->surface = surface;
    swapchain->device = device;
    swapchain->extent = info->imageExtent;
    swapchain->window = minato_window_size(surface);
    swapchain->copy = copy;
    swapchain->count = count;
    /* MoltenVK's, on MoltenVK's surface; graphics/host checks that the window is there. */
    const struct minato_windows windows = {MINATO_STRUCTURE_TYPE_WINDOWS, NULL, 1, &surface->id};
    VkSwapchainCreateInfoKHR ours = *info;
    ours.pNext = &windows;
    ours.surface = surface->metal;
    ours.oldSwapchain = old != NULL ? old->moltenvk : VK_NULL_HANDLE;
    ours.compositeAlpha = composite_alpha(info->compositeAlpha);
    if (copy) {
        ours.minImageCount = 3;
        ours.imageFormat = moltenvk_format(info->imageFormat)            ? info->imageFormat
                           : info->imageFormat == VK_FORMAT_R8G8B8A8_SRGB ? VK_FORMAT_B8G8R8A8_SRGB
                                                                          : VK_FORMAT_B8G8R8A8_UNORM;
        ours.imageArrayLayers = 1;
        ours.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        ours.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ours.queueFamilyIndexCount = 0;
        ours.pQueueFamilyIndices = NULL;
    }
    VkResult result = minato_host_vkCreateSwapchainKHR(device, &ours, NULL, &swapchain->moltenvk);
    if (result == VK_SUCCESS && copy && (result = copy_setup(swapchain, queues->physical, info)) != VK_SUCCESS) {
        copy_teardown(swapchain);
        minato_host_vkDestroySwapchainKHR(device, swapchain->moltenvk, NULL);
    }
    if (result != VK_SUCCESS) {
        free(swapchain);
        return result;
    }
    __atomic_store_n(&surface->swapchain, (VkSwapchainKHR)(uintptr_t)swapchain, __ATOMIC_RELEASE);
    *made = (VkSwapchainKHR)(uintptr_t)swapchain;
    return VK_SUCCESS;
}

void vkDestroySwapchainKHR(VkDevice device, VkSwapchainKHR handle, const VkAllocationCallbacks *allocator) {
    (void)allocator;
    struct swapchain *swapchain = swapchain_of(handle);
    if (swapchain == NULL) {
        return;
    }
    VkSwapchainKHR current = handle;
    __atomic_compare_exchange_n(&swapchain->surface->swapchain, &current, VK_NULL_HANDLE, 0, __ATOMIC_ACQ_REL,
                                __ATOMIC_ACQUIRE);
    if (swapchain->copy) {
        wait_copies(swapchain);
        copy_teardown(swapchain);
    }
    minato_host_vkDestroySwapchainKHR(device, swapchain->moltenvk, NULL);
    free(swapchain);
}

VkResult vkGetSwapchainImagesKHR(VkDevice device, VkSwapchainKHR handle, uint32_t *count, VkImage *images) {
    const struct swapchain *swapchain = swapchain_of(handle);
    if (!swapchain->copy) {
        return minato_host_vkGetSwapchainImagesKHR(device, swapchain->moltenvk, count, images);
    }
    if (images == NULL) {
        *count = swapchain->count;
        return VK_SUCCESS;
    }
    const uint32_t copied = *count < swapchain->count ? *count : swapchain->count;
    for (uint32_t index = 0; index < copied; index++) {
        images[index] = swapchain->images[index].image;
    }
    *count = copied;
    return copied < swapchain->count ? VK_INCOMPLETE : VK_SUCCESS;
}

/* The copy way's image for the next acquire: the least recently given back that the app does not hold,
   once the copy that last read it is done. */
static VkResult free_image(struct swapchain *swapchain, uint64_t timeout, struct image **found) {
    struct image *best = NULL;
    for (uint32_t index = 0; index < swapchain->count; index++) {
        struct image *image = &swapchain->images[index];
        if (!image->held && (best == NULL || image->presented < best->presented)) {
            best = image;
        }
    }
    if (best == NULL) {
        return timeout == 0 ? VK_NOT_READY : VK_TIMEOUT;  /* the app holds them all */
    }
    if (best->queue != NULL) {
        const VkSemaphoreWaitInfo wait = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1,
                                          .pSemaphores = &best->queue->timeline, .pValues = &best->copied};
        const VkResult result = vkWaitSemaphores(swapchain->device, &wait, timeout);
        if (result != VK_SUCCESS) {
            return result == VK_TIMEOUT && timeout == 0 ? VK_NOT_READY : result;
        }
    }
    *found = best;
    return VK_SUCCESS;
}

struct acquisition {
    VkDevice device;
    VkSwapchainKHR swapchain;
    VkSemaphore semaphore;
    VkFence fence;
    uint32_t *index;
};

static VkResult acquire_slice(const void *arguments, uint64_t timeout) {
    const struct acquisition *acquisition = arguments;
    return minato_host_vkAcquireNextImageKHR(acquisition->device, acquisition->swapchain, timeout,
                                             acquisition->semaphore, acquisition->fence, acquisition->index);
}

static VkResult acquire(VkDevice device, VkSwapchainKHR handle, uint64_t timeout, VkSemaphore semaphore,
                        VkFence fence, uint32_t *index) {
    struct swapchain *swapchain = swapchain_of(handle);
    if (replaced(swapchain)) {
        return VK_ERROR_OUT_OF_DATE_KHR;
    }
    /* A window that is gone answers at once; in the background this waits, before any GPU work. */
    const uint64_t window = swapchain->surface->id;
    if (minato_gate(window, APKRT_GATE_BACKGROUND)) {
        return VK_ERROR_SURFACE_LOST_KHR;
    }
    struct image *image = NULL;
    if (swapchain->copy) {
        const VkResult result = free_image(swapchain, timeout, &image);
        if (result != VK_SUCCESS) {
            return result;
        }
    }
    uint32_t target = 0;
    const struct acquisition acquisition = {device, swapchain->moltenvk, semaphore, fence, &target};
    VkResult result;
    MINATO_GATED(result, window, minato_sliced(acquire_slice, &acquisition, timeout));
    if (result == VK_SUBOPTIMAL_KHR) {
        result = VK_SUCCESS;  /* another size than the layer's */
    }
    if (result != VK_SUCCESS) {
        return result;
    }
    if (image != NULL) {
        image->held = 1;
        image->target = target;
        *index = (uint32_t)(image - swapchain->images);
    } else {
        *index = target;
    }
    return VK_SUCCESS;
}

VkResult vkAcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout, VkSemaphore semaphore,
                               VkFence fence, uint32_t *index) {
    return acquire(device, swapchain, timeout, semaphore, fence, index);
}

VkResult vkAcquireNextImage2KHR(VkDevice device, const VkAcquireNextImageInfoKHR *info, uint32_t *index) {
    return acquire(device, info->swapchain, info->timeout, info->semaphore, info->fence, index);
}

/* The copies' command pool for the queue family they run on, with a command buffer per image. Another
   family's pool goes once its copies are done. */
static VkResult copy_pool(struct swapchain *swapchain, uint32_t family) {
    if (swapchain->pool != VK_NULL_HANDLE && swapchain->family == family) {
        return VK_SUCCESS;
    }
    if (swapchain->pool != VK_NULL_HANDLE) {
        wait_copies(swapchain);
        vkDestroyCommandPool(swapchain->device, swapchain->pool, NULL);
        swapchain->pool = VK_NULL_HANDLE;
    }
    const VkCommandPoolCreateInfo info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                          .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                          .queueFamilyIndex = family};
    VkResult result = vkCreateCommandPool(swapchain->device, &info, NULL, &swapchain->pool);
    VkCommandBuffer *buffers = malloc(swapchain->count * sizeof *buffers);
    if (result == VK_SUCCESS && buffers == NULL) {
        result = VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    if (result == VK_SUCCESS) {
        const VkCommandBufferAllocateInfo allocate = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                                      .commandPool = swapchain->pool,
                                                      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                                      .commandBufferCount = swapchain->count};
        result = vkAllocateCommandBuffers(swapchain->device, &allocate, buffers);
    }
    for (uint32_t index = 0; result == VK_SUCCESS && index < swapchain->count; index++) {
        swapchain->images[index].commands = buffers[index];
    }
    free(buffers);
    if (result != VK_SUCCESS && swapchain->pool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(swapchain->device, swapchain->pool, NULL);
        swapchain->pool = VK_NULL_HANDLE;
    }
    swapchain->family = family;
    return result;
}

/* The copy of an app's image into MoltenVK's image it was acquired with, after all commands and memory
   writes; the app's image goes back to the layout it was presented in. */
static VkResult record(const struct swapchain *swapchain, const struct image *image) {
    const VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    const VkResult result = vkBeginCommandBuffer(image->commands, &begin);
    if (result != VK_SUCCESS) {
        return result;
    }
    const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageMemoryBarrier barriers[2] = {
        {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
         .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT, .oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
         .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = image->image, .subresourceRange = range},
        {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = 0,
         .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
         .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = swapchain->target_images[image->target],
         .subresourceRange = range}};
    vkCmdPipelineBarrier(image->commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                         NULL, 0, NULL, 2, barriers);
    const VkOffset3D end = {(int32_t)swapchain->extent.width, (int32_t)swapchain->extent.height, 1};
    const VkImageBlit region = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffsets = {{0, 0, 0}, end},
                                .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffsets = {{0, 0, 0}, end}};
    vkCmdBlitImage(image->commands, image->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   swapchain->target_images[image->target], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region,
                   VK_FILTER_NEAREST);
    barriers[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barriers[0].dstAccessMask = 0;
    barriers[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barriers[0].newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barriers[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barriers[1].dstAccessMask = 0;
    barriers[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barriers[1].newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(image->commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0,
                         NULL, 0, NULL, 2, barriers);
    return vkEndCommandBuffer(image->commands);
}

/* Android's ranking of a present's results, worst first. */
static VkResult worse(VkResult one, VkResult other) {
    static const VkResult ranked[] = {VK_ERROR_DEVICE_LOST, VK_ERROR_SURFACE_LOST_KHR, VK_ERROR_OUT_OF_DATE_KHR,
                                      VK_ERROR_OUT_OF_DEVICE_MEMORY, VK_ERROR_OUT_OF_HOST_MEMORY};
    for (size_t index = 0; index < sizeof ranked / sizeof *ranked; index++) {
        if (one == ranked[index] || other == ranked[index]) {
            return ranked[index];
        }
    }
    return one != VK_SUCCESS ? one : other;
}

/* What a present hands MoltenVK, per swapchain that goes on, and per copy. */
struct going {
    VkPresentTimeGOOGLE *times;
    VkSwapchainKHR *handles;
    uint64_t *windows;
    VkSemaphore *semaphores;
    VkCommandBuffer *buffers;
    uint32_t *indices, *which;
    VkResult *results, *theirs;
    VkPipelineStageFlags *stages;
};

static VkResult present(VkQueue queue, const VkPresentInfoKHR *info, struct going *going) {
    const uint32_t count = info->swapchainCount;
    VkResult *results = going->results;
    /* The windows' gates first: closed waits, gone answers without GPU work. */
    for (uint32_t index = 0; index < count; index++) {
        const struct swapchain *swapchain = swapchain_of(info->pSwapchains[index]);
        results[index] = minato_gate(swapchain->surface->id, APKRT_GATE_PRESENT) ? VK_ERROR_SURFACE_LOST_KHR
                         : replaced(swapchain)                                   ? VK_ERROR_OUT_OF_DATE_KHR
                                                                                 : VK_SUCCESS;
    }
    /* Those that go on, with the copies recorded and their times (VK_GOOGLE_display_timing); every image comes
       back to its swapchain. */
    const VkPresentTimesInfoGOOGLE *times = minato_chain_find(info->pNext, VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE);
    if (times != NULL && times->pTimes == NULL) {
        times = NULL;
    }
    struct minato_queue *ours = minato_queue(queue);
    uint32_t on = 0, copies = 0;
    VkResult result = VK_SUCCESS;
    for (uint32_t index = 0; index < count; index++) {
        struct swapchain *swapchain = swapchain_of(info->pSwapchains[index]);
        const uint32_t image_index = info->pImageIndices[index];
        struct image *image = swapchain->copy ? &swapchain->images[image_index] : NULL;
        if (image != NULL) {
            image->held = 0;
            image->presented = ++swapchain->given_back;
        }
        if (results[index] != VK_SUCCESS) {
            continue;
        }
        going->which[on] = index;
        if (times != NULL) {
            going->times[on] = times->pTimes[index];
        }
        going->handles[on] = swapchain->moltenvk;
        going->windows[on] = swapchain->surface->id;
        going->indices[on] = image != NULL ? image->target : image_index;
        on++;
        if (image != NULL && result == VK_SUCCESS) {
            result = ours == NULL ? VK_ERROR_DEVICE_LOST : copy_pool(swapchain, ours->family);
            if (result == VK_SUCCESS) {
                result = record(swapchain, image);
            }
            going->buffers[copies] = image->commands;
            going->semaphores[copies++] = swapchain->copied[image->target];
        }
    }
    /* The copies in one submit on the app's queue after the app's semaphores, which MoltenVK's present then
       does not wait for. */
    if (copies > 0 && result == VK_SUCCESS) {
        for (uint32_t index = 0; index < info->waitSemaphoreCount; index++) {
            going->stages[index] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        }
        const VkSubmitInfo batch = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                                    .waitSemaphoreCount = info->waitSemaphoreCount,
                                    .pWaitSemaphores = info->pWaitSemaphores, .pWaitDstStageMask = going->stages,
                                    .commandBufferCount = copies, .pCommandBuffers = going->buffers,
                                    .signalSemaphoreCount = copies, .pSignalSemaphores = going->semaphores};
        result = minato_queue_submit(queue, 1, &batch, VK_NULL_HANDLE, going->windows[0]);
        for (uint32_t index = 0; result == VK_SUCCESS && index < on; index++) {
            struct swapchain *swapchain = swapchain_of(info->pSwapchains[going->which[index]]);
            if (swapchain->copy) {
                struct image *image = &swapchain->images[info->pImageIndices[going->which[index]]];
                image->queue = ours;
                image->copied = __atomic_load_n(&ours->last, __ATOMIC_RELAXED);
            }
        }
    }
    if (on > 0 && result == VK_SUCCESS) {
        /* graphics/host counts the presents of each window, and hands MoltenVK the rest of the chain. */
        const VkPresentTimesInfoGOOGLE their_times = {VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE, NULL, on,
                                                      going->times};
        const struct minato_windows windows = {MINATO_STRUCTURE_TYPE_WINDOWS, times != NULL ? &their_times : NULL, on,
                                               going->windows};
        const VkPresentInfoKHR moltenvk = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .pNext = &windows,
                                           .waitSemaphoreCount = copies > 0 ? copies : info->waitSemaphoreCount,
                                           .pWaitSemaphores = copies > 0 ? going->semaphores : info->pWaitSemaphores,
                                           .swapchainCount = on, .pSwapchains = going->handles,
                                           .pImageIndices = going->indices, .pResults = going->theirs};
        for (uint32_t index = 0; index < on; index++) {
            going->theirs[index] = VK_RESULT_MAX_ENUM;
        }
        MINATO_GATED(result, going->windows[0], minato_host_vkQueuePresentKHR(queue, &moltenvk));
        for (uint32_t index = 0; index < on; index++) {
            const VkResult theirs = going->theirs[index] != VK_RESULT_MAX_ENUM ? going->theirs[index] : result;
            const VkResult own = theirs == VK_SUBOPTIMAL_KHR ? VK_SUCCESS : theirs;
            const struct swapchain *swapchain = swapchain_of(info->pSwapchains[going->which[index]]);
            results[going->which[index]] = own == VK_SUCCESS && turned(swapchain) ? VK_SUBOPTIMAL_KHR : own;
        }
    } else {
        for (uint32_t index = 0; index < on; index++) {
            results[going->which[index]] = result;
        }
    }
    VkResult worst = VK_SUCCESS;
    for (uint32_t index = 0; index < count; index++) {
        worst = worse(worst, results[index]);
    }
    return worst;
}

VkResult vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *info) {
    const size_t count = info->swapchainCount, waits = info->waitSemaphoreCount;
    /* One allocation: the arrays of 16-byte and 8-byte items, then those of 4-byte ones. */
    char *memory = malloc(count * (sizeof(VkPresentTimeGOOGLE) + sizeof(VkSwapchainKHR) + sizeof(uint64_t) +
                                   sizeof(VkSemaphore) + sizeof(VkCommandBuffer) + 2 * sizeof(uint32_t) +
                                   2 * sizeof(VkResult)) +
                          waits * sizeof(VkPipelineStageFlags) + 1);
    if (memory == NULL) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    struct going going;
    going.times = (VkPresentTimeGOOGLE *)(void *)memory;
    going.handles = (VkSwapchainKHR *)(going.times + count);
    going.windows = (uint64_t *)(going.handles + count);
    going.semaphores = (VkSemaphore *)(going.windows + count);
    going.buffers = (VkCommandBuffer *)(going.semaphores + count);
    going.indices = (uint32_t *)(going.buffers + count);
    going.which = going.indices + count;
    going.results = (VkResult *)(going.which + count);
    going.theirs = going.results + count;
    going.stages = (VkPipelineStageFlags *)(going.theirs + count);
    const VkResult result = present(queue, info, &going);
    if (info->pResults != NULL) {
        memcpy(info->pResults, going.results, count * sizeof *going.results);
    }
    free(memory);
    return result;
}

/* VK_GOOGLE_display_timing (ADR-0041 decision 11): MoltenVK's, on the swapchain's own. As on Android 15, a replaced
   swapchain gives no past timings (swapchain.cpp, GetPastPresentationTimingGOOGLE). */
VkResult vkGetRefreshCycleDurationGOOGLE(VkDevice device, VkSwapchainKHR handle, VkRefreshCycleDurationGOOGLE *duration) {
    return minato_host_vkGetRefreshCycleDurationGOOGLE(device, swapchain_of(handle)->moltenvk, duration);
}

VkResult vkGetPastPresentationTimingGOOGLE(VkDevice device, VkSwapchainKHR handle, uint32_t *count,
                                           VkPastPresentationTimingGOOGLE *timings) {
    const struct swapchain *swapchain = swapchain_of(handle);
    if (replaced(swapchain)) {
        return VK_ERROR_OUT_OF_DATE_KHR;
    }
    return minato_host_vkGetPastPresentationTimingGOOGLE(device, swapchain->moltenvk, count, timings);
}
