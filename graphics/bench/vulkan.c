/* The scene with Vulkan (ADR-0037, decision 1), the same file in both builds. It calls Vulkan only through the
   functions that the platform's vkGetInstanceProcAddr and the device's vkGetDeviceProcAddr give, as volk does: in the
   APKs libvulkan's, Minato's front lib; in the host build MoltenVK's own. A frame, two in flight: the offscreen pass
   (the background, the meshes with 128 bytes of push constants and a vkCmdDrawIndexed each, the particles), then the
   post pass into the swapchain's image. The swapchain takes the surface's first format: R8G8B8A8 from libvulkan,
   which copies it into the drawable, B8G8R8A8 from MoltenVK (decision 1). */
#define VK_NO_PROTOTYPES
#include "bench_vulkan.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "scene.h"

static const uint32_t fullscreen_vert[] =
#include "fullscreen.vert.inc"
    ;
static const uint32_t background_frag[] =
#include "background.frag.inc"
    ;
static const uint32_t fill_frag[] =
#include "fill.frag.inc"
    ;
static const uint32_t mesh_vert[] =
#include "mesh.vert.inc"
    ;
static const uint32_t mesh_frag[] =
#include "mesh.frag.inc"
    ;
static const uint32_t particle_vert[] =
#include "particle.vert.inc"
    ;
static const uint32_t particle_frag[] =
#include "particle.frag.inc"
    ;
static const uint32_t post_frag[] =
#include "post.frag.inc"
    ;

/* ---- The functions, by the names they have */

#define INSTANCE_FUNCTIONS(X)                                                                                         \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceMemoryProperties)                       \
    X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceSurfaceSupportKHR)                             \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) X(vkGetPhysicalDeviceSurfaceFormatsKHR)                            \
    X(vkEnumerateDeviceExtensionProperties) X(vkCreateDevice) X(vkGetDeviceProcAddr) X(vkDestroySurfaceKHR)
#define DEVICE_FUNCTIONS(X)                                                                                           \
    X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkDeviceWaitIdle) X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR)      \
    X(vkGetSwapchainImagesKHR) X(vkAcquireNextImageKHR) X(vkQueuePresentKHR) X(vkQueueSubmit) X(vkCreateImage)       \
    X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory) X(vkCreateImageView)                      \
    X(vkDestroyImageView) X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements)                      \
    X(vkBindBufferMemory) X(vkAllocateMemory) X(vkFreeMemory) X(vkMapMemory) X(vkCreateSampler) X(vkDestroySampler)  \
    X(vkCreateRenderPass) X(vkDestroyRenderPass) X(vkCreateFramebuffer) X(vkDestroyFramebuffer)                      \
    X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout)  \
    X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets)       \
    X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) X(vkCreateGraphicsPipelines) X(vkDestroyPipeline)           \
    X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer)               \
    X(vkEndCommandBuffer) X(vkResetCommandBuffer) X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences)              \
    X(vkResetFences) X(vkCreateSemaphore) X(vkDestroySemaphore) X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass)        \
    X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdBindVertexBuffers) X(vkCmdBindIndexBuffer)                \
    X(vkCmdPushConstants) X(vkCmdDraw) X(vkCmdDrawIndexed) X(vkCmdSetViewport) X(vkCmdSetScissor)                    \
    X(vkCmdPipelineBarrier) X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer)

#define DECLARE(name) static PFN_##name name;
static PFN_vkCreateInstance vkCreateInstance;
INSTANCE_FUNCTIONS(DECLARE)
DEVICE_FUNCTIONS(DECLARE)

