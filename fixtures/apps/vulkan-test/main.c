// vulkan-test (ADR-0030, test 7; step V5): a NativeActivity on native_app_glue that draws a fixed sequence of
// twelve Vulkan scenes, each into a 64 x 64 image and into an R8G8B8A8_UNORM swapchain (Android's first format)
// of 256 x 256 with FIFO and 3 images, reads each back with vkCmdCopyImageToBuffer before the present, and logs
// with the tag "vulkan-test" what every conformant GPU gives alike: a CRC-32 and four pixels per image, an
// occlusion query read after its fence, a timeline semaphore's value, and whether its presents succeeded. The
// scenes are gles-test's where the two APIs meet, so their CRCs are gles-test's too; a compute shader on a
// storage buffer and a timeline semaphore take the places of four attachments and transform feedback, and the
// instances come from an indirect draw. They use only exact operations. After them it presents 120 more frames.
// Then it measures, in lines with the tag "vulkan-test-device" that are not compared (as the device's name,
// versions, extensions, features and surface), what ADR-0030 asks about the copy of R8G8B8A8 swapchains (F15):
// the GPU time of the same copy at the window's size (timestamp queries), 120 presents at the window's size
// through the copy (R8G8B8A8) and 120 directly (B8G8R8A8, where offered), and the time of a call through the
// front lib. Then it calls ANativeActivity_finish.
// The live variant (vulkan-live; ADR-0032, test 8; ADR-0036 a) is this library started at
// ANativeActivity_onCreateLive. It skips the scenes and draws a frame per AChoreographer callback while it has a
// window and the focus, a clear and a square that moves, until onDestroy. Each window gets a surface and an
// R8G8B8A8_UNORM swapchain at its own size, and TERM_WINDOW takes only those. Every 5 s it asks the window for 120
// and 60 frames a second in turn (ANativeWindow_setFrameRate, by dlsym above the minSdk) and logs the frame
// intervals of each rate. AKEYCODE_BACK finishes it. At onDestroy it logs its frames, windows and failures with
// the tag "vulkan-live".
#define VK_USE_PLATFORM_ANDROID_KHR
#include <android/choreographer.h>
#include <android/input.h>
#include <android/log.h>
#include <android/looper.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <dlfcn.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vulkan/vulkan.h>

#include "android_native_app_glue.h"

// The log's tags: the live variant has its own.
static const char *tag = "vulkan-test", *device_tag = "vulkan-test-device";

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, tag, __VA_ARGS__)
#define DEVICE(...) __android_log_print(ANDROID_LOG_INFO, device_tag, __VA_ARGS__)

enum { kSize = 64, kWindow = 256, kFrames = 120, kMaxImages = 8, kCopies = 60 };

static const uint32_t flat_vert[] =
#include "flat.vert.inc"
    ;
static const uint32_t flat_frag[] =
#include "flat.frag.inc"
    ;
static const uint32_t arithmetic_frag[] =
#include "arithmetic.frag.inc"
    ;
static const uint32_t texel_frag[] =
#include "texel.frag.inc"
    ;
static const uint32_t compressed_frag[] =
#include "compressed.frag.inc"
    ;
static const uint32_t instances_vert[] =
#include "instances.vert.inc"
    ;
static const uint32_t instances_frag[] =
#include "instances.frag.inc"
    ;
static const uint32_t compute_comp[] =
#include "compute.comp.inc"
    ;
static const uint32_t show_frag[] =
#include "show.frag.inc"
    ;
static const uint32_t bits_frag[] =
#include "bits.frag.inc"
    ;

static const char *const command_names[] = {
    "APP_CMD_INPUT_CHANGED", "APP_CMD_INIT_WINDOW", "APP_CMD_TERM_WINDOW", "APP_CMD_WINDOW_RESIZED",
    "APP_CMD_WINDOW_REDRAW_NEEDED", "APP_CMD_CONTENT_RECT_CHANGED", "APP_CMD_GAINED_FOCUS", "APP_CMD_LOST_FOCUS",
    "APP_CMD_CONFIG_CHANGED", "APP_CMD_LOW_MEMORY", "APP_CMD_START", "APP_CMD_RESUME", "APP_CMD_SAVE_STATE",
    "APP_CMD_PAUSE", "APP_CMD_STOP", "APP_CMD_DESTROY",
};

// The shaders' push constants (shaders/push.glsl).
struct push {
    float color[4];
    int32_t origin[2];
    float depth;
    int32_t shift;
    uint32_t bit;
};

struct image {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    VkImageLayout layout;  // as recorded so far
    VkImageAspectFlags aspect;
};

struct buffer {
    VkBuffer buffer;
    VkDeviceMemory memory;
    void *mapped;
};

struct swapchain {
    VkSwapchainKHR swapchain;
    VkFormat format;
    VkExtent2D extent;
    uint32_t count;
    struct image images[kMaxImages];
    VkFramebuffer framebuffers[kMaxImages];
    VkSemaphore rendered[kMaxImages];
    VkRenderPass render_pass;  // the measurement's own, else none
};

static struct {
    VkInstance instance;
    VkPhysicalDevice physical;
    VkDevice device;
    VkQueue queue;
    uint32_t family;
    VkSurfaceKHR surface;
    VkPhysicalDeviceMemoryProperties memory;
    VkPhysicalDeviceProperties properties;
    uint32_t timestamp_bits;
    VkFormat depth_format;
    bool etc2;
    PFN_vkWaitSemaphores wait_semaphores;
    PFN_vkSignalSemaphore signal_semaphore;
    PFN_vkGetSemaphoreCounterValue counter_value;

    VkCommandPool pool;
    VkCommandBuffer commands, first, second;
    VkFence fence;
    VkSemaphore acquired, timeline;
    VkRenderPass color_pass, load_pass, present_pass, integer_pass, integer_load_pass;
    VkDescriptorSetLayout set_layout, compute_set_layout;
    VkPipelineLayout layout, compute_layout;
    VkPipeline flat, depth, stencil_write, stencil_equal, replace, add, show, bits;
    VkPipeline arithmetic, texel, compressed, instances, compute;
    VkDescriptorPool descriptor_pool;
    VkDescriptorSet arithmetic_set, texel_set, etc2_set, copy_set, target_set, integers_set, compute_set;
    VkSampler sampler;
    VkQueryPool queries;

    struct image target, rects, integers, target_depth, colors, values, etc2_image, copy, window_depth;
    VkFramebuffer target_framebuffer, rects_framebuffer, integers_framebuffer;
    struct buffer vertices, square, offsets, indirect, uniforms, staging, storage, small_readback, readback;
    uint32_t vertex_count;

    struct swapchain chain;
    bool ready;
    int frames_left;
    int presents, presented;
} vk;

static int64_t now(void) {
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return (int64_t)time.tv_sec * 1000000000 + time.tv_nsec;
}

static uint32_t crc32(const uint8_t *data, size_t size) {
    uint32_t crc = 0xffffffffu;
    for (size_t index = 0; index < size; ++index) {
        crc ^= data[index];
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

// Bytes from a linear congruential generator, the same everywhere (gles-test's).
static void fill(uint8_t *bytes, size_t size, uint32_t seed) {
    for (size_t index = 0; index < size; ++index) {
        seed = seed * 1664525u + 1013904223u;
        bytes[index] = (uint8_t)(seed >> 24);
    }
}

static bool ok(VkResult result, const char *what) {
    if (result != VK_SUCCESS) LOG("%s failed: %d", what, result);
    return result == VK_SUCCESS;
}

// ---- Memory, buffers and images

static uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags flags) {
    for (uint32_t index = 0; index < vk.memory.memoryTypeCount; ++index) {
        if ((bits & (1u << index)) != 0 && (vk.memory.memoryTypes[index].propertyFlags & flags) == flags) return index;
    }
    return UINT32_MAX;
}

// A buffer in host-visible, coherent memory, mapped for good.
static bool make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, struct buffer *buffer) {
    const VkBufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage};
    if (!ok(vkCreateBuffer(vk.device, &info, NULL, &buffer->buffer), "vkCreateBuffer")) return false;
    VkMemoryRequirements needs;
    vkGetBufferMemoryRequirements(vk.device, buffer->buffer, &needs);
    const VkMemoryAllocateInfo allocate = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = needs.size,
        .memoryTypeIndex =
            memory_type(needs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    return ok(vkAllocateMemory(vk.device, &allocate, NULL, &buffer->memory), "vkAllocateMemory") &&
           ok(vkBindBufferMemory(vk.device, buffer->buffer, buffer->memory, 0), "vkBindBufferMemory") &&
           ok(vkMapMemory(vk.device, buffer->memory, 0, VK_WHOLE_SIZE, 0, &buffer->mapped), "vkMapMemory");
}

static bool make_view(struct image *image, VkFormat format) {
    const VkImageViewCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = image->image, .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format, .subresourceRange = {image->aspect, 0, 1, 0, 1}};
    return ok(vkCreateImageView(vk.device, &info, NULL, &image->view), "vkCreateImageView");
}

// An image of one level in device memory, with its view.
static bool make_image(VkFormat format, uint32_t width, uint32_t height, VkImageUsageFlags usage,
                       VkImageAspectFlags aspect, struct image *image) {
    const VkImageCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = format,
        .extent = {width, height, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    if (!ok(vkCreateImage(vk.device, &info, NULL, &image->image), "vkCreateImage")) return false;
    VkMemoryRequirements needs;
    vkGetImageMemoryRequirements(vk.device, image->image, &needs);
    const VkMemoryAllocateInfo allocate = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = needs.size,
        .memoryTypeIndex = memory_type(needs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    image->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    image->aspect = aspect;
    return ok(vkAllocateMemory(vk.device, &allocate, NULL, &image->memory), "vkAllocateMemory") &&
           ok(vkBindImageMemory(vk.device, image->image, image->memory, 0), "vkBindImageMemory") &&
           make_view(image, format);
}

static void destroy_image(struct image *image) {
    vkDestroyImageView(vk.device, image->view, NULL);
    vkDestroyImage(vk.device, image->image, NULL);
    vkFreeMemory(vk.device, image->memory, NULL);
    memset(image, 0, sizeof *image);
}

// A layout change that also orders everything before it against everything after it.
static void transition(VkCommandBuffer commands, struct image *image, VkImageLayout layout) {
    const VkImageMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, .oldLayout = image->layout,
        .newLayout = layout, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image->image, .subresourceRange = {image->aspect, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0,
                         NULL, 1, &barrier);
    image->layout = layout;
}

static void buffer_barrier(VkCommandBuffer commands) {
    const VkMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                                     .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
    vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &barrier,
                         0, NULL, 0, NULL);
}

static void copy_to_buffer(VkCommandBuffer commands, struct image *image, uint32_t width, uint32_t height,
                           const struct buffer *buffer) {
    transition(commands, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    const VkBufferImageCopy region = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                      .imageExtent = {width, height, 1}};
    vkCmdCopyImageToBuffer(commands, image->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer->buffer, 1, &region);
    buffer_barrier(commands);
}

// ---- Render passes and pipelines

// One color attachment, and one of depth and stencil with `depth`; every pass with the same formats is
// compatible with the others, so the pipelines serve them all.
static VkRenderPass render_pass(VkFormat color, VkAttachmentLoadOp load, VkImageLayout initial, VkImageLayout final,
                                bool depth) {
    const VkAttachmentDescription attachments[2] = {
        {.format = color, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = load, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
         .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
         .initialLayout = initial, .finalLayout = final},
        {.format = vk.depth_format, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
         .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
         .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
         .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL}};
    const VkAttachmentReference color_reference = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    const VkAttachmentReference depth_reference = {1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    const VkSubpassDescription subpass = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                          .colorAttachmentCount = 1, .pColorAttachments = &color_reference,
                                          .pDepthStencilAttachment = depth ? &depth_reference : NULL};
    const VkSubpassDependency dependencies[2] = {
        {.srcSubpass = VK_SUBPASS_EXTERNAL, .dstSubpass = 0, .srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
         .dstStageMask = VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
         .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT},
        {.srcSubpass = 0, .dstSubpass = VK_SUBPASS_EXTERNAL, .srcStageMask = VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
         .dstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
         .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT}};
    const VkRenderPassCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = depth ? 2 : 1,
        .pAttachments = attachments, .subpassCount = 1, .pSubpasses = &subpass, .dependencyCount = 2,
        .pDependencies = dependencies};
    VkRenderPass pass = VK_NULL_HANDLE;
    ok(vkCreateRenderPass(vk.device, &info, NULL, &pass), "vkCreateRenderPass");
    return pass;
}