static int load_instance(PFN_vkGetInstanceProcAddr get, VkInstance instance) {
#define LOAD_INSTANCE(name)                                  \
    if ((name = (PFN_##name)get(instance, #name)) == NULL) { \
        bench_log("no %s", #name);                           \
        return -1;                                           \
    }
    INSTANCE_FUNCTIONS(LOAD_INSTANCE)
    return 0;
}

static int load_device(VkDevice device) {
#define LOAD_DEVICE(name)                                                   \
    if ((name = (PFN_##name)vkGetDeviceProcAddr(device, #name)) == NULL) { \
        bench_log("no %s", #name);                                          \
        return -1;                                                          \
    }
    DEVICE_FUNCTIONS(LOAD_DEVICE)
    return 0;
}

/* ---- The scene's objects */

enum { FRAMES = 2, IMAGES = 8 };
static const VkFormat target_format = VK_FORMAT_R8G8B8A8_UNORM, depth_format = VK_FORMAT_D32_SFLOAT;

struct image {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
};

struct buffer {
    VkBuffer buffer;
    VkDeviceMemory memory;
    void *mapped;
};

struct bench_vulkan {
    int meshes, check;
    int cold;  // 1 once the check frame was drawn before the first frame
    float aspect;
    VkInstance instance;
    VkSurfaceKHR surface;
    VkPhysicalDevice physical;
    VkPhysicalDeviceMemoryProperties memory;
    uint32_t family;
    VkDevice device;
    VkQueue queue;
    VkCommandPool pool;
    VkCommandBuffer commands[FRAMES];
    VkFence fences[FRAMES];
    VkSemaphore acquired[FRAMES];
    VkSwapchainKHR swapchain;
    VkFormat format;
    VkExtent2D extent;
    VkExtent2D target_extent;  // the offscreen target's, the swapchain's at scale 100
    int scale;
    uint32_t image_count;
    VkImage images[IMAGES];
    VkImageView views[IMAGES];
    VkFramebuffer framebuffers[IMAGES];
    VkSemaphore rendered[IMAGES];
    VkRenderPass scene_pass, post_pass;
    struct image target, depth, texture;
    VkFramebuffer target_framebuffer;
    VkSampler repeat, clamp;
    VkDescriptorSetLayout set_layout;
    VkDescriptorPool descriptors;
    VkDescriptorSet texture_set, target_set;
    VkPipelineLayout layout;
    VkPipeline background, fill, mesh, particles, post;
    int fill_layers;
    struct buffer vertices, indices, readback;
};

/* The push constants of the background, the particles and the post pass. */
struct push {
    float time, aspect;
};
/* The fill-heavy load's layer: the push constants of fill.frag. */
struct fill_push {
    float time, aspect, layer;
};

static int ok(VkResult result, const char *what) {
    if (result != VK_SUCCESS) bench_log("%s failed: %d", what, result);
    return result == VK_SUCCESS;
}

static uint32_t memory_type(const struct bench_vulkan *scene, uint32_t bits, VkMemoryPropertyFlags flags) {
    for (uint32_t index = 0; index < scene->memory.memoryTypeCount; ++index) {
        if ((bits & (1U << index)) != 0 && (scene->memory.memoryTypes[index].propertyFlags & flags) == flags) {
            return index;
        }
    }
    return UINT32_MAX;
}

/* A buffer in host-visible, coherent memory, mapped for good. */
static int make_buffer(struct bench_vulkan *scene, VkDeviceSize size, VkBufferUsageFlags usage,
                       struct buffer *buffer) {
    const VkBufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage};
    if (!ok(vkCreateBuffer(scene->device, &info, NULL, &buffer->buffer), "vkCreateBuffer")) return -1;
    VkMemoryRequirements needs;
    vkGetBufferMemoryRequirements(scene->device, buffer->buffer, &needs);
    const VkMemoryAllocateInfo allocate = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = needs.size,
        .memoryTypeIndex = memory_type(scene, needs.memoryTypeBits,
                                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    return ok(vkAllocateMemory(scene->device, &allocate, NULL, &buffer->memory), "vkAllocateMemory") &&
                   ok(vkBindBufferMemory(scene->device, buffer->buffer, buffer->memory, 0), "vkBindBufferMemory") &&
                   ok(vkMapMemory(scene->device, buffer->memory, 0, VK_WHOLE_SIZE, 0, &buffer->mapped), "vkMapMemory")
               ? 0
               : -1;
}

static void destroy_buffer(const struct bench_vulkan *scene, struct buffer *buffer) {
    vkDestroyBuffer(scene->device, buffer->buffer, NULL);
    vkFreeMemory(scene->device, buffer->memory, NULL);
    memset(buffer, 0, sizeof *buffer);
}

/* An image in device memory, with a view of all its levels. */
static int make_image(struct bench_vulkan *scene, VkFormat format, VkExtent2D extent, uint32_t levels,
                      VkImageUsageFlags usage, VkImageAspectFlags aspect, struct image *image) {
    const VkImageCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = format,
        .extent = {extent.width, extent.height, 1}, .mipLevels = levels, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    if (!ok(vkCreateImage(scene->device, &info, NULL, &image->image), "vkCreateImage")) return -1;
    VkMemoryRequirements needs;
    vkGetImageMemoryRequirements(scene->device, image->image, &needs);
    const VkMemoryAllocateInfo allocate = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = needs.size,
        .memoryTypeIndex = memory_type(scene, needs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    const VkImageViewCreateInfo view = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = format,
        .subresourceRange = {aspect, 0, levels, 0, 1}};
    if (!ok(vkAllocateMemory(scene->device, &allocate, NULL, &image->memory), "vkAllocateMemory") ||
        !ok(vkBindImageMemory(scene->device, image->image, image->memory, 0), "vkBindImageMemory")) {
        return -1;
    }
    VkImageViewCreateInfo ours = view;
    ours.image = image->image;
    return ok(vkCreateImageView(scene->device, &ours, NULL, &image->view), "vkCreateImageView") ? 0 : -1;
}

static void destroy_image(const struct bench_vulkan *scene, struct image *image) {
    vkDestroyImageView(scene->device, image->view, NULL);
    vkDestroyImage(scene->device, image->image, NULL);
    vkFreeMemory(scene->device, image->memory, NULL);
    memset(image, 0, sizeof *image);
}

static void barrier(VkCommandBuffer commands, VkImage image, uint32_t levels, VkImageLayout from, VkImageLayout to,
                    VkPipelineStageFlags source, VkAccessFlags source_access, VkPipelineStageFlags destination,
                    VkAccessFlags destination_access) {
    const VkImageMemoryBarrier info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = source_access,
        .dstAccessMask = destination_access, .oldLayout = from, .newLayout = to,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1}};
    vkCmdPipelineBarrier(commands, source, destination, 0, 0, NULL, 0, NULL, 1, &info);
}

/* ---- Instance, device and swapchain */

static int start_device(struct bench_vulkan *scene, const struct bench_vulkan_platform *platform) {
    vkCreateInstance = (PFN_vkCreateInstance)platform->get_instance_proc_addr(VK_NULL_HANDLE, "vkCreateInstance");
    const VkApplicationInfo application = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                           .pApplicationName = "bench", .apiVersion = VK_API_VERSION_1_3};
    const VkInstanceCreateInfo instance = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pNext = platform->next, .pApplicationInfo = &application,
        .enabledExtensionCount = platform->extension_count, .ppEnabledExtensionNames = platform->extensions};
    if (vkCreateInstance == NULL || !ok(vkCreateInstance(&instance, NULL, &scene->instance), "vkCreateInstance") ||
        load_instance(platform->get_instance_proc_addr, scene->instance) != 0 ||
        !ok(platform->create_surface(platform->context, scene->instance, platform->get_instance_proc_addr,
                                     &scene->surface),
            "the surface")) {
        return -1;
    }
    uint32_t count = 1;
    const VkResult enumerated = vkEnumeratePhysicalDevices(scene->instance, &count, &scene->physical);
    if ((enumerated != VK_SUCCESS && enumerated != VK_INCOMPLETE) || count == 0) {
        bench_log("no physical device: %d", enumerated);
        return -1;
    }
    vkGetPhysicalDeviceMemoryProperties(scene->physical, &scene->memory);

    VkQueueFamilyProperties families[16];
    count = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(scene->physical, &count, families);
    scene->family = UINT32_MAX;
    for (uint32_t family = 0; family < count && scene->family == UINT32_MAX; ++family) {
        VkBool32 presents = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(scene->physical, family, scene->surface, &presents);
        if ((families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0 && presents) scene->family = family;
    }
    if (scene->family == UINT32_MAX) {
        bench_log("no queue family draws and presents");
        return -1;
    }

    // The specification asks for the portability subset where a device offers it, as MoltenVK's does.
    count = 0;
    vkEnumerateDeviceExtensionProperties(scene->physical, NULL, &count, NULL);
    VkExtensionProperties *offered = calloc(count, sizeof *offered);
    if (offered == NULL) return -1;
    vkEnumerateDeviceExtensionProperties(scene->physical, NULL, &count, offered);
    const char *names[2] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, "VK_KHR_portability_subset"};
    uint32_t enabled = 1;
    for (uint32_t index = 0; index < count; ++index) {
        if (strcmp(offered[index].extensionName, names[1]) == 0) enabled = 2;
    }
    free(offered);
    const float priority = 1;
    const VkDeviceQueueCreateInfo queue = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                           .queueFamilyIndex = scene->family, .queueCount = 1,
                                           .pQueuePriorities = &priority};
    const VkDeviceCreateInfo device = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
                                       .pQueueCreateInfos = &queue, .enabledExtensionCount = enabled,
                                       .ppEnabledExtensionNames = names};
    if (!ok(vkCreateDevice(scene->physical, &device, NULL, &scene->device), "vkCreateDevice")) return -1;
    if (load_device(scene->device) != 0) {
        if (vkDestroyDevice != NULL) vkDestroyDevice(scene->device, NULL);
        scene->device = VK_NULL_HANDLE;
        return -1;
    }
    vkGetDeviceQueue(scene->device, scene->family, 0, &scene->queue);

    const VkCommandPoolCreateInfo pool = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                          .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                          .queueFamilyIndex = scene->family};
    const VkCommandBufferAllocateInfo allocate = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                                  .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                                  .commandBufferCount = FRAMES};
    if (!ok(vkCreateCommandPool(scene->device, &pool, NULL, &scene->pool), "vkCreateCommandPool")) return -1;
    VkCommandBufferAllocateInfo ours = allocate;
    ours.commandPool = scene->pool;
    if (!ok(vkAllocateCommandBuffers(scene->device, &ours, scene->commands), "vkAllocateCommandBuffers")) return -1;
    const VkFenceCreateInfo fence = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                                     .flags = VK_FENCE_CREATE_SIGNALED_BIT};
    const VkSemaphoreCreateInfo semaphore = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (int slot = 0; slot < FRAMES; ++slot) {
        if (!ok(vkCreateFence(scene->device, &fence, NULL, &scene->fences[slot]), "vkCreateFence") ||
            !ok(vkCreateSemaphore(scene->device, &semaphore, NULL, &scene->acquired[slot]), "vkCreateSemaphore")) {
            return -1;
        }
    }
    return 0;
}

/* Three images of the surface's first format at its size, FIFO, opaque; with a readback, TRANSFER_SRC too. */
static int make_swapchain(struct bench_vulkan *scene) {
    VkSurfaceCapabilitiesKHR capabilities;
    if (!ok(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(scene->physical, scene->surface, &capabilities),
            "vkGetPhysicalDeviceSurfaceCapabilitiesKHR")) {
        return -1;
    }
    VkSurfaceFormatKHR format;
    uint32_t count = 1;
    const VkResult formats = vkGetPhysicalDeviceSurfaceFormatsKHR(scene->physical, scene->surface, &count, &format);
    if ((formats != VK_SUCCESS && formats != VK_INCOMPLETE) || count == 0 ||
        capabilities.currentExtent.width == UINT32_MAX) {
        bench_log("the surface has no format or no size: %d", formats);
        return -1;
    }
    scene->format = format.format;
    scene->extent = capabilities.currentExtent;
    scene->aspect = (float)scene->extent.width / (float)scene->extent.height;
    int target_width, target_height;
    bench_target_size((int)scene->extent.width, (int)scene->extent.height, scene->scale, &target_width,
                      &target_height);
    scene->target_extent = (VkExtent2D){(uint32_t)target_width, (uint32_t)target_height};
    uint32_t images = capabilities.minImageCount > 3 ? capabilities.minImageCount : 3;
    if (capabilities.maxImageCount != 0 && images > capabilities.maxImageCount) images = capabilities.maxImageCount;
    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    const VkCompositeAlphaFlagBitsKHR alphas[4] = {
        VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR, VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR};
    for (int index = 0; index < 4; ++index) {
        if ((capabilities.supportedCompositeAlpha & alphas[index]) != 0) alpha = alphas[index];
    }
    const VkSwapchainCreateInfoKHR info = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = scene->surface, .minImageCount = images,
        .imageFormat = format.format, .imageColorSpace = format.colorSpace, .imageExtent = scene->extent,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                      (scene->check >= 0 ? (VkImageUsageFlags)VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0U),
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE, .preTransform = capabilities.currentTransform,
        .compositeAlpha = alpha, .presentMode = VK_PRESENT_MODE_FIFO_KHR, .clipped = VK_TRUE};
    if (!ok(vkCreateSwapchainKHR(scene->device, &info, NULL, &scene->swapchain), "vkCreateSwapchainKHR")) return -1;
    scene->image_count = IMAGES;
    if (!ok(vkGetSwapchainImagesKHR(scene->device, scene->swapchain, &scene->image_count, scene->images),
            "vkGetSwapchainImagesKHR")) {
        return -1;
    }
    const VkSemaphoreCreateInfo semaphore = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (uint32_t index = 0; index < scene->image_count; ++index) {
        const VkImageViewCreateInfo view = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = scene->images[index],
            .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = scene->format,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        if (!ok(vkCreateImageView(scene->device, &view, NULL, &scene->views[index]), "vkCreateImageView") ||
            !ok(vkCreateSemaphore(scene->device, &semaphore, NULL, &scene->rendered[index]), "vkCreateSemaphore")) {
            return -1;
        }
    }
    return 0;
}