static VkShaderModule shader(const uint32_t *code, size_t size) {
    const VkShaderModuleCreateInfo info = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = size,
                                           .pCode = code};
    VkShaderModule module = VK_NULL_HANDLE;
    ok(vkCreateShaderModule(vk.device, &info, NULL, &module), "vkCreateShaderModule");
    return module;
}

enum blend { kNoBlend, kReplace, kAdd };
enum tests { kNoTests, kDepthLess, kStencilWrite, kStencilEqual };

struct pipeline_kind {
    const uint32_t *vertex;
    size_t vertex_size;
    const uint32_t *fragment;
    size_t fragment_size;
    VkRenderPass render_pass;
    bool instanced;
    enum blend blend;
    enum tests tests;
};

static VkPipeline pipeline(const struct pipeline_kind *kind) {
    const VkShaderModule vertex = shader(kind->vertex, kind->vertex_size);
    const VkShaderModule fragment = shader(kind->fragment, kind->fragment_size);
    const VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vertex, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = fragment, .pName = "main"}};
    const VkVertexInputBindingDescription bindings[2] = {{0, 4, VK_VERTEX_INPUT_RATE_VERTEX},
                                                         {1, 4, VK_VERTEX_INPUT_RATE_INSTANCE}};
    const VkVertexInputAttributeDescription attributes[2] = {{0, 0, VK_FORMAT_R16G16_SINT, 0},
                                                             {1, 1, VK_FORMAT_R16G16_SINT, 0}};
    const VkPipelineVertexInputStateCreateInfo input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = kind->instanced ? 2 : 1, .pVertexBindingDescriptions = bindings,
        .vertexAttributeDescriptionCount = kind->instanced ? 2 : 1, .pVertexAttributeDescriptions = attributes};
    const VkPipelineInputAssemblyStateCreateInfo assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    const VkPipelineViewportStateCreateInfo viewport = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                                        .viewportCount = 1, .scissorCount = 1};
    const VkPipelineRasterizationStateCreateInfo raster = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1};
    const VkPipelineMultisampleStateCreateInfo multisample = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    const VkStencilOpState write = {VK_STENCIL_OP_KEEP, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_KEEP,
                                    VK_COMPARE_OP_ALWAYS, 0xff, 0xff, 1};
    const VkStencilOpState equal = {VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP,
                                    VK_COMPARE_OP_EQUAL, 0xff, 0, 1};
    VkPipelineDepthStencilStateCreateInfo tests = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
                                                   .depthCompareOp = VK_COMPARE_OP_LESS};
    if (kind->tests == kDepthLess) tests.depthTestEnable = tests.depthWriteEnable = VK_TRUE;
    if (kind->tests == kStencilWrite || kind->tests == kStencilEqual) {
        tests.stencilTestEnable = VK_TRUE;
        tests.front = tests.back = kind->tests == kStencilWrite ? write : equal;
    }
    VkPipelineColorBlendAttachmentState blend = {
        .colorWriteMask = kind->tests == kStencilWrite ? 0 : VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                                                 VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT};
    if (kind->blend != kNoBlend) {
        const VkBlendFactor destination = kind->blend == kAdd ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ZERO;
        blend.blendEnable = VK_TRUE;
        blend.srcColorBlendFactor = blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.dstColorBlendFactor = blend.dstAlphaBlendFactor = destination;
        blend.colorBlendOp = blend.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    const VkPipelineColorBlendStateCreateInfo colors = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &blend};
    const VkDynamicState states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    const VkPipelineDynamicStateCreateInfo dynamic = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                                      .dynamicStateCount = 2, .pDynamicStates = states};
    const VkGraphicsPipelineCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = stages,
        .pVertexInputState = &input, .pInputAssemblyState = &assembly, .pViewportState = &viewport,
        .pRasterizationState = &raster, .pMultisampleState = &multisample, .pDepthStencilState = &tests,
        .pColorBlendState = &colors, .pDynamicState = &dynamic, .layout = vk.layout, .renderPass = kind->render_pass};
    VkPipeline pipeline = VK_NULL_HANDLE;
    ok(vkCreateGraphicsPipelines(vk.device, VK_NULL_HANDLE, 1, &info, NULL, &pipeline), "vkCreateGraphicsPipelines");
    vkDestroyShaderModule(vk.device, vertex, NULL);
    vkDestroyShaderModule(vk.device, fragment, NULL);
    return pipeline;
}

#define SHADER(name) name, sizeof name

static bool make_pipelines(void) {
    const VkDescriptorSetLayoutBinding bindings[3] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL}};
    const VkDescriptorSetLayoutCreateInfo set_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                      .bindingCount = 3, .pBindings = bindings};
    const VkDescriptorSetLayoutBinding storage = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT,
                                                  NULL};
    const VkDescriptorSetLayoutCreateInfo compute_set_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &storage};
    if (!ok(vkCreateDescriptorSetLayout(vk.device, &set_info, NULL, &vk.set_layout), "vkCreateDescriptorSetLayout") ||
        !ok(vkCreateDescriptorSetLayout(vk.device, &compute_set_info, NULL, &vk.compute_set_layout),
            "vkCreateDescriptorSetLayout")) {
        return false;
    }
    const VkPushConstantRange range = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(struct push)};
    const VkPipelineLayoutCreateInfo layout_info = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                                    .setLayoutCount = 1, .pSetLayouts = &vk.set_layout,
                                                    .pushConstantRangeCount = 1, .pPushConstantRanges = &range};
    const VkPipelineLayoutCreateInfo compute_layout_info = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                                            .setLayoutCount = 1, .pSetLayouts = &vk.compute_set_layout};
    if (!ok(vkCreatePipelineLayout(vk.device, &layout_info, NULL, &vk.layout), "vkCreatePipelineLayout") ||
        !ok(vkCreatePipelineLayout(vk.device, &compute_layout_info, NULL, &vk.compute_layout), "vkCreatePipelineLayout")) {
        return false;
    }

    const VkFormat unorm = VK_FORMAT_R8G8B8A8_UNORM, uint = VK_FORMAT_R8G8B8A8_UINT;
    vk.color_pass = render_pass(unorm, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_IMAGE_LAYOUT_UNDEFINED,
                                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, true);
    vk.load_pass = render_pass(unorm, VK_ATTACHMENT_LOAD_OP_LOAD, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, true);
    vk.present_pass = render_pass(unorm, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_IMAGE_LAYOUT_UNDEFINED,
                                  VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, true);
    vk.integer_pass = render_pass(uint, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_IMAGE_LAYOUT_UNDEFINED,
                                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, false);
    vk.integer_load_pass = render_pass(uint, VK_ATTACHMENT_LOAD_OP_LOAD, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, false);

    const struct pipeline_kind kinds[] = {
        {SHADER(flat_vert), SHADER(flat_frag), vk.color_pass, false, kNoBlend, kNoTests},
        {SHADER(flat_vert), SHADER(flat_frag), vk.color_pass, false, kNoBlend, kDepthLess},
        {SHADER(flat_vert), SHADER(flat_frag), vk.color_pass, false, kNoBlend, kStencilWrite},
        {SHADER(flat_vert), SHADER(flat_frag), vk.color_pass, false, kNoBlend, kStencilEqual},
        {SHADER(flat_vert), SHADER(flat_frag), vk.color_pass, false, kReplace, kNoTests},
        {SHADER(flat_vert), SHADER(flat_frag), vk.color_pass, false, kAdd, kNoTests},
        {SHADER(flat_vert), SHADER(show_frag), vk.color_pass, false, kNoBlend, kNoTests},
        {SHADER(flat_vert), SHADER(bits_frag), vk.color_pass, false, kNoBlend, kNoTests},
        {SHADER(flat_vert), SHADER(arithmetic_frag), vk.integer_pass, false, kNoBlend, kNoTests},
        {SHADER(flat_vert), SHADER(texel_frag), vk.integer_pass, false, kNoBlend, kNoTests},
        {SHADER(flat_vert), SHADER(compressed_frag), vk.integer_pass, false, kNoBlend, kNoTests},
        {SHADER(instances_vert), SHADER(instances_frag), vk.integer_pass, true, kNoBlend, kNoTests},
    };
    VkPipeline *const targets[] = {&vk.flat, &vk.depth, &vk.stencil_write, &vk.stencil_equal, &vk.replace, &vk.add,
                                   &vk.show, &vk.bits, &vk.arithmetic, &vk.texel, &vk.compressed, &vk.instances};
    for (size_t index = 0; index < sizeof kinds / sizeof kinds[0]; ++index) *targets[index] = pipeline(&kinds[index]);

    const VkShaderModule compute = shader(SHADER(compute_comp));
    const VkComputePipelineCreateInfo compute_info = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                  .module = compute, .pName = "main"},
        .layout = vk.compute_layout};
    const bool made = ok(vkCreateComputePipelines(vk.device, VK_NULL_HANDLE, 1, &compute_info, NULL, &vk.compute),
                         "vkCreateComputePipelines");
    vkDestroyShaderModule(vk.device, compute, NULL);
    return made;
}

// ---- Objects of the scenes

static void write_image(VkDescriptorSet set, uint32_t binding, const struct image *image) {
    const VkDescriptorImageInfo info = {vk.sampler, image->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
                                        .dstBinding = binding, .descriptorCount = 1,
                                        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &info};
    vkUpdateDescriptorSets(vk.device, 1, &write, 0, NULL);
}

static void write_buffer(VkDescriptorSet set, uint32_t binding, VkDescriptorType type, const struct buffer *buffer) {
    const VkDescriptorBufferInfo info = {buffer->buffer, 0, VK_WHOLE_SIZE};
    const VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
                                        .dstBinding = binding, .descriptorCount = 1, .descriptorType = type,
                                        .pBufferInfo = &info};
    vkUpdateDescriptorSets(vk.device, 1, &write, 0, NULL);
}

static VkFramebuffer framebuffer(VkRenderPass pass, const struct image *color, const struct image *depth,
                                 uint32_t width, uint32_t height) {
    const VkImageView views[2] = {color->view, depth != NULL ? depth->view : VK_NULL_HANDLE};
    const VkFramebufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = pass,
                                          .attachmentCount = depth != NULL ? 2 : 1, .pAttachments = views,
                                          .width = width, .height = height, .layers = 1};
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    ok(vkCreateFramebuffer(vk.device, &info, NULL, &framebuffer), "vkCreateFramebuffer");
    return framebuffer;
}