/* ---- Passes and pipelines */

/* The offscreen pass: the target cleared and left for the post pass to sample, the depth cleared and dropped. The
   dependencies order it after the frame before: its post pass read the target, its meshes wrote the depth. */
static VkRenderPass make_scene_pass(const struct bench_vulkan *scene) {
    const VkAttachmentDescription attachments[2] = {
        {.format = target_format, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
         .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
         .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
         .finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {.format = depth_format, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
         .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
         .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
         .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL}};
    const VkAttachmentReference color = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    const VkAttachmentReference depth = {1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    const VkSubpassDescription subpass = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                          .colorAttachmentCount = 1, .pColorAttachments = &color,
                                          .pDepthStencilAttachment = &depth};
    const VkPipelineStageFlags tests =
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    const VkSubpassDependency dependencies[2] = {
        {.srcSubpass = VK_SUBPASS_EXTERNAL, .dstSubpass = 0,
         .srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | tests | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
         .dstStageMask = tests | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
         .srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
         .dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                          VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT},
        {.srcSubpass = 0, .dstSubpass = VK_SUBPASS_EXTERNAL,
         .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
         .dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
         .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, .dstAccessMask = VK_ACCESS_SHADER_READ_BIT}};
    const VkRenderPassCreateInfo info = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 2,
                                         .pAttachments = attachments, .subpassCount = 1, .pSubpasses = &subpass,
                                         .dependencyCount = 2, .pDependencies = dependencies};
    VkRenderPass pass = VK_NULL_HANDLE;
    ok(vkCreateRenderPass(scene->device, &info, NULL, &pass), "vkCreateRenderPass");
    return pass;
}

/* The post pass: the swapchain's image, all of it drawn, then presented. */
static VkRenderPass make_post_pass(const struct bench_vulkan *scene) {
    const VkAttachmentDescription attachment = {
        .format = scene->format, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR};
    const VkAttachmentReference color = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    const VkSubpassDescription subpass = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                          .colorAttachmentCount = 1, .pColorAttachments = &color};
    const VkSubpassDependency dependency = {
        .srcSubpass = VK_SUBPASS_EXTERNAL, .dstSubpass = 0,
        .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT};
    const VkRenderPassCreateInfo info = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1,
                                         .pAttachments = &attachment, .subpassCount = 1, .pSubpasses = &subpass,
                                         .dependencyCount = 1, .pDependencies = &dependency};
    VkRenderPass pass = VK_NULL_HANDLE;
    ok(vkCreateRenderPass(scene->device, &info, NULL, &pass), "vkCreateRenderPass");
    return pass;
}

static VkShaderModule shader(const struct bench_vulkan *scene, const uint32_t *code, size_t size) {
    const VkShaderModuleCreateInfo info = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = size,
                                           .pCode = code};
    VkShaderModule module = VK_NULL_HANDLE;
    ok(vkCreateShaderModule(scene->device, &info, NULL, &module), "vkCreateShaderModule");
    return module;
}

enum kind { kFullScreen, kMesh, kParticles, kFill };

/* A pipeline: a full-screen triangle without tests, the meshes with their vertices, depth and back faces culled,
   or the particles, blended over the depth without writing it. */