static bool begin(VkCommandBuffer commands) {
    const VkCommandBufferBeginInfo info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                           .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    return ok(vkResetCommandBuffer(commands, 0), "vkResetCommandBuffer") &&
           ok(vkBeginCommandBuffer(commands, &info), "vkBeginCommandBuffer");
}

// Submits `commands` with the fence, waiting for `wait` and signaling `signal` where given, and waits for it.
static bool submit_and_wait(VkCommandBuffer commands, VkSemaphore wait, VkSemaphore signal) {
    const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    const VkSubmitInfo info = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = wait != VK_NULL_HANDLE,
                               .pWaitSemaphores = &wait, .pWaitDstStageMask = &stage, .commandBufferCount = 1,
                               .pCommandBuffers = &commands, .signalSemaphoreCount = signal != VK_NULL_HANDLE,
                               .pSignalSemaphores = &signal};
    return ok(vkEndCommandBuffer(commands), "vkEndCommandBuffer") &&
           ok(vkQueueSubmit(vk.queue, 1, &info, vk.fence), "vkQueueSubmit") &&
           ok(vkWaitForFences(vk.device, 1, &vk.fence, VK_TRUE, UINT64_MAX), "vkWaitForFences") &&
           ok(vkResetFences(vk.device, 1, &vk.fence), "vkResetFences");
}

static bool make_objects(void) {
    const VkImageUsageFlags attachment = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                         VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    const VkImageUsageFlags upload = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    const VkImageAspectFlags color = VK_IMAGE_ASPECT_COLOR_BIT;
    const VkImageAspectFlags depth = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    if (!make_image(VK_FORMAT_R8G8B8A8_UNORM, kSize, kSize, attachment, color, &vk.target) ||
        !make_image(VK_FORMAT_R8G8B8A8_UNORM, kSize, kSize, attachment, color, &vk.rects) ||
        !make_image(VK_FORMAT_R8G8B8A8_UINT, kSize, kSize, attachment, color, &vk.integers) ||
        !make_image(vk.depth_format, kSize, kSize, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, depth, &vk.target_depth) ||
        !make_image(VK_FORMAT_R8G8B8A8_UNORM, 16, 16, upload, color, &vk.colors) ||
        !make_image(VK_FORMAT_R8G8B8A8_UINT, 16, 16, upload, color, &vk.values) ||
        !make_image(VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, 16, 16, upload, color, &vk.etc2_image) ||
        !make_image(VK_FORMAT_R8G8B8A8_UNORM, 32, 32, upload, color, &vk.copy) ||
        !make_image(vk.depth_format, kWindow, kWindow, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, depth,
                    &vk.window_depth)) {
        return false;
    }
    vk.target_framebuffer = framebuffer(vk.color_pass, &vk.target, &vk.target_depth, kSize, kSize);
    vk.rects_framebuffer = framebuffer(vk.color_pass, &vk.rects, &vk.target_depth, kSize, kSize);
    vk.integers_framebuffer = framebuffer(vk.integer_pass, &vk.integers, NULL, kSize, kSize);

    // Buffers: rectangles written while recording, the instances' square and offsets, the indirect draw, the
    // uniform block of scene 5, the uploads, the compute shader's storage, and the two readbacks.
    static const int16_t square[12] = {0, 0, 8, 0, 8, 8, 0, 0, 8, 8, 0, 8};
    int16_t offsets[32];
    for (int instance = 0; instance < 16; ++instance) {
        offsets[2 * instance] = (int16_t)((instance & 3) * 16 + 4);
        offsets[2 * instance + 1] = (int16_t)((instance >> 2) * 16 + 4);
    }
    static const VkDrawIndirectCommand indirect = {6, 16, 0, 0};
    static const uint32_t values[8] = {0x9e3779b9u, 0x7f4a7c15u, 0, 0, 0x85ebca6bu, 0xc2b2ae35u, 0x27d4eb2fu, 0};
    if (!make_buffer(65536, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &vk.vertices) ||
        !make_buffer(sizeof square, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &vk.square) ||
        !make_buffer(sizeof offsets, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &vk.offsets) ||
        !make_buffer(sizeof indirect, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, &vk.indirect) ||
        !make_buffer(sizeof values, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &vk.uniforms) ||
        !make_buffer(4096, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &vk.staging) ||
        !make_buffer(kSize * kSize * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     &vk.storage) ||
        !make_buffer(kSize * kSize * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &vk.small_readback) ||
        !make_buffer(kWindow * kWindow * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &vk.readback)) {
        return false;
    }
    memcpy(vk.square.mapped, square, sizeof square);
    memcpy(vk.offsets.mapped, offsets, sizeof offsets);
    memcpy(vk.indirect.mapped, &indirect, sizeof indirect);
    memcpy(vk.uniforms.mapped, values, sizeof values);

    // The textures of 16 x 16 from the generator, as gles-test's: RGBA8, RGBA8UI and ETC2 RGB8.
    uint8_t *staging = vk.staging.mapped;
    fill(staging, 1024, 1);
    fill(staging + 1024, 1024, 2);
    fill(staging + 2048, 128, 3);
    if (!begin(vk.commands)) return false;
    struct image *const uploads[3] = {&vk.colors, &vk.values, &vk.etc2_image};
    for (int index = 0; index < 3; ++index) {
        transition(vk.commands, uploads[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        const VkBufferImageCopy region = {.bufferOffset = 1024 * (VkDeviceSize)index,
                                          .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                          .imageExtent = {16, 16, 1}};
        vkCmdCopyBufferToImage(vk.commands, vk.staging.buffer, uploads[index]->image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        transition(vk.commands, uploads[index], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    if (!submit_and_wait(vk.commands, VK_NULL_HANDLE, VK_NULL_HANDLE)) return false;

    const VkSamplerCreateInfo sampler = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    const VkDescriptorPoolSize sizes[3] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16},
                                           {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2},
                                           {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2}};
    const VkDescriptorPoolCreateInfo pool = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 8,
                                             .poolSizeCount = 3, .pPoolSizes = sizes};
    if (!ok(vkCreateSampler(vk.device, &sampler, NULL, &vk.sampler), "vkCreateSampler") ||
        !ok(vkCreateDescriptorPool(vk.device, &pool, NULL, &vk.descriptor_pool), "vkCreateDescriptorPool")) {
        return false;
    }
    VkDescriptorSet *const sets[] = {&vk.arithmetic_set, &vk.texel_set, &vk.etc2_set, &vk.copy_set, &vk.target_set,
                                     &vk.integers_set, &vk.compute_set};
    for (size_t index = 0; index < sizeof sets / sizeof sets[0]; ++index) {
        const VkDescriptorSetLayout layout = index == 6 ? vk.compute_set_layout : vk.set_layout;
        const VkDescriptorSetAllocateInfo allocate = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                                      .descriptorPool = vk.descriptor_pool, .descriptorSetCount = 1,
                                                      .pSetLayouts = &layout};
        if (!ok(vkAllocateDescriptorSets(vk.device, &allocate, sets[index]), "vkAllocateDescriptorSets")) return false;
    }
    write_buffer(vk.arithmetic_set, 2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &vk.uniforms);
    write_image(vk.texel_set, 0, &vk.colors);
    write_image(vk.texel_set, 1, &vk.values);
    write_image(vk.etc2_set, 0, &vk.etc2_image);
    write_image(vk.copy_set, 0, &vk.copy);
    write_image(vk.target_set, 0, &vk.target);
    write_image(vk.integers_set, 0, &vk.integers);
    write_buffer(vk.compute_set, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &vk.storage);

    const VkQueryPoolCreateInfo queries = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                           .queryType = VK_QUERY_TYPE_OCCLUSION, .queryCount = 2};
    const VkSemaphoreTypeCreateInfo timeline_type = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                                     .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE};
    const VkSemaphoreCreateInfo timeline = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &timeline_type};
    return ok(vkCreateQueryPool(vk.device, &queries, NULL, &vk.queries), "vkCreateQueryPool") &&
           ok(vkCreateSemaphore(vk.device, &timeline, NULL, &vk.timeline), "vkCreateSemaphore");
}

// ---- Drawing

static void pass(VkCommandBuffer commands, VkRenderPass render_pass, VkFramebuffer framebuffer, uint32_t size,
                 const float clear[4]) {
    VkClearValue values[2];
    memset(values, 0, sizeof values);
    memcpy(values[0].color.float32, clear, 4 * sizeof(float));
    values[1].depthStencil.depth = 1;
    values[1].depthStencil.stencil = 0;
    const VkRenderPassBeginInfo info = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = render_pass,
                                        .framebuffer = framebuffer, .renderArea = {{0, 0}, {size, size}},
                                        .clearValueCount = 2, .pClearValues = values};
    vkCmdBeginRenderPass(commands, &info, VK_SUBPASS_CONTENTS_INLINE);
    const VkViewport viewport = {0, 0, (float)size, (float)size, 0, 1};
    const VkRect2D scissor = {{0, 0}, {size, size}};
    vkCmdSetViewport(commands, 0, 1, &viewport);
    vkCmdSetScissor(commands, 0, 1, &scissor);
}

static void use(VkCommandBuffer commands, VkPipeline pipeline, VkDescriptorSet set, const struct push *push) {
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    if (set != VK_NULL_HANDLE) {
        vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.layout, 0, 1, &set, 0, NULL);
    }
    const struct push none = {{0, 0, 0, 0}, {0, 0}, 0, 0, 0};
    vkCmdPushConstants(commands, vk.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(struct push), push != NULL ? push : &none);
}

static void flat(VkCommandBuffer commands, VkPipeline pipeline, float red, float green, float blue, float alpha,
                 float depth) {
    const struct push push = {{red, green, blue, alpha}, {0, 0}, depth, 0, 0};
    use(commands, pipeline, VK_NULL_HANDLE, &push);
}

// A rectangle of the 64 grid, its six corners written into the vertex buffer while recording.
static void rect(VkCommandBuffer commands, int x0, int y0, int x1, int y1) {
    const int16_t corners[12] = {(int16_t)x0, (int16_t)y0, (int16_t)x1, (int16_t)y0, (int16_t)x1, (int16_t)y1,
                                 (int16_t)x0, (int16_t)y0, (int16_t)x1, (int16_t)y1, (int16_t)x0, (int16_t)y1};
    memcpy((int16_t *)vk.vertices.mapped + 2 * vk.vertex_count, corners, sizeof corners);
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(commands, 0, 1, &vk.vertices.buffer, &offset);
    vkCmdDraw(commands, 6, 1, vk.vertex_count, 0);
    vk.vertex_count += 6;
}

// Scene 1 inside a pass of `size` pixels that cleared to blue: two rectangles cleared, as gles-test scissors.
static void draw_clears(VkCommandBuffer commands, int size) {
    const int scale = size / kSize;
    const VkClearAttachment red = {VK_IMAGE_ASPECT_COLOR_BIT, 0, {.color = {.float32 = {1, 0, 0, 1}}}};
    const VkClearAttachment green = {VK_IMAGE_ASPECT_COLOR_BIT, 0, {.color = {.float32 = {0, 1, 0, 0}}}};
    const VkClearRect first = {{{8 * scale, 16 * scale}, {32u * scale, 40u * scale}}, 0, 1};
    const VkClearRect second = {{{32 * scale, 4 * scale}, {28u * scale, 24u * scale}}, 0, 1};
    vkCmdClearAttachments(commands, 1, &red, 1, &first);
    vkCmdClearAttachments(commands, 1, &green, 1, &second);
}

static void draw_rectangles(VkCommandBuffer commands) {
    flat(commands, vk.flat, 1, 0, 0, 1, 0);
    rect(commands, 4, 4, 36, 28);
    flat(commands, vk.flat, 0, 1, 0, 1, 0);
    rect(commands, 20, 20, 60, 60);
    flat(commands, vk.flat, 1, 1, 1, 1, 0);
    rect(commands, 8, 40, 28, 60);
    flat(commands, vk.flat, 1, 0, 1, 0, 0);
    rect(commands, 40, 4, 60, 16);
}

// Window depths 0.25, 0.5 and 0.75 differ in every depth format; the stencil holds 0 or 1.
static void draw_depth_and_stencil(VkCommandBuffer commands) {
    flat(commands, vk.depth, 1, 0, 0, 1, 0.5f);
    rect(commands, 4, 4, 40, 40);
    flat(commands, vk.depth, 0, 1, 0, 1, 0.75f);
    rect(commands, 20, 20, 60, 60);
    flat(commands, vk.depth, 0, 0, 1, 1, 0.25f);
    rect(commands, 30, 8, 50, 30);
    flat(commands, vk.stencil_write, 0, 0, 0, 0, 0);
    rect(commands, 8, 44, 56, 52);
    flat(commands, vk.stencil_equal, 1, 1, 1, 1, 0);
    rect(commands, 0, 0, 64, 64);
}

static void draw_blending(VkCommandBuffer commands) {
    flat(commands, vk.replace, 1, 0, 0, 1, 0);
    rect(commands, 4, 4, 44, 44);
    flat(commands, vk.add, 0, 1, 0, 1, 0);
    rect(commands, 20, 20, 60, 60);
    flat(commands, vk.add, 0, 0, 1, 0, 0);
    rect(commands, 12, 36, 52, 56);
}

// Scene 12: with `queries`, the occluded and the visible square each inside an occlusion query.
static void draw_occlusion(VkCommandBuffer commands, bool queries) {
    flat(commands, vk.depth, 1, 0, 0, 1, 0.25f);
    rect(commands, 8, 8, 56, 56);
    if (queries) vkCmdBeginQuery(commands, vk.queries, 0, 0);
    flat(commands, vk.depth, 0, 1, 0, 1, 0.75f);
    rect(commands, 16, 16, 48, 48);
    if (queries) vkCmdEndQuery(commands, vk.queries, 0);
    if (queries) vkCmdBeginQuery(commands, vk.queries, 1, 0);
    flat(commands, vk.depth, 0, 0, 1, 1, 0.125f);
    rect(commands, 40, 40, 62, 62);
    if (queries) vkCmdEndQuery(commands, vk.queries, 1);
}

static void draw_instances(VkCommandBuffer commands) {
    use(commands, vk.instances, VK_NULL_HANDLE, NULL);
    const VkBuffer buffers[2] = {vk.square.buffer, vk.offsets.buffer};
    const VkDeviceSize offsets[2] = {0, 0};
    vkCmdBindVertexBuffers(commands, 0, 2, buffers, offsets);
    vkCmdDrawIndirect(commands, vk.indirect.buffer, 0, 1, sizeof(VkDrawIndirectCommand));
}

// Bit `bit` of the RGBA8UI image into the pass, four times as large.
static void show_bits(VkCommandBuffer commands, unsigned bit) {
    const struct push push = {{0, 0, 0, 0}, {0, 0}, 0, 0, bit};
    use(commands, vk.bits, vk.integers_set, &push);
    rect(commands, 0, 0, kSize, kSize);
}

static const float black[4] = {0, 0, 0, 1}, blue[4] = {0, 0, 1, 1}, clear_black[4] = {0, 0, 0, 0};

static void integer_pass(VkCommandBuffer commands) {
    pass(commands, vk.integer_pass, vk.integers_framebuffer, kSize, clear_black);
}