static VkPipeline pipeline(const struct bench_vulkan *scene, const uint32_t *vertex, size_t vertex_size,
                           const uint32_t *fragment, size_t fragment_size, VkRenderPass pass, enum kind kind) {
    const VkShaderModule modules[2] = {shader(scene, vertex, vertex_size), shader(scene, fragment, fragment_size)};
    // mesh.frag's constant 0: the offscreen target's height, for the meshes' texture level.
    const float height = (float)scene->target_extent.height;
    const VkSpecializationMapEntry entry = {0, 0, sizeof height};
    const VkSpecializationInfo specialization = {1, &entry, sizeof height, &height};
    const VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = modules[0], .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = modules[1], .pName = "main", .pSpecializationInfo = kind == kMesh ? &specialization : NULL}};
    const VkVertexInputBindingDescription binding = {0, sizeof(struct bench_vertex), VK_VERTEX_INPUT_RATE_VERTEX};
    const VkVertexInputAttributeDescription attributes[3] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(struct bench_vertex, position)},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(struct bench_vertex, normal)},
        {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(struct bench_vertex, uv)}};
    const VkPipelineVertexInputStateCreateInfo input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = kind == kMesh ? 1 : 0, .pVertexBindingDescriptions = &binding,
        .vertexAttributeDescriptionCount = kind == kMesh ? 3 : 0, .pVertexAttributeDescriptions = attributes};
    const VkPipelineInputAssemblyStateCreateInfo assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    const VkPipelineViewportStateCreateInfo viewport = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                                        .viewportCount = 1, .scissorCount = 1};
    const VkPipelineRasterizationStateCreateInfo raster = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = kind == kMesh ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1};
    const VkPipelineMultisampleStateCreateInfo multisample = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    const VkPipelineDepthStencilStateCreateInfo depth = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = kind != kFullScreen && kind != kFill, .depthWriteEnable = kind == kMesh,
        .depthCompareOp = VK_COMPARE_OP_LESS};
    const VkPipelineColorBlendAttachmentState blend = {
        .blendEnable = kind == kParticles || kind == kFill, .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA, .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .alphaBlendOp = VK_BLEND_OP_ADD,
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                          VK_COLOR_COMPONENT_A_BIT};
    const VkPipelineColorBlendStateCreateInfo colors = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1,
        .pAttachments = &blend};
    const VkDynamicState states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    const VkPipelineDynamicStateCreateInfo dynamic = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                                      .dynamicStateCount = 2, .pDynamicStates = states};
    const VkGraphicsPipelineCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = stages,
        .pVertexInputState = &input, .pInputAssemblyState = &assembly, .pViewportState = &viewport,
        .pRasterizationState = &raster, .pMultisampleState = &multisample, .pDepthStencilState = &depth,
        .pColorBlendState = &colors, .pDynamicState = &dynamic, .layout = scene->layout, .renderPass = pass};
    VkPipeline made = VK_NULL_HANDLE;
    ok(vkCreateGraphicsPipelines(scene->device, VK_NULL_HANDLE, 1, &info, NULL, &made), "vkCreateGraphicsPipelines");
    vkDestroyShaderModule(scene->device, modules[0], NULL);
    vkDestroyShaderModule(scene->device, modules[1], NULL);
    return made;
}

#define SPIRV(name) name, sizeof name

static int make_pipelines(struct bench_vulkan *scene) {
    const VkDescriptorSetLayoutBinding binding = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                                  VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
    const VkDescriptorSetLayoutCreateInfo set = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                 .bindingCount = 1, .pBindings = &binding};
    if (!ok(vkCreateDescriptorSetLayout(scene->device, &set, NULL, &scene->set_layout),
            "vkCreateDescriptorSetLayout")) {
        return -1;
    }
    const VkPushConstantRange range = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                       sizeof(struct bench_mesh)};
    const VkPipelineLayoutCreateInfo layout = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                               .setLayoutCount = 1, .pSetLayouts = &scene->set_layout,
                                               .pushConstantRangeCount = 1, .pPushConstantRanges = &range};
    if (!ok(vkCreatePipelineLayout(scene->device, &layout, NULL, &scene->layout), "vkCreatePipelineLayout")) return -1;
    scene->scene_pass = make_scene_pass(scene);
    scene->post_pass = make_post_pass(scene);
    if (scene->scene_pass == VK_NULL_HANDLE || scene->post_pass == VK_NULL_HANDLE) return -1;
    scene->background =
        pipeline(scene, SPIRV(fullscreen_vert), SPIRV(background_frag), scene->scene_pass, kFullScreen);
    // The fill load's pipeline only for that load, so the others pay nothing for it.
    if (scene->fill_layers > 0) {
        scene->fill = pipeline(scene, SPIRV(fullscreen_vert), SPIRV(fill_frag), scene->scene_pass, kFill);
    }
    scene->mesh = pipeline(scene, SPIRV(mesh_vert), SPIRV(mesh_frag), scene->scene_pass, kMesh);
    scene->particles = pipeline(scene, SPIRV(particle_vert), SPIRV(particle_frag), scene->scene_pass, kParticles);
    scene->post = pipeline(scene, SPIRV(fullscreen_vert), SPIRV(post_frag), scene->post_pass, kFullScreen);
    return scene->background != VK_NULL_HANDLE && (scene->fill_layers == 0 || scene->fill != VK_NULL_HANDLE) &&
                   scene->mesh != VK_NULL_HANDLE && scene->particles != VK_NULL_HANDLE && scene->post != VK_NULL_HANDLE
               ? 0
               : -1;
}

/* ---- The targets, the texture, the sphere */

static int make_targets(struct bench_vulkan *scene) {
    if (make_image(scene, target_format, scene->target_extent, 1,
                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                   &scene->target) != 0 ||
        make_image(scene, depth_format, scene->target_extent, 1, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                   VK_IMAGE_ASPECT_DEPTH_BIT, &scene->depth) != 0) {
        return -1;
    }
    const VkImageView views[2] = {scene->target.view, scene->depth.view};
    VkFramebufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                    .renderPass = scene->scene_pass, .attachmentCount = 2, .pAttachments = views,
                                    .width = scene->target_extent.width, .height = scene->target_extent.height,
                                    .layers = 1};
    if (!ok(vkCreateFramebuffer(scene->device, &info, NULL, &scene->target_framebuffer), "vkCreateFramebuffer")) {
        return -1;
    }
    info.renderPass = scene->post_pass;
    info.attachmentCount = 1;
    for (uint32_t index = 0; index < scene->image_count; ++index) {
        info.pAttachments = &scene->views[index];
        if (!ok(vkCreateFramebuffer(scene->device, &info, NULL, &scene->framebuffers[index]), "vkCreateFramebuffer")) {
            return -1;
        }
    }
    return 0;
}