// Records scene `scene` into its 64 x 64 image, which it returns.
static struct image *record_target(VkCommandBuffer commands, int scene) {
    switch (scene) {
        case 1:
            pass(commands, vk.color_pass, vk.target_framebuffer, kSize, blue);
            draw_clears(commands, kSize);
            break;
        case 2:
            pass(commands, vk.color_pass, vk.rects_framebuffer, kSize, black);
            draw_rectangles(commands);
            vkCmdEndRenderPass(commands);
            vk.rects.layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            return &vk.rects;
        case 3:
            pass(commands, vk.color_pass, vk.target_framebuffer, kSize, black);
            draw_depth_and_stencil(commands);
            break;
        case 4:
            pass(commands, vk.color_pass, vk.target_framebuffer, kSize, clear_black);
            draw_blending(commands);
            break;
        case 5:
            integer_pass(commands);
            use(commands, vk.arithmetic, vk.arithmetic_set, NULL);
            rect(commands, 0, 0, kSize, kSize);
            vkCmdEndRenderPass(commands);
            vk.integers.layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            return &vk.integers;
        case 6:
            integer_pass(commands);
            use(commands, vk.texel, vk.texel_set, NULL);
            rect(commands, 0, 0, kSize, kSize);
            vkCmdEndRenderPass(commands);
            vk.integers.layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            return &vk.integers;
        case 7:
            integer_pass(commands);
            use(commands, vk.compressed, vk.etc2_set, NULL);
            rect(commands, 0, 0, kSize, kSize);
            vkCmdEndRenderPass(commands);
            vk.integers.layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            return &vk.integers;
        case 8: {
            // The compute shader's buffer, one packed value per pixel, copied into the RGBA8UI image.
            vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, vk.compute);
            vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, vk.compute_layout, 0, 1, &vk.compute_set,
                                    0, NULL);
            vkCmdDispatch(commands, kSize / 8, kSize / 8, 1);
            buffer_barrier(commands);
            transition(commands, &vk.integers, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            const VkBufferImageCopy region = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                              .imageExtent = {kSize, kSize, 1}};
            vkCmdCopyBufferToImage(commands, vk.storage.buffer, vk.integers.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   1, &region);
            return &vk.integers;
        }
        case 9:
            integer_pass(commands);
            draw_instances(commands);
            vkCmdEndRenderPass(commands);
            vk.integers.layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            return &vk.integers;
        case 10:
            // Drawn by the two submits of timeline(); nothing more here.
            return &vk.integers;
        case 11: {
            // A quarter of scene 2's picture blitted 1:1, another copied into an image and drawn back.
            transition(commands, &vk.target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            const VkClearColorValue black_value = {.float32 = {0, 0, 0, 1}};
            const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdClearColorImage(commands, vk.target.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black_value, 1,
                                 &range);
            transition(commands, &vk.rects, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            transition(commands, &vk.target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            const VkImageBlit blit = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                      .srcOffsets = {{0, 0, 0}, {32, 32, 1}},
                                      .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                      .dstOffsets = {{32, 32, 0}, {64, 64, 1}}};
            vkCmdBlitImage(commands, vk.rects.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, vk.target.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
            transition(commands, &vk.copy, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            const VkImageCopy copy = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                      .srcOffset = {16, 16, 0},
                                      .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                      .extent = {32, 32, 1}};
            vkCmdCopyImage(commands, vk.rects.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, vk.copy.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            transition(commands, &vk.copy, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            transition(commands, &vk.target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            pass(commands, vk.load_pass, vk.target_framebuffer, kSize, black);
            const struct push push = {{0, 0, 0, 0}, {0, 32}, 0, 0, 0};
            use(commands, vk.show, vk.copy_set, &push);
            rect(commands, 0, 32, 32, 64);
            break;
        }
        default:
            vkCmdResetQueryPool(commands, vk.queries, 0, 2);
            pass(commands, vk.color_pass, vk.target_framebuffer, kSize, black);
            draw_occlusion(commands, true);
            break;
    }
    vkCmdEndRenderPass(commands);
    vk.target.layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    return &vk.target;
}

// Records scene `scene` into the swapchain's image `index`, which it leaves in TRANSFER_SRC_OPTIMAL.
static void record_window(VkCommandBuffer commands, int scene, uint32_t index) {
    struct image *image = &vk.chain.images[index];
    const VkFramebuffer framebuffer = vk.chain.framebuffers[index];
    pass(commands, vk.color_pass, framebuffer, kWindow,
         scene == 1 ? blue : scene == 4 ? clear_black : black);
    switch (scene) {
        case 1:
            draw_clears(commands, kWindow);
            break;
        case 2:
            draw_rectangles(commands);
            break;
        case 3:
            draw_depth_and_stencil(commands);
            break;
        case 4:
            draw_blending(commands);
            break;
        case 5:
        case 6:
        case 7:
            show_bits(commands, 7);
            break;
        case 8:
        case 10:
            show_bits(commands, 0);
            break;
        case 9:
            show_bits(commands, 4);
            break;
        case 11: {
            const struct push push = {{0, 0, 0, 0}, {0, 0}, 0, 2, 0};
            use(commands, vk.show, vk.target_set, &push);
            rect(commands, 0, 0, kSize, kSize);
            break;
        }
        default:
            draw_occlusion(commands, false);
            break;
    }
    vkCmdEndRenderPass(commands);
    image->layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    if (scene == 11) {
        // Scene 2's picture blitted 1:1 into the corner, as gles-test's glBlitFramebuffer.
        transition(commands, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        const VkImageBlit blit = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                  .srcOffsets = {{0, 0, 0}, {kSize, kSize, 1}},
                                  .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                  .dstOffsets = {{0, 0, 0}, {kSize, kSize, 1}}};
        vkCmdBlitImage(commands, vk.rects.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image->image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
    }
}

// Scene 10: the integer image cleared by a first submit that waits for the host's value 1 on a timeline
// semaphore and signals 2, the instances drawn over it by a second that waits for 2 and signals 3. Were the
// order broken, the clear would hide the instances.
static void timeline(void) {
    if (!begin(vk.first)) return;
    transition(vk.first, &vk.integers, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const VkClearColorValue seven = {.uint32 = {7, 7, 7, 7}};
    const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(vk.first, vk.integers.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &seven, 1, &range);
    if (!ok(vkEndCommandBuffer(vk.first), "vkEndCommandBuffer") || !begin(vk.second)) return;
    transition(vk.second, &vk.integers, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    pass(vk.second, vk.integer_load_pass, vk.integers_framebuffer, kSize, clear_black);
    draw_instances(vk.second);
    vkCmdEndRenderPass(vk.second);
    vk.integers.layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    if (!ok(vkEndCommandBuffer(vk.second), "vkEndCommandBuffer")) return;

    const uint64_t waits[2] = {1, 2}, signals[2] = {2, 3};
    const VkTimelineSemaphoreSubmitInfo values[2] = {
        {.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, .waitSemaphoreValueCount = 1,
         .pWaitSemaphoreValues = &waits[0], .signalSemaphoreValueCount = 1, .pSignalSemaphoreValues = &signals[0]},
        {.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, .waitSemaphoreValueCount = 1,
         .pWaitSemaphoreValues = &waits[1], .signalSemaphoreValueCount = 1, .pSignalSemaphoreValues = &signals[1]}};
    const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    const VkSubmitInfo submits[2] = {
        {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &values[0], .waitSemaphoreCount = 1,
         .pWaitSemaphores = &vk.timeline, .pWaitDstStageMask = &stage, .commandBufferCount = 1,
         .pCommandBuffers = &vk.first, .signalSemaphoreCount = 1, .pSignalSemaphores = &vk.timeline},
        {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &values[1], .waitSemaphoreCount = 1,
         .pWaitSemaphores = &vk.timeline, .pWaitDstStageMask = &stage, .commandBufferCount = 1,
         .pCommandBuffers = &vk.second, .signalSemaphoreCount = 1, .pSignalSemaphores = &vk.timeline}};
    if (!ok(vkQueueSubmit(vk.queue, 1, &submits[0], VK_NULL_HANDLE), "vkQueueSubmit") ||
        !ok(vkQueueSubmit(vk.queue, 1, &submits[1], VK_NULL_HANDLE), "vkQueueSubmit")) {
        return;
    }
    const VkSemaphoreSignalInfo signal = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, .semaphore = vk.timeline,
                                          .value = 1};
    const uint64_t goal = 3;
    const VkSemaphoreWaitInfo wait = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1,
                                      .pSemaphores = &vk.timeline, .pValues = &goal};
    uint64_t reached = 0;
    const VkResult signaled = vk.signal_semaphore(vk.device, &signal);
    const VkResult waited = vk.wait_semaphores(vk.device, &wait, 5000000000u);
    const VkResult counted = vk.counter_value(vk.device, vk.timeline, &reached);
    LOG("scene 10 timeline semaphore: the host signaled 1 (%d), the wait for 3 gave %d, the value is %llu (%d)",
        signaled, waited, (unsigned long long)reached, counted);
}

// ---- Reading back and logging (gles-test's lines)

static const int places[4][2] = {{1, 1}, {62, 5}, {21, 42}, {48, 30}};

static void report(int scene, const char *name, const char *where, const uint8_t *image, int size) {
    const int scale = size / kSize;
    char text[4][24];
    for (int index = 0; index < 4; ++index) {
        const uint8_t *pixel = image + ((size_t)(places[index][1] * scale + scale / 2) * size +
                                        (size_t)(places[index][0] * scale + scale / 2)) * 4;
        snprintf(text[index], sizeof text[index], "%s(%u,%u,%u,%u)", index == 0 ? "" : " ", pixel[0], pixel[1],
                 pixel[2], pixel[3]);
    }
    LOG("scene %d %s: %s crc %08x, %s%s%s%s", scene, name, where, crc32(image, (size_t)size * size * 4), text[0],
        text[1], text[2], text[3]);
}

static const char *const scene_names[] = {
    "", "clears and scissor", "rectangles", "depth and stencil", "blending", "integer arithmetic",
    "texelFetch", "ETC2", "compute", "instancing", "timeline semaphore", "blit and copy", "query and fence",
};

static bool acquire(uint32_t *index) {
    const VkResult result = vkAcquireNextImageKHR(vk.device, vk.chain.swapchain, UINT64_MAX, vk.acquired,
                                                  VK_NULL_HANDLE, index);
    if (result != VK_SUCCESS) DEVICE("vkAcquireNextImageKHR: %d", result);
    return result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR;
}

static void present(uint32_t index) {
    const VkPresentInfoKHR info = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1,
                                   .pWaitSemaphores = &vk.chain.rendered[index], .swapchainCount = 1,
                                   .pSwapchains = &vk.chain.swapchain, .pImageIndices = &index};
    const VkResult result = vkQueuePresentKHR(vk.queue, &info);
    ++vk.presents;
    if (result == VK_SUCCESS) {
        ++vk.presented;
    } else {
        DEVICE("present %d: %d", vk.presents, result);
    }
}

static void scene(int number) {
    const char *name = scene_names[number];
    uint32_t index = 0;
    vk.vertex_count = 0;
    if (!acquire(&index)) return;
    if (number == 10) timeline();
    if (!begin(vk.commands)) return;
    struct image *target = record_target(vk.commands, number);
    copy_to_buffer(vk.commands, target, kSize, kSize, &vk.small_readback);
    // The images the window's part reads.
    if (target == &vk.integers) transition(vk.commands, &vk.integers, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (number == 11) transition(vk.commands, &vk.target, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    record_window(vk.commands, number, index);
    copy_to_buffer(vk.commands, &vk.chain.images[index], kWindow, kWindow, &vk.readback);
    transition(vk.commands, &vk.chain.images[index], VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    if (!submit_and_wait(vk.commands, vk.acquired, vk.chain.rendered[index])) return;
    if (number == 12) {
        uint64_t samples[2] = {UINT64_MAX, UINT64_MAX};
        const VkResult result = vkGetQueryPoolResults(vk.device, vk.queries, 0, 2, sizeof samples, samples,
                                                      sizeof samples[0], VK_QUERY_RESULT_64_BIT);
        LOG("scene 12 query and fence: after the fence the results are there (%d): occluded %llu samples, visible: %s",
            result, (unsigned long long)samples[0], samples[1] != 0 && samples[1] != UINT64_MAX ? "yes" : "no");
    }
    report(number, name, "64 x 64", vk.small_readback.mapped, kSize);
    report(number, name, "window", vk.readback.mapped, kWindow);
    present(index);
}

// ---- Instance, device and swapchain

static void device_list(const char *what, const char *names) {
    const size_t length = strlen(names);
    for (size_t start = 0; start == 0 || start < length; start += 800) DEVICE("%s: %.800s", what, names + start);
}

static bool has_extension(const VkExtensionProperties *extensions, uint32_t count, const char *name) {
    for (uint32_t index = 0; index < count; ++index) {
        if (strcmp(extensions[index].extensionName, name) == 0) return true;
    }
    return false;
}

static void log_extensions(const char *what, const VkExtensionProperties *extensions, uint32_t count) {
    static char names[16384];
    names[0] = 0;
    for (uint32_t index = 0; index < count; ++index) {
        strncat(names, extensions[index].extensionName, sizeof names - strlen(names) - 2);
        strncat(names, " ", sizeof names - strlen(names) - 1);
    }
    device_list(what, names);
}

static bool start_instance(void) {
    uint32_t version = VK_API_VERSION_1_0;
    vkEnumerateInstanceVersion(&version);
    DEVICE("vkEnumerateInstanceVersion: %u.%u.%u", VK_API_VERSION_MAJOR(version), VK_API_VERSION_MINOR(version),
           VK_API_VERSION_PATCH(version));
    uint32_t count = 0;
    vkEnumerateInstanceExtensionProperties(NULL, &count, NULL);
    VkExtensionProperties *extensions = calloc(count, sizeof *extensions);
    vkEnumerateInstanceExtensionProperties(NULL, &count, extensions);
    log_extensions("instance extensions", extensions, count);
    free(extensions);
    const char *const names[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
    const VkApplicationInfo application = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                           .pApplicationName = "vulkan-test", .apiVersion = version};
    const VkInstanceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                       .pApplicationInfo = &application, .enabledExtensionCount = 2,
                                       .ppEnabledExtensionNames = names};
    if (!ok(vkCreateInstance(&info, NULL, &vk.instance), "vkCreateInstance")) return false;
    uint32_t devices = 1;
    const VkResult enumerated = vkEnumeratePhysicalDevices(vk.instance, &devices, &vk.physical);
    if ((enumerated != VK_SUCCESS && enumerated != VK_INCOMPLETE) || devices == 0) {
        LOG("no physical device: %d", enumerated);
        return false;
    }
    vkGetPhysicalDeviceProperties(vk.physical, &vk.properties);
    vkGetPhysicalDeviceMemoryProperties(vk.physical, &vk.memory);
    const uint32_t api = vk.properties.apiVersion < version ? vk.properties.apiVersion : version;
    DEVICE("device: %s, API %u.%u.%u, driver 0x%08x, vendor 0x%04x, device 0x%04x; in effect %u.%u",
           vk.properties.deviceName, VK_API_VERSION_MAJOR(vk.properties.apiVersion),
           VK_API_VERSION_MINOR(vk.properties.apiVersion), VK_API_VERSION_PATCH(vk.properties.apiVersion),
           vk.properties.driverVersion, vk.properties.vendorID, vk.properties.deviceID, VK_API_VERSION_MAJOR(api),
           VK_API_VERSION_MINOR(api));
    const VkPhysicalDeviceLimits *limits = &vk.properties.limits;
    DEVICE("limits: maxImageDimension2D %u, maxPushConstantsSize %u, maxColorAttachments %u, "
           "maxComputeWorkGroupInvocations %u, timestampComputeAndGraphics %u, timestampPeriod %.3f ns, "
           "minMemoryMapAlignment %zu", limits->maxImageDimension2D, limits->maxPushConstantsSize,
           limits->maxColorAttachments, limits->maxComputeWorkGroupInvocations, limits->timestampComputeAndGraphics,
           limits->timestampPeriod, limits->minMemoryMapAlignment);
    return true;
}

static bool start_device(ANativeWindow *window) {
    const VkAndroidSurfaceCreateInfoKHR surface = {.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR,
                                                   .window = window};
    if (!ok(vkCreateAndroidSurfaceKHR(vk.instance, &surface, NULL, &vk.surface), "vkCreateAndroidSurfaceKHR")) {
        return false;
    }
    uint32_t families = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(vk.physical, &families, NULL);
    VkQueueFamilyProperties properties[16];
    families = families > 16 ? 16 : families;
    vkGetPhysicalDeviceQueueFamilyProperties(vk.physical, &families, properties);
    vk.family = UINT32_MAX;
    for (uint32_t family = 0; family < families && vk.family == UINT32_MAX; ++family) {
        VkBool32 presents = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(vk.physical, family, vk.surface, &presents);
        const VkQueueFlags needed = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
        if ((properties[family].queueFlags & needed) == needed && presents) {
            vk.family = family;
            vk.timestamp_bits = properties[family].timestampValidBits;
        }
    }
    LOG("a queue family with graphics, compute and presents: %s", vk.family != UINT32_MAX ? "yes" : "no");
    if (vk.family == UINT32_MAX) return false;

    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(vk.physical, NULL, &count, NULL);
    VkExtensionProperties *extensions = calloc(count, sizeof *extensions);
    vkEnumerateDeviceExtensionProperties(vk.physical, NULL, &count, extensions);
    log_extensions("device extensions", extensions, count);
    uint32_t version = VK_API_VERSION_1_0;
    vkEnumerateInstanceVersion(&version);
    const bool core = VK_API_VERSION_MINOR(vk.properties.apiVersion) >= 2 && VK_API_VERSION_MINOR(version) >= 2;
    const bool extension = has_extension(extensions, count, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
    free(extensions);

    VkPhysicalDeviceTimelineSemaphoreFeatures timeline = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
    VkPhysicalDeviceFeatures2 features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                                          .pNext = core || extension ? &timeline : NULL};
    vkGetPhysicalDeviceFeatures2(vk.physical, &features);
    DEVICE("features: geometryShader %u, tessellationShader %u, wideLines %u, largePoints %u, logicOp %u, "
           "fillModeNonSolid %u, samplerAnisotropy %u, textureCompressionASTC_LDR %u, textureCompressionBC %u, "
           "occlusionQueryPrecise %u, shaderInt64 %u", features.features.geometryShader,
           features.features.tessellationShader, features.features.wideLines, features.features.largePoints,
           features.features.logicOp, features.features.fillModeNonSolid, features.features.samplerAnisotropy,
           features.features.textureCompressionASTC_LDR, features.features.textureCompressionBC,
           features.features.occlusionQueryPrecise, features.features.shaderInt64);
    vk.etc2 = features.features.textureCompressionETC2 == VK_TRUE;
    LOG("features: textureCompressionETC2 %s, timelineSemaphore %s", vk.etc2 ? "yes" : "no",
        timeline.timelineSemaphore ? "yes" : "no");
    if (!vk.etc2 || !timeline.timelineSemaphore) return false;
    const VkFormat depths[2] = {VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT};
    vk.depth_format = VK_FORMAT_UNDEFINED;
    for (int index = 0; index < 2 && vk.depth_format == VK_FORMAT_UNDEFINED; ++index) {
        VkFormatProperties format;
        vkGetPhysicalDeviceFormatProperties(vk.physical, depths[index], &format);
        if (format.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) vk.depth_format = depths[index];
    }
    DEVICE("depth and stencil: %s", vk.depth_format == VK_FORMAT_D24_UNORM_S8_UINT ? "D24_UNORM_S8_UINT"
                                    : vk.depth_format == VK_FORMAT_D32_SFLOAT_S8_UINT ? "D32_SFLOAT_S8_UINT"
                                                                                    : "none");
    if (vk.depth_format == VK_FORMAT_UNDEFINED) return false;

    const float priority = 1;
    const VkDeviceQueueCreateInfo queue = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                           .queueFamilyIndex = vk.family, .queueCount = 1,
                                           .pQueuePriorities = &priority};
    const char *names[2] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME};
    VkPhysicalDeviceTimelineSemaphoreFeatures enable_timeline = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES, .timelineSemaphore = VK_TRUE};
    const VkPhysicalDeviceFeatures enable = {.textureCompressionETC2 = VK_TRUE};
    const VkDeviceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &enable_timeline,
                                     .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue,
                                     .enabledExtensionCount = core ? 1 : 2, .ppEnabledExtensionNames = names,
                                     .pEnabledFeatures = &enable};
    if (!ok(vkCreateDevice(vk.physical, &info, NULL, &vk.device), "vkCreateDevice")) return false;
    vkGetDeviceQueue(vk.device, vk.family, 0, &vk.queue);
    vk.wait_semaphores = (PFN_vkWaitSemaphores)vkGetDeviceProcAddr(vk.device, core ? "vkWaitSemaphores"
                                                                                  : "vkWaitSemaphoresKHR");
    vk.signal_semaphore = (PFN_vkSignalSemaphore)vkGetDeviceProcAddr(vk.device, core ? "vkSignalSemaphore"
                                                                                    : "vkSignalSemaphoreKHR");
    vk.counter_value = (PFN_vkGetSemaphoreCounterValue)vkGetDeviceProcAddr(
        vk.device, core ? "vkGetSemaphoreCounterValue" : "vkGetSemaphoreCounterValueKHR");
    DEVICE("timeline semaphores: %s", core ? "core" : "VK_KHR_timeline_semaphore");
    if (vk.wait_semaphores == NULL || vk.signal_semaphore == NULL || vk.counter_value == NULL) {
        LOG("the timeline semaphore functions are missing");
        return false;
    }

    const VkCommandPoolCreateInfo pool = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                          .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                          .queueFamilyIndex = vk.family};
    if (!ok(vkCreateCommandPool(vk.device, &pool, NULL, &vk.pool), "vkCreateCommandPool")) return false;
    VkCommandBuffer buffers[3];
    const VkCommandBufferAllocateInfo allocate = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                                  .commandPool = vk.pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                                  .commandBufferCount = 3};
    const VkFenceCreateInfo fence = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    const VkSemaphoreCreateInfo semaphore = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    if (!ok(vkAllocateCommandBuffers(vk.device, &allocate, buffers), "vkAllocateCommandBuffers") ||
        !ok(vkCreateFence(vk.device, &fence, NULL, &vk.fence), "vkCreateFence") ||
        !ok(vkCreateSemaphore(vk.device, &semaphore, NULL, &vk.acquired), "vkCreateSemaphore")) {
        return false;
    }
    vk.commands = buffers[0];
    vk.first = buffers[1];
    vk.second = buffers[2];
    return true;
}