/* Submits the commands recorded in the first frame's buffer and waits for the device. */
static int run_once(struct bench_vulkan *scene) {
    const VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1,
                                 .pCommandBuffers = &scene->commands[0]};
    return ok(vkEndCommandBuffer(scene->commands[0]), "vkEndCommandBuffer") &&
                   ok(vkQueueSubmit(scene->queue, 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit") &&
                   ok(vkDeviceWaitIdle(scene->device), "vkDeviceWaitIdle")
               ? 0
               : -1;
}

static int begin(VkCommandBuffer commands) {
    const VkCommandBufferBeginInfo info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                           .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    return ok(vkResetCommandBuffer(commands, 0), "vkResetCommandBuffer") &&
                   ok(vkBeginCommandBuffer(commands, &info), "vkBeginCommandBuffer")
               ? 0
               : -1;
}

/* The texture with its levels, through a staging buffer; the sphere's vertices and indices in buffers of their own. */
static int make_contents(struct bench_vulkan *scene) {
    struct buffer staging = {0};
    const VkExtent2D size = {BENCH_TEXTURE_SIZE, BENCH_TEXTURE_SIZE};
    if (make_buffer(scene, bench_texture_bytes(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &staging) != 0 ||
        make_image(scene, VK_FORMAT_R8G8B8A8_UNORM, size, BENCH_TEXTURE_LEVELS,
                   VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                   &scene->texture) != 0 ||
        begin(scene->commands[0]) != 0) {
        destroy_buffer(scene, &staging);
        return -1;
    }
    bench_texture(staging.mapped);
    VkBufferImageCopy regions[BENCH_TEXTURE_LEVELS];
    VkDeviceSize offset = 0;
    for (uint32_t level = 0; level < BENCH_TEXTURE_LEVELS; ++level) {
        const uint32_t side = (uint32_t)BENCH_TEXTURE_SIZE >> level;
        regions[level] = (VkBufferImageCopy){.bufferOffset = offset,
                                             .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1},
                                             .imageExtent = {side, side, 1}};
        offset += 4 * (VkDeviceSize)side * side;
    }
    VkCommandBuffer commands = scene->commands[0];
    barrier(commands, scene->texture.image, BENCH_TEXTURE_LEVELS, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT);
    vkCmdCopyBufferToImage(commands, staging.buffer, scene->texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           BENCH_TEXTURE_LEVELS, regions);
    barrier(commands, scene->texture.image, BENCH_TEXTURE_LEVELS, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    const int uploaded = run_once(scene);
    destroy_buffer(scene, &staging);
    if (uploaded != 0 ||
        make_buffer(scene, BENCH_VERTICES * sizeof(struct bench_vertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                    &scene->vertices) != 0 ||
        make_buffer(scene, BENCH_INDICES * sizeof(uint16_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, &scene->indices) != 0) {
        return -1;
    }
    bench_sphere(scene->vertices.mapped, scene->indices.mapped);
    if (scene->check >= 0 &&
        make_buffer(scene, 4 * (VkDeviceSize)scene->extent.width * scene->extent.height,
                    VK_BUFFER_USAGE_TRANSFER_DST_BIT, &scene->readback) != 0) {
        return -1;
    }
    return 0;
}

/* The samplers and the descriptor sets: the texture repeated with its levels, the target clamped. */
static int make_descriptors(struct bench_vulkan *scene) {
    VkSamplerCreateInfo sampler = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR, .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT, .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .maxLod = VK_LOD_CLAMP_NONE};
    if (!ok(vkCreateSampler(scene->device, &sampler, NULL, &scene->repeat), "vkCreateSampler")) return -1;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.maxLod = 0;
    if (!ok(vkCreateSampler(scene->device, &sampler, NULL, &scene->clamp), "vkCreateSampler")) return -1;
    const VkDescriptorPoolSize sizes = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2};
    const VkDescriptorPoolCreateInfo pool = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 2,
                                             .poolSizeCount = 1, .pPoolSizes = &sizes};
    if (!ok(vkCreateDescriptorPool(scene->device, &pool, NULL, &scene->descriptors), "vkCreateDescriptorPool")) {
        return -1;
    }
    const VkDescriptorSetLayout layouts[2] = {scene->set_layout, scene->set_layout};
    const VkDescriptorSetAllocateInfo allocate = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                                  .descriptorPool = scene->descriptors, .descriptorSetCount = 2,
                                                  .pSetLayouts = layouts};
    VkDescriptorSet sets[2];
    if (!ok(vkAllocateDescriptorSets(scene->device, &allocate, sets), "vkAllocateDescriptorSets")) return -1;
    scene->texture_set = sets[0];
    scene->target_set = sets[1];
    const VkDescriptorImageInfo images[2] = {
        {scene->repeat, scene->texture.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {scene->clamp, scene->target.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    VkWriteDescriptorSet writes[2];
    for (int index = 0; index < 2; ++index) {
        writes[index] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = sets[index], .dstBinding = 0,
            .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &images[index]};
    }
    vkUpdateDescriptorSets(scene->device, 2, writes, 0, NULL);
    return 0;
}

struct bench_vulkan *bench_vulkan_start(const struct bench_vulkan_platform *platform, int meshes, int check,
                                        int scale, int fill_layers) {
    struct bench_vulkan *scene = calloc(1, sizeof *scene);
    if (scene == NULL) return NULL;
    scene->meshes = meshes;
    scene->check = check;
    scene->scale = scale;
    scene->fill_layers = fill_layers;
    if (start_device(scene, platform) != 0 || make_swapchain(scene) != 0 || make_pipelines(scene) != 0 ||
        make_targets(scene) != 0 || make_contents(scene) != 0 || make_descriptors(scene) != 0) {
        bench_vulkan_stop(scene);
        return NULL;
    }
    bench_log("Vulkan: %u x %u, format %d, %u images", scene->extent.width, scene->extent.height, scene->format,
              scene->image_count);
    bench_log("target: %u x %u (%d %%)", scene->target_extent.width, scene->target_extent.height, scale);
    const VkPipeline pipelines[5] = {scene->background, scene->fill, scene->mesh, scene->particles, scene->post};
    int built = 0;
    for (int index = 0; index < 5; ++index) built += pipelines[index] != VK_NULL_HANDLE;
    bench_log("pipelines: %d", built);
    return scene;
}

/* ---- A frame */

static void record(const struct bench_vulkan *scene, VkCommandBuffer commands, uint32_t image, int frame) {
    const VkViewport viewport = {0, 0, (float)scene->extent.width, (float)scene->extent.height, 0, 1};
    const VkRect2D area = {{0, 0}, scene->extent};
    const VkViewport target_viewport = {0, 0, (float)scene->target_extent.width, (float)scene->target_extent.height,
                                        0, 1};
    const VkRect2D target_area = {{0, 0}, scene->target_extent};
    const struct push push = {bench_time(frame), scene->aspect};
    const VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

    const VkClearValue clears[2] = {{.color = {.float32 = {0, 0, 0, 1}}}, {.depthStencil = {1, 0}}};
    const VkRenderPassBeginInfo offscreen = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                             .renderPass = scene->scene_pass, .framebuffer = scene->target_framebuffer,
                                             .renderArea = target_area, .clearValueCount = 2,
                                             .pClearValues = clears};
    vkCmdBeginRenderPass(commands, &offscreen, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdSetViewport(commands, 0, 1, &target_viewport);
    vkCmdSetScissor(commands, 0, 1, &target_area);
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->background);
    vkCmdPushConstants(commands, scene->layout, stages, 0, sizeof push, &push);
    vkCmdDraw(commands, 3, 1, 0, 0);

    if (scene->fill_layers > 0) {
        // Full-screen alpha layers over the background, M6's MetalFX plan, step 0.
        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->fill);
        vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->layout, 0, 1, &scene->texture_set, 0,
                                NULL);
        for (int layer = 0; layer < scene->fill_layers; ++layer) {
            const struct fill_push layer_push = {push.time, push.aspect, (float)layer};
            vkCmdPushConstants(commands, scene->layout, stages, 0, sizeof layer_push, &layer_push);
            vkCmdDraw(commands, 3, 1, 0, 0);
        }
    }

    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->mesh);
    const VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(commands, 0, 1, &scene->vertices.buffer, &zero);
    vkCmdBindIndexBuffer(commands, scene->indices.buffer, 0, VK_INDEX_TYPE_UINT16);
    vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->layout, 0, 1, &scene->texture_set, 0,
                            NULL);
    for (int index = 0; index < scene->meshes; ++index) {
        struct bench_mesh mesh;
        bench_mesh(index, frame, scene->aspect, &mesh);
        vkCmdPushConstants(commands, scene->layout, stages, 0, sizeof mesh, &mesh);
        vkCmdDrawIndexed(commands, BENCH_INDICES, 1, 0, 0, 0);
    }

    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->particles);
    vkCmdPushConstants(commands, scene->layout, stages, 0, sizeof push, &push);
    vkCmdDraw(commands, 6, (uint32_t)scene->meshes, 0, 0);
    vkCmdEndRenderPass(commands);

    const VkRenderPassBeginInfo post = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                        .renderPass = scene->post_pass, .framebuffer = scene->framebuffers[image],
                                        .renderArea = area};
    vkCmdBeginRenderPass(commands, &post, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdSetViewport(commands, 0, 1, &viewport);
    vkCmdSetScissor(commands, 0, 1, &area);
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->post);
    vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, scene->layout, 0, 1, &scene->target_set, 0,
                            NULL);
    vkCmdDraw(commands, 3, 1, 0, 0);
    vkCmdEndRenderPass(commands);
}