static const char *format_name(VkFormat format) {
    switch (format) {
        case VK_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
        case VK_FORMAT_R8G8B8A8_SRGB: return "R8G8B8A8_SRGB";
        case VK_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
        case VK_FORMAT_B8G8R8A8_SRGB: return "B8G8R8A8_SRGB";
        case VK_FORMAT_R5G6B5_UNORM_PACK16: return "R5G6B5_UNORM_PACK16";
        case VK_FORMAT_R16G16B16A16_SFLOAT: return "R16G16B16A16_SFLOAT";
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return "A2B10G10R10_UNORM_PACK32";
        default: return "another";
    }
}

static bool offers(VkFormat wanted) {
    uint32_t count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(vk.physical, vk.surface, &count, NULL);
    VkSurfaceFormatKHR formats[32];
    count = count > 32 ? 32 : count;
    vkGetPhysicalDeviceSurfaceFormatsKHR(vk.physical, vk.surface, &count, formats);
    for (uint32_t index = 0; index < count; ++index) {
        if (formats[index].format == wanted) return true;
    }
    return false;
}

static void log_surface(void) {
    VkSurfaceCapabilitiesKHR capabilities;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk.physical, vk.surface, &capabilities);
    DEVICE("surface: current %u x %u, extents %u x %u to %u x %u, images %u to %u, transforms 0x%x (current 0x%x), "
           "composite alpha 0x%x, usage 0x%x", capabilities.currentExtent.width, capabilities.currentExtent.height,
           capabilities.minImageExtent.width, capabilities.minImageExtent.height, capabilities.maxImageExtent.width,
           capabilities.maxImageExtent.height, capabilities.minImageCount, capabilities.maxImageCount,
           capabilities.supportedTransforms, capabilities.currentTransform, capabilities.supportedCompositeAlpha,
           capabilities.supportedUsageFlags);
    uint32_t count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(vk.physical, vk.surface, &count, NULL);
    VkSurfaceFormatKHR formats[32];
    count = count > 32 ? 32 : count;
    vkGetPhysicalDeviceSurfaceFormatsKHR(vk.physical, vk.surface, &count, formats);
    char text[1024] = "";
    for (uint32_t index = 0; index < count; ++index) {
        snprintf(text + strlen(text), sizeof text - strlen(text), "%s%s/%d", index == 0 ? "" : " ",
                 format_name(formats[index].format), formats[index].colorSpace);
    }
    DEVICE("surface formats: %s", text);
    VkPresentModeKHR modes[8];
    count = 8;
    vkGetPhysicalDeviceSurfacePresentModesKHR(vk.physical, vk.surface, &count, modes);
    text[0] = 0;
    for (uint32_t index = 0; index < count; ++index) {
        snprintf(text + strlen(text), sizeof text - strlen(text), "%s%d", index == 0 ? "" : " ", modes[index]);
    }
    DEVICE("present modes: %s", text);
}

static void destroy_chain(struct swapchain *chain, bool swapchain_too) {
    for (uint32_t index = 0; index < chain->count; ++index) {
        vkDestroyFramebuffer(vk.device, chain->framebuffers[index], NULL);
        vkDestroyImageView(vk.device, chain->images[index].view, NULL);
        vkDestroySemaphore(vk.device, chain->rendered[index], NULL);
    }
    if (chain->render_pass != VK_NULL_HANDLE) vkDestroyRenderPass(vk.device, chain->render_pass, NULL);
    if (swapchain_too) {
        vkDestroySwapchainKHR(vk.device, chain->swapchain, NULL);
        chain->swapchain = VK_NULL_HANDLE;
    }
    chain->count = 0;
    chain->render_pass = VK_NULL_HANDLE;
}

// A swapchain of `format` and `extent` with FIFO, replacing the one there is. The scenes' swapchain (with its
// depth attachment) serves the scenes' passes; a measurement's has a pass of its own that only clears.
static bool make_chain(VkFormat format, VkExtent2D extent, VkImageUsageFlags usage, bool scenes) {
    VkSurfaceCapabilitiesKHR capabilities;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk.physical, vk.surface, &capabilities);
    uint32_t images = capabilities.minImageCount > 3 ? capabilities.minImageCount : 3;
    if (capabilities.maxImageCount != 0 && images > capabilities.maxImageCount) images = capabilities.maxImageCount;
    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    const VkCompositeAlphaFlagBitsKHR alphas[4] = {VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR, VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
                                                   VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
                                                   VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR};
    for (int index = 3; index >= 0; --index) {
        if (capabilities.supportedCompositeAlpha & alphas[index]) alpha = alphas[index];
    }
    struct swapchain old = vk.chain;
    const VkSwapchainCreateInfoKHR info = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = vk.surface, .minImageCount = images,
        .imageFormat = format, .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, .imageExtent = extent,
        .imageArrayLayers = 1, .imageUsage = usage, .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = capabilities.currentTransform, .compositeAlpha = alpha, .presentMode = VK_PRESENT_MODE_FIFO_KHR,
        .clipped = VK_TRUE, .oldSwapchain = old.swapchain};
    memset(&vk.chain, 0, sizeof vk.chain);
    vk.chain.format = format;
    vk.chain.extent = extent;
    const VkResult created = vkCreateSwapchainKHR(vk.device, &info, NULL, &vk.chain.swapchain);
    if (old.swapchain != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(vk.device);
        destroy_chain(&old, true);
    }
    if (!ok(created, "vkCreateSwapchainKHR")) return false;
    vk.chain.count = kMaxImages;
    VkImage handles[kMaxImages];
    if (!ok(vkGetSwapchainImagesKHR(vk.device, vk.chain.swapchain, &vk.chain.count, handles),
            "vkGetSwapchainImagesKHR")) {
        return false;
    }
    DEVICE("swapchain: %s, %u x %u, %u images asked, %u given", format_name(format), extent.width, extent.height,
           images, vk.chain.count);
    if (!scenes) {
        vk.chain.render_pass = render_pass(format, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_IMAGE_LAYOUT_UNDEFINED,
                                           VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, false);
    }
    const VkSemaphoreCreateInfo semaphore = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (uint32_t index = 0; index < vk.chain.count; ++index) {
        struct image *image = &vk.chain.images[index];
        image->image = handles[index];
        image->aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        image->layout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (!make_view(image, format) ||
            !ok(vkCreateSemaphore(vk.device, &semaphore, NULL, &vk.chain.rendered[index]), "vkCreateSemaphore")) {
            return false;
        }
        vk.chain.framebuffers[index] =
            scenes ? framebuffer(vk.color_pass, image, &vk.window_depth, extent.width, extent.height)
                   : framebuffer(vk.chain.render_pass, image, NULL, extent.width, extent.height);
    }
    return true;
}

// One frame: a pass that clears to `color`, presented.
static bool clear_frame(const float color[4], VkRenderPass render_pass) {
    uint32_t index = 0;
    if (!acquire(&index) || !begin(vk.commands)) return false;
    VkClearValue values[2];
    memset(values, 0, sizeof values);
    memcpy(values[0].color.float32, color, 4 * sizeof(float));
    values[1].depthStencil.depth = 1;
    const VkRenderPassBeginInfo info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = render_pass,
        .framebuffer = vk.chain.framebuffers[index], .renderArea = {{0, 0}, vk.chain.extent},
        .clearValueCount = render_pass == vk.present_pass ? 2 : 1, .pClearValues = values};
    vkCmdBeginRenderPass(vk.commands, &info, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdEndRenderPass(vk.commands);
    vk.chain.images[index].layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    if (!submit_and_wait(vk.commands, vk.acquired, vk.chain.rendered[index])) return false;
    present(index);
    return true;
}