/* The window's image into the readback buffer, and back for the present. */
static void record_readback(const struct bench_vulkan *scene, VkCommandBuffer commands, uint32_t image) {
    barrier(commands, scene->images[image], 1, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    const VkBufferImageCopy region = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                      .imageExtent = {scene->extent.width, scene->extent.height, 1}};
    vkCmdCopyImageToBuffer(commands, scene->images[image], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, scene->readback.buffer,
                           1, &region);
    barrier(commands, scene->images[image], 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0);
    const VkBufferMemoryBarrier host = {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                                        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
                                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                        .buffer = scene->readback.buffer, .size = VK_WHOLE_SIZE};
    vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &host, 0,
                         NULL);
}

/* The CRC of the image read back, its bytes in RGBA order whatever the format, so both builds' are alike; `cold`
   for the check frame drawn before the first frame. */
static void log_crc(const struct bench_vulkan *scene, int frame, int cold) {
    const size_t row = 4 * (size_t)scene->extent.width;
    uint8_t *line = malloc(row);
    if (line == NULL) return;
    const int bgra = scene->format == VK_FORMAT_B8G8R8A8_UNORM || scene->format == VK_FORMAT_B8G8R8A8_SRGB;
    const uint8_t *pixels = scene->readback.mapped;
    uint32_t crc = 0;
    for (uint32_t y = 0; y < scene->extent.height; ++y) {
        const uint8_t *source = pixels + y * row;
        for (size_t x = 0; x < row; x += 4) {
            line[x] = source[x + (bgra ? 2 : 0)];
            line[x + 1] = source[x + 1];
            line[x + 2] = source[x + (bgra ? 0 : 2)];
            line[x + 3] = source[x + 3];
        }
        crc = bench_crc32(crc, line, row);
    }
    free(line);
    bench_log("crc frame %d%s: %08x", frame, cold ? " cold" : "", crc);
}