// ---- The measurements of ADR-0030 (device lines only)

static int compare_doubles(const void *a, const void *b) {
    const double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

// What a call through the front lib costs: vkGetRenderAreaGranularity, a plain one. At most a million calls or
// a quarter of a second, since in the emulator every call crosses to the host.
static void measure_calls(void) {
    VkExtent2D granularity;
    int calls = 0;
    const int64_t start = now();
    int64_t end = start;
    while (calls < 1000000 && end - start < 250000000) {
        for (int call = 0; call < 1000; ++call) vkGetRenderAreaGranularity(vk.device, vk.color_pass, &granularity);
        calls += 1000;
        end = now();
    }
    DEVICE("a call of vkGetRenderAreaGranularity: %.1f ns over %d calls", (double)(end - start) / calls, calls);
}

// An image of `format` and `extent` filled with noise from the generator, which no GPU compresses away as it
// does a single color: the source of the copy and of the measured frames, which copy it in their own format
// (MoltenVK 1.4.2 keeps a view of a swapchain image in another format across drawables, and crashes when that
// swapchain is destroyed).
static bool make_noise(VkFormat format, VkExtent2D extent, struct image *noise) {
    const VkDeviceSize size = (VkDeviceSize)extent.width * extent.height * 4;
    struct buffer upload;
    if (!make_image(format, extent.width, extent.height,
                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT, noise) ||
        !make_buffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &upload)) {
        return false;
    }
    fill(upload.mapped, (size_t)size, 5);
    bool made = begin(vk.commands);
    if (made) {
        transition(vk.commands, noise, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        const VkBufferImageCopy region = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                          .imageExtent = {extent.width, extent.height, 1}};
        vkCmdCopyBufferToImage(vk.commands, upload.buffer, noise->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                               &region);
        transition(vk.commands, noise, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        made = submit_and_wait(vk.commands, VK_NULL_HANDLE, VK_NULL_HANDLE);
    }
    vkDestroyBuffer(vk.device, upload.buffer, NULL);
    vkFreeMemory(vk.device, upload.memory, NULL);
    return made;
}

// The copy that R8G8B8A8 swapchains cost (ADR-0030, decision 12): vkCmdBlitImage with NEAREST from R8G8B8A8
// into B8G8R8A8 at the window's size, as libvulkan's copy does, 60 times between timestamps. On Apple's GPUs
// MoltenVK 1.4.2 takes a timestamp only once the Metal encoder that follows it ends, and its render encoders
// (a blit that converts) do not hold the timestamp's pass back. So a dispatch ends the encoder before each
// copy, and reading one pixel of the copy's target, which Metal makes wait for the copy, comes before the
// second timestamp; both add a few microseconds.
static void measure_copy(VkExtent2D extent, const struct image *noise) {
    struct image destination;
    if (!make_image(VK_FORMAT_B8G8R8A8_UNORM, extent.width, extent.height,
                    VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                    &destination)) {
        return;
    }
    VkQueryPool pool = VK_NULL_HANDLE;
    const VkQueryPoolCreateInfo info = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                        .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 2 * kCopies};
    const bool timestamps = vk.timestamp_bits != 0 && ok(vkCreateQueryPool(vk.device, &info, NULL, &pool),
                                                         "vkCreateQueryPool");
    if (begin(vk.commands)) {
        transition(vk.commands, &destination, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        if (timestamps) vkCmdResetQueryPool(vk.commands, pool, 0, 2 * kCopies);
        const VkImageBlit blit = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                  .srcOffsets = {{0, 0, 0}, {(int32_t)extent.width, (int32_t)extent.height, 1}},
                                  .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                  .dstOffsets = {{0, 0, 0}, {(int32_t)extent.width, (int32_t)extent.height, 1}}};
        const VkBufferImageCopy pixel = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                         .imageOffset = {(int32_t)extent.width - 1, (int32_t)extent.height - 1, 0},
                                         .imageExtent = {1, 1, 1}};
        vkCmdBindPipeline(vk.commands, VK_PIPELINE_BIND_POINT_COMPUTE, vk.compute);
        vkCmdBindDescriptorSets(vk.commands, VK_PIPELINE_BIND_POINT_COMPUTE, vk.compute_layout, 0, 1, &vk.compute_set, 0,
                                NULL);
        for (uint32_t copy = 0; copy < kCopies; ++copy) {
            if (timestamps) vkCmdWriteTimestamp(vk.commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, pool, 2 * copy);
            vkCmdDispatch(vk.commands, 1, 1, 1);
            buffer_barrier(vk.commands);
            vkCmdBlitImage(vk.commands, noise->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
            transition(vk.commands, &destination, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            vkCmdCopyImageToBuffer(vk.commands, destination.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   vk.small_readback.buffer, 1, &pixel);
            buffer_barrier(vk.commands);
            if (timestamps) vkCmdWriteTimestamp(vk.commands, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, pool, 2 * copy + 1);
            transition(vk.commands, &destination, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        }
        const int64_t start = now();
        if (submit_and_wait(vk.commands, VK_NULL_HANDLE, VK_NULL_HANDLE)) {
            const double wall = (double)(now() - start) / 1e6;
            char gpu[160] = "no timestamps on this queue";
            uint64_t stamps[2 * kCopies];
            if (timestamps && vkGetQueryPoolResults(vk.device, pool, 0, 2 * kCopies, sizeof stamps, stamps,
                                                    sizeof stamps[0], VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
                const uint64_t mask = vk.timestamp_bits >= 64 ? UINT64_MAX : (UINT64_C(1) << vk.timestamp_bits) - 1;
                double times[kCopies];
                for (int copy = 0; copy < kCopies; ++copy) {
                    times[copy] = (double)((stamps[2 * copy + 1] - stamps[2 * copy]) & mask) *
                                  vk.properties.limits.timestampPeriod / 1000.0;
                }
                qsort(times, kCopies, sizeof times[0], compare_doubles);
                snprintf(gpu, sizeof gpu, "GPU median %.1f us, min %.1f us, max %.1f us", times[kCopies / 2], times[0],
                         times[kCopies - 1]);
            }
            DEVICE("the copy, R8G8B8A8 to B8G8R8A8 at %u x %u with NEAREST, of noise: %s; %d copies in %.2f ms",
                   extent.width, extent.height, gpu, kCopies, wall);
        }
    }
    if (pool != VK_NULL_HANDLE) vkDestroyQueryPool(vk.device, pool, NULL);
    destroy_image(&destination);
}

// One measured frame: the noise copied 1:1 into the swapchain's image, presented.
static bool noise_frame(const struct image *noise) {
    uint32_t index = 0;
    if (!acquire(&index) || !begin(vk.commands)) return false;
    struct image *image = &vk.chain.images[index];
    image->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    transition(vk.commands, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const VkImageCopy copy = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                              .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                              .extent = {vk.chain.extent.width, vk.chain.extent.height, 1}};
    vkCmdCopyImage(vk.commands, noise->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image->image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    transition(vk.commands, image, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    if (!submit_and_wait(vk.commands, vk.acquired, vk.chain.rendered[index])) return false;
    present(index);
    return true;
}

// 120 presents of `format` at the window's size, each a frame of noise: the frame times through the copy
// (R8G8B8A8) or directly (B8G8R8A8).
static void measure_frames(VkFormat format, VkExtent2D extent, const char *way, const struct image *noise) {
    if (!offers(format)) {
        DEVICE("%s: %s is not offered", way, format_name(format));
        return;
    }
    if (!make_chain(format, extent, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, false)) {
        return;
    }
    double intervals[kFrames] = {0};
    const int presented = vk.presented;
    int64_t last = now();
    const int64_t start = last;
    for (int frame = 0; frame < kFrames; ++frame) {
        if (!noise_frame(noise)) break;
        const int64_t time = now();
        intervals[frame] = (double)(time - last) / 1e6;
        last = time;
    }
    const int frames = vk.presented - presented;
    qsort(intervals, (size_t)kFrames, sizeof intervals[0], compare_doubles);
    DEVICE("%s, %s at %u x %u: %d presents in %.1f ms, %.1f per second; frame time median %.2f ms, p99 %.2f ms", way,
           format_name(format), extent.width, extent.height, frames, (double)(last - start) / 1e6,
           frames * 1e9 / (double)(last - start), intervals[kFrames / 2], intervals[kFrames * 99 / 100]);
}

static void measure(void) {
    VkSurfaceCapabilitiesKHR capabilities;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk.physical, vk.surface, &capabilities);
    const VkExtent2D extent = capabilities.currentExtent;
    measure_calls();
    struct image noise, bgra_noise;
    if (!make_noise(VK_FORMAT_R8G8B8A8_UNORM, extent, &noise)) return;
    measure_copy(extent, &noise);
    measure_frames(VK_FORMAT_R8G8B8A8_UNORM, extent, "through the copy", &noise);
    if (make_noise(VK_FORMAT_B8G8R8A8_UNORM, extent, &bgra_noise)) {
        measure_frames(VK_FORMAT_B8G8R8A8_UNORM, extent, "directly", &bgra_noise);
        vkDeviceWaitIdle(vk.device);
        destroy_image(&bgra_noise);
    }
    vkDeviceWaitIdle(vk.device);
    destroy_image(&noise);
}

// ---- The app

static bool start_vulkan(ANativeWindow *window) {
    if (!start_instance() || !start_device(window)) return false;
    log_surface();
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!make_pipelines() || !make_objects() ||
        !make_chain(VK_FORMAT_R8G8B8A8_UNORM, (VkExtent2D){kWindow, kWindow}, usage, true)) {
        return false;
    }
    LOG("swapchain: R8G8B8A8_UNORM, FIFO, %u x %u", vk.chain.extent.width, vk.chain.extent.height);
    return true;
}

// What depends on the window goes with it; the device stays for the process's life, as in many apps.
static void stop_vulkan(void) {
    const VkResult idle = vkDeviceWaitIdle(vk.device);
    destroy_chain(&vk.chain, true);
    vkDestroySurfaceKHR(vk.instance, vk.surface, NULL);
    vk.surface = VK_NULL_HANDLE;
    vk.ready = false;
    LOG("APP_CMD_TERM_WINDOW: vkDeviceWaitIdle %d, the swapchain and the surface destroyed", idle);
}

// ---- The live variant (vulkan-live)

enum { kPhaseSeconds = 5, kMaxIntervals = 4096 };

static struct {
    bool on;       // started at ANativeActivity_onCreateLive
    AChoreographer *choreographer;
    bool ready;    // the window's swapchain is there
    bool focused;
    bool lost;     // an acquire or present found the window gone: no frames until the next window
    int windows, frames, failures, losses;
    float rate;            // the frames a second the window is asked for: 120 or 60
    int64_t phase_start;   // the frame time the rate's phase began, 0 before the first callback
    int64_t previous;      // the frame time of the frame before, 0 after a break
    int interval_count;
    double intervals[kMaxIntervals];  // ms
} live = {.rate = 120};

// The live variant's entry, which its manifest names (android.app.func_name).
__attribute__((visibility("default"))) void ANativeActivity_onCreateLive(ANativeActivity *activity, void *saved_state,
                                                                         size_t saved_state_size) {
    live.on = true;
    tag = "vulkan-live";
    device_tag = "vulkan-live-device";
    ANativeActivity_onCreate(activity, saved_state, saved_state_size);
}

// The frame intervals of the rate's phase: the median and the 99th percentile by nearest rank, and the longest.
static void log_phase(void) {
    const int count = live.interval_count;
    if (count == 0) return;
    qsort(live.intervals, (size_t)count, sizeof live.intervals[0], compare_doubles);
    DEVICE("rate %.0f: %d frame intervals, median %.2f ms, p99 %.2f ms, max %.2f ms", live.rate, count,
           live.intervals[(count + 1) / 2 - 1], live.intervals[(99 * count + 99) / 100 - 1], live.intervals[count - 1]);
    live.interval_count = 0;
}

typedef int32_t (*set_frame_rate_function)(ANativeWindow *window, float rate, int8_t compatibility);

// ANativeWindow_setFrameRate came with API 30, above the minSdk: from libnativewindow by dlsym.
static void ask_rate(ANativeWindow *window) {
    static set_frame_rate_function set_frame_rate;
    static bool looked;
    if (!looked) {
        looked = true;
        void *library = dlopen("libnativewindow.so", RTLD_NOW);
        if (library != NULL) set_frame_rate = (set_frame_rate_function)dlsym(library, "ANativeWindow_setFrameRate");
        DEVICE("ANativeWindow_setFrameRate: %s", set_frame_rate != NULL ? "there" : "not there");
    }
    if (set_frame_rate != NULL) {
        DEVICE("ANativeWindow_setFrameRate(%.0f): %d", live.rate,
               set_frame_rate(window, live.rate, ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_DEFAULT));
    }
}

// A result of a live frame: true to go on. A window that is gone counts as a loss, anything else as a failure.
static bool live_result(VkResult result, const char *what) {
    if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) return true;
    if (result == VK_ERROR_SURFACE_LOST_KHR || result == VK_ERROR_OUT_OF_DATE_KHR) {
        ++live.losses;
        live.lost = true;
        DEVICE("frame %d: %s: the window is gone (%d)", live.frames, what, result);
    } else {
        ++live.failures;
        DEVICE("frame %d: %s failed: %d", live.frames, what, result);
    }
    return false;
}

// A window: the instance and the device with the first, and for each a surface and a swapchain of
// R8G8B8A8_UNORM, Android's first format, at the surface's size.
static void live_window(ANativeWindow *window) {
    ++live.windows;
    live.lost = false;
    if (vk.device == VK_NULL_HANDLE) {
        if (!start_instance() || !start_device(window)) {
            ++live.failures;
            return;
        }
    } else {
        const VkAndroidSurfaceCreateInfoKHR surface = {.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR,
                                                       .window = window};
        if (!ok(vkCreateAndroidSurfaceKHR(vk.instance, &surface, NULL, &vk.surface), "vkCreateAndroidSurfaceKHR")) {
            ++live.failures;
            return;
        }
    }
    VkBool32 presents = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(vk.physical, vk.family, vk.surface, &presents);
    VkSurfaceCapabilitiesKHR capabilities;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk.physical, vk.surface, &capabilities);
    if (!presents || !make_chain(VK_FORMAT_R8G8B8A8_UNORM, capabilities.currentExtent,
                                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, false)) {
        ++live.failures;
        DEVICE("window %d: no swapchain (presents %u)", live.windows, presents);
        return;
    }
    live.ready = true;
    DEVICE("window %d: swapchain %u x %u, %u images", live.windows, vk.chain.extent.width, vk.chain.extent.height,
           vk.chain.count);
    ask_rate(window);
}

// The window goes, and its swapchain and surface with it; the device stays for the process's life.
static void live_window_gone(void) {
    live.ready = false;
    live.previous = 0;
    if (vk.surface == VK_NULL_HANDLE) return;
    if (vk.device != VK_NULL_HANDLE) {
        const VkResult idle = vkDeviceWaitIdle(vk.device);
        if (idle != VK_SUCCESS) {
            ++live.failures;
            DEVICE("window %d: vkDeviceWaitIdle %d", live.windows, idle);
        }
        destroy_chain(&vk.chain, true);
    }
    vkDestroySurfaceKHR(vk.instance, vk.surface, NULL);
    vk.surface = VK_NULL_HANDLE;
}

// A frame: a render pass that clears to a dark color and a square that moves with the frame count, presented.
static void live_frame(void) {
    uint32_t index = 0;
    if (!live_result(vkAcquireNextImageKHR(vk.device, vk.chain.swapchain, UINT64_MAX, vk.acquired, VK_NULL_HANDLE,
                                           &index),
                     "vkAcquireNextImageKHR")) {
        return;
    }
    if (!begin(vk.commands)) {
        ++live.failures;
        return;
    }
    const VkExtent2D extent = vk.chain.extent;
    const uint32_t size = (extent.width < extent.height ? extent.width : extent.height) / 4;
    const VkClearValue background = {.color = {.float32 = {0, 0, 0.2f, 1}}};
    const VkRenderPassBeginInfo pass = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = vk.chain.render_pass,
        .framebuffer = vk.chain.framebuffers[index], .renderArea = {{0, 0}, extent}, .clearValueCount = 1,
        .pClearValues = &background};
    vkCmdBeginRenderPass(vk.commands, &pass, VK_SUBPASS_CONTENTS_INLINE);
    const VkClearAttachment square = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .colorAttachment = 0,
                                      .clearValue = {.color = {.float32 = {1, 0.6f, 0, 1}}}};
    const int32_t x = extent.width > size ? (int32_t)(((uint32_t)live.frames * 8) % (extent.width - size)) : 0;
    const VkClearRect rect = {.rect = {{x, (int32_t)((extent.height - size) / 2)}, {size, size}}, .layerCount = 1};
    vkCmdClearAttachments(vk.commands, 1, &square, 1, &rect);
    vkCmdEndRenderPass(vk.commands);
    if (!submit_and_wait(vk.commands, vk.acquired, vk.chain.rendered[index])) {
        ++live.failures;
        return;
    }
    const VkPresentInfoKHR info = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1,
                                   .pWaitSemaphores = &vk.chain.rendered[index], .swapchainCount = 1,
                                   .pSwapchains = &vk.chain.swapchain, .pImageIndices = &index};
    if (live_result(vkQueuePresentKHR(vk.queue, &info), "vkQueuePresentKHR")) ++live.frames;
}

// A frame per callback while the app has a window and the focus; every 5 s of frame time the other rate.
static void on_frame(int64_t frame_time, void *data) {
    struct android_app *app = data;
    if (app->destroyRequested) return;
    AChoreographer_postFrameCallback64(live.choreographer, on_frame, app);
    if (live.phase_start == 0) live.phase_start = frame_time;
    if (frame_time - live.phase_start >= kPhaseSeconds * 1000000000LL) {
        log_phase();
        live.rate = live.rate == 120 ? 60 : 120;
        live.phase_start = frame_time;
        live.previous = 0;
        if (live.ready) ask_rate(app->window);
    }
    if (!live.ready || !live.focused || live.lost) {
        live.previous = 0;
        return;
    }
    if (live.previous != 0 && live.interval_count < kMaxIntervals) {
        live.intervals[live.interval_count++] = (double)(frame_time - live.previous) / 1e6;
    }
    live.previous = frame_time;
    live_frame();
}

// BACK finishes the live variant: from Android 12 on, BACK alone only moves a root activity to the back.
static int32_t on_input(struct android_app *app, AInputEvent *event) {
    if (AInputEvent_getType(event) != AINPUT_EVENT_TYPE_KEY || AKeyEvent_getKeyCode(event) != AKEYCODE_BACK) return 0;
    if (AKeyEvent_getAction(event) == AKEY_EVENT_ACTION_UP) {
        LOG("AKEYCODE_BACK: ANativeActivity_finish");
        ANativeActivity_finish(app->activity);
    }
    return 1;
}

static void on_live_command(struct android_app *app, int32_t command) {
    switch (command) {
        case APP_CMD_INIT_WINDOW:
            live_window(app->window);
            break;
        case APP_CMD_TERM_WINDOW:
            live_window_gone();
            break;
        case APP_CMD_GAINED_FOCUS:
            live.focused = true;
            break;
        case APP_CMD_LOST_FOCUS:
            live.focused = false;
            live.previous = 0;  // a break, no frame interval
            break;
        case APP_CMD_DESTROY:
            log_phase();
            LOG("onDestroy: frames %d, windows %d, failures %d, lost %d", live.frames, live.windows, live.failures,
                live.losses);
            break;
        default:
            break;
    }
}

// The live variant's loop: the frames come from the choreographer's callbacks inside ALooper_pollOnce.
static void live_main(struct android_app *app) {
    app->onInputEvent = on_input;
    live.choreographer = AChoreographer_getInstance();
    if (live.choreographer == NULL) {
        ++live.failures;
        LOG("no AChoreographer");
    } else {
        AChoreographer_postFrameCallback64(live.choreographer, on_frame, app);
    }
    while (!app->destroyRequested) {
        struct android_poll_source *source = NULL;
        const int result = ALooper_pollOnce(-1, NULL, NULL, (void **)&source);
        if ((result == LOOPER_ID_MAIN || result == LOOPER_ID_INPUT) && source != NULL) source->process(app, source);
    }
}

static void on_command(struct android_app *app, int32_t command) {
    if (command >= 0 && command < (int32_t)(sizeof command_names / sizeof command_names[0])) {
        DEVICE("%s", command_names[command]);
    }
    if (live.on) {
        on_live_command(app, command);
        return;
    }
    switch (command) {
        case APP_CMD_INIT_WINDOW:
            if (vk.device == VK_NULL_HANDLE && vk.presents == 0 && start_vulkan(app->window)) {
                const int64_t start = now();
                vk.ready = true;
                for (int number = 1; number <= 12; ++number) scene(number);
                DEVICE("12 scenes in %.1f ms", (double)(now() - start) / 1e6);
                vk.frames_left = kFrames;
            }
            break;
        case APP_CMD_TERM_WINDOW:
            if (vk.surface != VK_NULL_HANDLE) stop_vulkan();
            vk.frames_left = 0;
            break;
        default:
            break;
    }
}

// One of the 120 frames: a clear of a color that changes each frame; after the last, the measurements.
static void frame(struct android_app *app) {
    const int index = kFrames - vk.frames_left;
    const float color[4] = {(float)(index & 1), (float)((index >> 1) & 1), (float)((index >> 2) & 1), 1};
    clear_frame(color, vk.present_pass);
    if (--vk.frames_left == 0) {
        LOG("presents: %d of %d succeeded", vk.presented, vk.presents);
        measure();
        LOG("ANativeActivity_finish");
        ANativeActivity_finish(app->activity);
    }
}

void android_main(struct android_app *app) {
    app->onAppCmd = on_command;
    LOG("android_main");
    if (live.on) live_main(app);
    while (!app->destroyRequested) {
        const bool drawing = vk.ready && vk.frames_left > 0;
        struct android_poll_source *source = NULL;
        const int result = ALooper_pollOnce(drawing ? 0 : -1, NULL, NULL, (void **)&source);
        if ((result == LOOPER_ID_MAIN || result == LOOPER_ID_INPUT) && source != NULL) source->process(app, source);
        if (vk.ready && vk.frames_left > 0) frame(app);
    }
    LOG("android_main returns");
}