static VkResult draw(struct bench_vulkan *scene, int frame, int cold) {
    const uint32_t slot = (uint32_t)frame % FRAMES;
    VkResult result = vkWaitForFences(scene->device, 1, &scene->fences[slot], VK_TRUE, UINT64_MAX);
    if (result != VK_SUCCESS) return result;
    uint32_t image = 0;
    result = vkAcquireNextImageKHR(scene->device, scene->swapchain, UINT64_MAX, scene->acquired[slot], VK_NULL_HANDLE,
                                   &image);
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) return result;
    VkCommandBuffer commands = scene->commands[slot];
    if ((result = vkResetFences(scene->device, 1, &scene->fences[slot])) != VK_SUCCESS ||
        begin(commands) != 0) {
        return result != VK_SUCCESS ? result : VK_ERROR_UNKNOWN;
    }
    record(scene, commands, image, frame);
    const int check = frame == scene->check && scene->readback.buffer != VK_NULL_HANDLE;
    if (check) record_readback(scene, commands, image);
    const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    const VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1,
                                 .pWaitSemaphores = &scene->acquired[slot], .pWaitDstStageMask = &stage,
                                 .commandBufferCount = 1, .pCommandBuffers = &commands, .signalSemaphoreCount = 1,
                                 .pSignalSemaphores = &scene->rendered[image]};
    if ((result = vkEndCommandBuffer(commands)) != VK_SUCCESS ||
        (result = vkQueueSubmit(scene->queue, 1, &submit, scene->fences[slot])) != VK_SUCCESS) {
        return result;
    }
    const VkPresentInfoKHR present = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1,
                                      .pWaitSemaphores = &scene->rendered[image], .swapchainCount = 1,
                                      .pSwapchains = &scene->swapchain, .pImageIndices = &image};
    result = vkQueuePresentKHR(scene->queue, &present);
    if (check && vkWaitForFences(scene->device, 1, &scene->fences[slot], VK_TRUE, UINT64_MAX) == VK_SUCCESS) {
        log_crc(scene, frame, cold);
    }
    return result;
}

VkResult bench_vulkan_frame(struct bench_vulkan *scene, int frame) {
    // The check frame once before the first frame too, while the GPU is cold: its CRC must not depend on what the
    // GPU drew before (graphics.bench).
    if (scene->check >= 0 && !scene->cold) {
        scene->cold = 1;
        const VkResult result = draw(scene, scene->check, 1);
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) return result;
    }
    return draw(scene, frame, 0);
}

void bench_vulkan_stop(struct bench_vulkan *scene) {
    if (scene == NULL) return;
    if (scene->device != VK_NULL_HANDLE && vkDestroyDevice != NULL) {
        VkDevice device = scene->device;
        vkDeviceWaitIdle(device);
        const VkPipeline pipelines[5] = {scene->background, scene->fill, scene->mesh, scene->particles, scene->post};
        for (int index = 0; index < 5; ++index) vkDestroyPipeline(device, pipelines[index], NULL);
        vkDestroyPipelineLayout(device, scene->layout, NULL);
        vkDestroyDescriptorPool(device, scene->descriptors, NULL);
        vkDestroyDescriptorSetLayout(device, scene->set_layout, NULL);
        vkDestroySampler(device, scene->repeat, NULL);
        vkDestroySampler(device, scene->clamp, NULL);
        destroy_buffer(scene, &scene->vertices);
        destroy_buffer(scene, &scene->indices);
        destroy_buffer(scene, &scene->readback);
        destroy_image(scene, &scene->texture);
        destroy_image(scene, &scene->target);
        destroy_image(scene, &scene->depth);
        vkDestroyFramebuffer(device, scene->target_framebuffer, NULL);
        for (uint32_t index = 0; index < scene->image_count; ++index) {
            vkDestroyFramebuffer(device, scene->framebuffers[index], NULL);
            vkDestroyImageView(device, scene->views[index], NULL);
            vkDestroySemaphore(device, scene->rendered[index], NULL);
        }
        vkDestroyRenderPass(device, scene->scene_pass, NULL);
        vkDestroyRenderPass(device, scene->post_pass, NULL);
        vkDestroySwapchainKHR(device, scene->swapchain, NULL);
        for (int slot = 0; slot < FRAMES; ++slot) {
            vkDestroyFence(device, scene->fences[slot], NULL);
            vkDestroySemaphore(device, scene->acquired[slot], NULL);
        }
        vkDestroyCommandPool(device, scene->pool, NULL);
        vkDestroyDevice(device, NULL);
    }
    if (scene->surface != VK_NULL_HANDLE && vkDestroySurfaceKHR != NULL) {
        vkDestroySurfaceKHR(scene->instance, scene->surface, NULL);
    }
    if (scene->instance != VK_NULL_HANDLE && vkDestroyInstance != NULL) vkDestroyInstance(scene->instance, NULL);
    free(scene);
}
