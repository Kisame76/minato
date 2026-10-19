/* ADR-0058 e1), decision 7, the graphics breadth's G3d: vk-ahb-test, VK_ANDROID_external_memory_android_hardware_buffer
   of the Vulkan front on MoltenVK (graphics.vk_ahb), with Android 15's answers from the emulator, where it runs as a
   shell program (tools/android-reference.sh):
   - the extension and VK_EXT_queue_family_foreign among the device's, and an image's format properties for the
     AHardwareBuffer handle type;
   - vkGetAndroidHardwareBufferPropertiesANDROID for three formats of the format equivalence table;
   - the import of an 8 x 8 buffer the CPU filled, as a dedicated VkDeviceMemory of a VkImage, sampled by a compute
     shader through a NEAREST sampler at each texel's centre, every texel printed; a write of the CPU after a release
     to the foreign queue family, seen after the next acquire, since the image is the buffer and no copy;
   - the export: an image's dedicated memory as an AHardwareBuffer, the same buffer for a second
     vkGetMemoryAndroidHardwareBufferANDROID, its description, a clear of the GPU sampled through the buffer imported
     into a second image, and the buffer still there after the app released what it got, while the memory is.
   No value of a driver alone is printed: a size as "fits", memory types as "some", the usages as the bits the
   specification's equivalence table asks for. fixtures/apps/vk-ahb-test runs the same test as an app, the way it runs
   on the iPhone, with SAY and VK_AHB_MAIN of its own. */
#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>

#include <android/hardware_buffer.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Where the lines go: stdout, or the app's log. */
#ifndef SAY
#define SAY printf
#endif
#ifndef VK_AHB_MAIN
#define VK_AHB_MAIN main
#endif

enum { SIDE = 8 };

static VkDevice device;
static VkPhysicalDevice physical;
static VkQueue queue;
static uint32_t family;
static VkCommandPool pool;
static PFN_vkGetAndroidHardwareBufferPropertiesANDROID properties_of;
static PFN_vkGetMemoryAndroidHardwareBufferANDROID buffer_of;

static void check(const char *what, VkResult result) {
    if (result != VK_SUCCESS) {
        SAY("%s: %d\n", what, result);
        exit(1);
    }
}

static int offered(const VkExtensionProperties *list, uint32_t count, const char *name) {
    for (uint32_t index = 0; index < count; index++) {
        if (strcmp(list[index].extensionName, name) == 0) {
            return 1;
        }
    }
    return 0;
}

/* The lowest memory type of `bits` with all of `flags`, or UINT32_MAX. */
static uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties memory;
    vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    for (uint32_t index = 0; index < memory.memoryTypeCount; index++) {
        if ((bits & (1u << index)) != 0 && (memory.memoryTypes[index].propertyFlags & flags) == flags) {
            return index;
        }
    }
    return UINT32_MAX;
}

static const char *format_name(VkFormat format) {
    switch (format) {
        case VK_FORMAT_UNDEFINED: return "VK_FORMAT_UNDEFINED";
        case VK_FORMAT_R8G8B8A8_UNORM: return "VK_FORMAT_R8G8B8A8_UNORM";
        case VK_FORMAT_R5G6B5_UNORM_PACK16: return "VK_FORMAT_R5G6B5_UNORM_PACK16";
        default: return "another format";
    }
}

static AHardwareBuffer *allocated(uint32_t format, uint64_t usage) {
    AHardwareBuffer_Desc description;
    memset(&description, 0, sizeof description);
    description.width = SIDE;
    description.height = SIDE;
    description.layers = 1;
    description.format = format;
    description.usage = usage;
    AHardwareBuffer *buffer = NULL;
    return AHardwareBuffer_allocate(&description, &buffer) == 0 ? buffer : NULL;
}

static uint32_t stride_of(AHardwareBuffer *buffer) {
    AHardwareBuffer_Desc description;
    AHardwareBuffer_describe(buffer, &description);
    return description.stride;
}

/* Texel (x, y) of the pattern as R, G, B, A bytes. */
static void texel(uint32_t x, uint32_t y, uint8_t out[4]) {
    out[0] = (uint8_t)(x * 32);
    out[1] = (uint8_t)(y * 32);
    out[2] = (uint8_t)(200 + x + y);
    out[3] = (uint8_t)(255 - x * 16);
}

static int fill(AHardwareBuffer *buffer) {
    void *address = NULL;
    if (AHardwareBuffer_lock(buffer, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, NULL, &address) != 0) {
        return -1;
    }
    const uint32_t stride = stride_of(buffer);
    for (uint32_t y = 0; y < SIDE; y++) {
        for (uint32_t x = 0; x < SIDE; x++) {
            texel(x, y, (uint8_t *)address + ((size_t)y * stride + x) * 4);
        }
    }
    return AHardwareBuffer_unlock(buffer, NULL);
}

static void print_properties(const char *name, uint32_t format) {
    AHardwareBuffer *buffer =
        allocated(format, AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN);
    if (buffer == NULL) {
        SAY("properties %s: no buffer\n", name);
        return;
    }
    VkAndroidHardwareBufferFormatPropertiesANDROID formats = {
        .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID};
    VkAndroidHardwareBufferPropertiesANDROID properties = {
        .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID, .pNext = &formats};
    const VkResult result = properties_of(device, buffer, &properties);
    const uint32_t bytes = format == AHARDWAREBUFFER_FORMAT_R5G6B5_UNORM ? 2 : 4;
    SAY("properties %s: %d, format %s, size fits %d, some memory types %d, externalFormat set %d, sampled %d\n",
           name, result, format_name(formats.format), properties.allocationSize >= (VkDeviceSize)SIDE * SIDE * bytes,
           properties.memoryTypeBits != 0, formats.externalFormat != 0,
           (formats.formatFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0);
    AHardwareBuffer_release(buffer);
}

/* An 8 x 8 R8G8B8A8 image whose memory is an AHardwareBuffer's, with `usage`. */
static VkResult external_image(VkImageUsageFlags usage, VkImage *image) {
    const VkExternalMemoryImageCreateInfo external = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID};
    const VkImageCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &external,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = {SIDE, SIDE, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    return vkCreateImage(device, &info, NULL, image);
}

static VkCommandBuffer begin(void) {
    const VkCommandBufferAllocateInfo info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                              .commandPool = pool,
                                              .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                              .commandBufferCount = 1};
    VkCommandBuffer commands;
    check("vkAllocateCommandBuffers", vkAllocateCommandBuffers(device, &info, &commands));
    const VkCommandBufferBeginInfo begin_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                                 .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    check("vkBeginCommandBuffer", vkBeginCommandBuffer(commands, &begin_info));
    return commands;
}

static void submit(VkCommandBuffer commands) {
    check("vkEndCommandBuffer", vkEndCommandBuffer(commands));
    const VkSubmitInfo info = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1,
                               .pCommandBuffers = &commands};
    check("vkQueueSubmit", vkQueueSubmit(queue, 1, &info, VK_NULL_HANDLE));
    check("vkQueueWaitIdle", vkQueueWaitIdle(queue));
    vkFreeCommandBuffers(device, pool, 1, &commands);
}

/* A layout change of `image`, with a transfer of its ownership from or to the foreign queue family when `from` or
   `to` is VK_QUEUE_FAMILY_FOREIGN_EXT. */
static void barrier(VkCommandBuffer commands, VkImage image, VkImageLayout old_layout, VkImageLayout new_layout,
                    uint32_t from, uint32_t to, VkAccessFlags source, VkAccessFlags destination,
                    VkPipelineStageFlags after, VkPipelineStageFlags before) {
    const VkImageMemoryBarrier changed = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = source,
        .dstAccessMask = destination,
        .oldLayout = old_layout,
        .newLayout = new_layout,
        .srcQueueFamilyIndex = from,
        .dstQueueFamilyIndex = to,
        .image = image,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    vkCmdPipelineBarrier(commands, after, before, 0, 0, NULL, 0, NULL, 1, &changed);
}

/* The compute shader and what it reads and writes. */
struct sampling {
    VkSampler sampler;
    VkImageView view;
    VkBuffer buffer;
    VkDeviceMemory memory;
    uint32_t *texels;
    VkDescriptorSetLayout set_layout;
    VkPipelineLayout layout;
    VkShaderModule module;
    VkPipeline pipeline;
    VkDescriptorPool descriptors;
    VkDescriptorSet set;
};

static void sampling_make(struct sampling *made, VkImage image) {
    const VkSamplerCreateInfo sampler = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                         .magFilter = VK_FILTER_NEAREST,
                                         .minFilter = VK_FILTER_NEAREST,
                                         .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                                         .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                         .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                         .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    check("vkCreateSampler", vkCreateSampler(device, &sampler, NULL, &made->sampler));
    const VkImageViewCreateInfo view = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                        .image = image,
                                        .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                        .format = VK_FORMAT_R8G8B8A8_UNORM,
                                        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    check("vkCreateImageView", vkCreateImageView(device, &view, NULL, &made->view));
    const VkBufferCreateInfo buffer = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                       .size = SIDE * SIDE * 4,
                                       .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                       .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    check("vkCreateBuffer", vkCreateBuffer(device, &buffer, NULL, &made->buffer));
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(device, made->buffer, &requirements);
    const VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = memory_type(requirements.memoryTypeBits,
                                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    check("vkAllocateMemory", vkAllocateMemory(device, &allocation, NULL, &made->memory));
    check("vkBindBufferMemory", vkBindBufferMemory(device, made->buffer, made->memory, 0));
    check("vkMapMemory", vkMapMemory(device, made->memory, 0, VK_WHOLE_SIZE, 0, (void **)&made->texels));

    const VkDescriptorSetLayoutBinding bindings[] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
    };
    const VkDescriptorSetLayoutCreateInfo set_layout = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                        .bindingCount = 2,
                                                        .pBindings = bindings};
    check("vkCreateDescriptorSetLayout", vkCreateDescriptorSetLayout(device, &set_layout, NULL, &made->set_layout));
    const VkPipelineLayoutCreateInfo layout = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                               .setLayoutCount = 1,
                                               .pSetLayouts = &made->set_layout};
    check("vkCreatePipelineLayout", vkCreatePipelineLayout(device, &layout, NULL, &made->layout));
    static const uint32_t code[] =
#include "vk_ahb_test.comp.inc"
        ;
    const VkShaderModuleCreateInfo module = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                             .codeSize = sizeof code,
                                             .pCode = code};
    check("vkCreateShaderModule", vkCreateShaderModule(device, &module, NULL, &made->module));
    const VkComputePipelineCreateInfo pipeline = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                  .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                  .module = made->module,
                  .pName = "main"},
        .layout = made->layout};
    check("vkCreateComputePipelines",
          vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline, NULL, &made->pipeline));
    const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1},
                                          {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
    const VkDescriptorPoolCreateInfo descriptors = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                                    .maxSets = 1,
                                                    .poolSizeCount = 2,
                                                    .pPoolSizes = sizes};
    check("vkCreateDescriptorPool", vkCreateDescriptorPool(device, &descriptors, NULL, &made->descriptors));
    const VkDescriptorSetAllocateInfo set = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                             .descriptorPool = made->descriptors,
                                             .descriptorSetCount = 1,
                                             .pSetLayouts = &made->set_layout};
    check("vkAllocateDescriptorSets", vkAllocateDescriptorSets(device, &set, &made->set));
    const VkDescriptorImageInfo sampled = {made->sampler, made->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkDescriptorBufferInfo written = {made->buffer, 0, VK_WHOLE_SIZE};
    const VkWriteDescriptorSet writes[] = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = made->set, .dstBinding = 0,
         .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &sampled},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = made->set, .dstBinding = 1,
         .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &written},
    };
    vkUpdateDescriptorSets(device, 2, writes, 0, NULL);
}

static void sampling_free(struct sampling *made) {
    vkDestroyDescriptorPool(device, made->descriptors, NULL);
    vkDestroyPipeline(device, made->pipeline, NULL);
    vkDestroyShaderModule(device, made->module, NULL);
    vkDestroyPipelineLayout(device, made->layout, NULL);
    vkDestroyDescriptorSetLayout(device, made->set_layout, NULL);
    vkDestroyBuffer(device, made->buffer, NULL);
    vkFreeMemory(device, made->memory, NULL);
    vkDestroyImageView(device, made->view, NULL);
    vkDestroySampler(device, made->sampler, NULL);
}

/* Acquires the image from the foreign queue family, samples it, releases it back, and prints `rows` rows of texels as
   R, G, B, A bytes, row 0 first: the buffer's row 0. */
static void sample(const char *name, struct sampling *made, VkImage image, uint32_t rows) {
    memset(made->texels, 0, SIDE * SIDE * 4);
    VkCommandBuffer commands = begin();
    barrier(commands, image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_QUEUE_FAMILY_FOREIGN_EXT, family, 0, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, made->pipeline);
    vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, made->layout, 0, 1, &made->set, 0, NULL);
    vkCmdDispatch(commands, 1, 1, 1);
    barrier(commands, image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, family,
            VK_QUEUE_FAMILY_FOREIGN_EXT, VK_ACCESS_SHADER_READ_BIT, 0, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    const VkMemoryBarrier written = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                     .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                                     .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &written, 0,
                         NULL, 0, NULL);
    submit(commands);
    SAY("%s:\n", name);
    for (uint32_t y = 0; y < rows; y++) {
        SAY("  row %u:", y);
        for (uint32_t x = 0; x < SIDE; x++) {
            const uint32_t word = made->texels[y * SIDE + x];
            SAY(" %02x%02x%02x%02x", word & 0xff, (word >> 8) & 0xff, (word >> 16) & 0xff, word >> 24);
        }
        SAY("\n");
    }
}

/* `buffer` imported as the dedicated memory of a sampled image: prints `name` with the answers, and exits without
   a bound image. */
static void imported_image(const char *name, AHardwareBuffer *buffer, VkImage *out_image, VkDeviceMemory *out_memory) {
    VkAndroidHardwareBufferFormatPropertiesANDROID formats = {
        .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID};
    VkAndroidHardwareBufferPropertiesANDROID properties = {
        .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID, .pNext = &formats};
    check("vkGetAndroidHardwareBufferPropertiesANDROID", properties_of(device, buffer, &properties));
    VkImage image = VK_NULL_HANDLE;
    const VkResult created = external_image(VK_IMAGE_USAGE_SAMPLED_BIT, &image);
    const VkImportAndroidHardwareBufferInfoANDROID imported = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID, .buffer = buffer};
    const VkMemoryDedicatedAllocateInfo dedicated = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
                                                     .pNext = &imported,
                                                     .image = image};
    const VkMemoryAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                             .pNext = &dedicated,
                                             .allocationSize = properties.allocationSize,
                                             .memoryTypeIndex = memory_type(properties.memoryTypeBits, 0)};
    VkDeviceMemory memory = VK_NULL_HANDLE;
    const VkResult allocated_memory =
        created == VK_SUCCESS ? vkAllocateMemory(device, &allocation, NULL, &memory) : VK_ERROR_UNKNOWN;
    const VkResult bound =
        allocated_memory == VK_SUCCESS ? vkBindImageMemory(device, image, memory, 0) : VK_ERROR_UNKNOWN;
    SAY("%s: create image %d, allocate %d, bind %d\n", name, created, allocated_memory, bound);
    if (bound != VK_SUCCESS) {
        exit(1);
    }
    *out_image = image;
    *out_memory = memory;
}

static void import(void) {
    const uint64_t buffer_usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN;
    AHardwareBuffer *buffer = allocated(AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM, buffer_usage);
    if (buffer == NULL || fill(buffer) != 0) {
        SAY("import: no buffer\n");
        exit(1);
    }
    VkImage image;
    VkDeviceMemory memory;
    imported_image("import", buffer, &image, &memory);
    // The buffer is the memory's: the app's release leaves it to the memory.
    AHardwareBuffer_release(buffer);

    struct sampling made;
    sampling_make(&made, image);
    sample("sampled", &made, image, SIDE);
    // A write of the CPU while the foreign queue family owns the image: texel (0, 0) becomes 0x11223344.
    void *address = NULL;
    if (AHardwareBuffer_lock(buffer, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, NULL, &address) == 0) {
        memcpy(address, "\x11\x22\x33\x44", 4);
        AHardwareBuffer_unlock(buffer, NULL);
    } else {
        SAY("no lock\n");
    }
    sample("after the CPU's write", &made, image, 1);
    sampling_free(&made);
    vkFreeMemory(device, memory, NULL);
    vkDestroyImage(device, image, NULL);
}

static void export(void) {
    VkImage image = VK_NULL_HANDLE;
    const VkResult created =
        external_image(VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, &image);
    VkMemoryRequirements requirements = {0};
    if (created == VK_SUCCESS) {
        vkGetImageMemoryRequirements(device, image, &requirements);
    }
    const VkExportMemoryAllocateInfo exported = {
        .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID};
    const VkMemoryDedicatedAllocateInfo dedicated = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
                                                     .pNext = &exported,
                                                     .image = image};
    // An export of an image's dedicated memory as an AHardwareBuffer has no size of its own.
    const VkMemoryAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                             .pNext = &dedicated,
                                             .allocationSize = 0,
                                             .memoryTypeIndex = memory_type(requirements.memoryTypeBits, 0)};
    VkDeviceMemory memory = VK_NULL_HANDLE;
    const VkResult allocated_memory =
        created == VK_SUCCESS ? vkAllocateMemory(device, &allocation, NULL, &memory) : VK_ERROR_UNKNOWN;
    const VkResult bound =
        allocated_memory == VK_SUCCESS ? vkBindImageMemory(device, image, memory, 0) : VK_ERROR_UNKNOWN;
    const VkMemoryGetAndroidHardwareBufferInfoANDROID info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_GET_ANDROID_HARDWARE_BUFFER_INFO_ANDROID, .memory = memory};
    AHardwareBuffer *buffer = NULL, *again = NULL;
    const VkResult got = bound == VK_SUCCESS ? buffer_of(device, &info, &buffer) : VK_ERROR_UNKNOWN;
    const VkResult got_again = got == VK_SUCCESS ? buffer_of(device, &info, &again) : VK_ERROR_UNKNOWN;
    SAY("export: create image %d, allocate %d, bind %d, get %d, a buffer %d, the same again %d\n", created,
           allocated_memory, bound, got, buffer != NULL, got_again == VK_SUCCESS && again == buffer);
    if (got != VK_SUCCESS || buffer == NULL) {
        exit(1);
    }
    AHardwareBuffer_Desc description;
    AHardwareBuffer_describe(buffer, &description);
    SAY("exported: %u x %u, %u layer, format %u, sampled %d\n", description.width, description.height,
           description.layers, description.format,
           (description.usage & AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE) != 0);

    // A clear of the GPU, released to the foreign queue family, then read by the CPU.
    VkCommandBuffer commands = begin();
    barrier(commands, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED,
            VK_QUEUE_FAMILY_IGNORED, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT);
    const VkClearColorValue color = {.float32 = {0x10 / 255.0f, 0x20 / 255.0f, 0x30 / 255.0f, 0x40 / 255.0f}};
    const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(commands, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
    barrier(commands, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, family,
            VK_QUEUE_FAMILY_FOREIGN_EXT, VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    submit(commands);
    // The exported buffer imported into a second image, which samples the clear: the buffer is the memory. (A lock
    // of the CPU is gralloc's: the emulator's refuses a buffer without a CPU usage with -38.)
    VkImage again_image;
    VkDeviceMemory again_memory;
    imported_image("the exported buffer imported", buffer, &again_image, &again_memory);
    struct sampling made;
    sampling_make(&made, again_image);
    sample("the clear, sampled", &made, again_image, SIDE);
    sampling_free(&made);
    vkFreeMemory(device, again_memory, NULL);
    vkDestroyImage(device, again_image, NULL);
    // Each vkGetMemoryAndroidHardwareBufferANDROID gave a reference of the app's; the memory keeps its own.
    AHardwareBuffer_release(buffer);
    if (again != NULL) {
        AHardwareBuffer_release(again);
    }
    AHardwareBuffer_Desc kept;
    memset(&kept, 0, sizeof kept);
    AHardwareBuffer_describe(buffer, &kept);
    SAY("after the app's releases: %u x %u\n", kept.width, kept.height);
    vkFreeMemory(device, memory, NULL);
    vkDestroyImage(device, image, NULL);
}

int VK_AHB_MAIN(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    const VkApplicationInfo application = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                           .pApplicationName = "vk-ahb-test",
                                           .apiVersion = VK_API_VERSION_1_1};
    const VkInstanceCreateInfo instance_info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                                .pApplicationInfo = &application};
    VkInstance instance;
    check("vkCreateInstance", vkCreateInstance(&instance_info, NULL, &instance));
    uint32_t count = 1;
    VkResult result = vkEnumeratePhysicalDevices(instance, &count, &physical);
    if ((result != VK_SUCCESS && result != VK_INCOMPLETE) || count == 0) {
        SAY("no physical device: %d\n", result);
        return 1;
    }
    count = 0;
    check("vkEnumerateDeviceExtensionProperties", vkEnumerateDeviceExtensionProperties(physical, NULL, &count, NULL));
    VkExtensionProperties *extensions = calloc(count, sizeof *extensions);
    check("vkEnumerateDeviceExtensionProperties",
          vkEnumerateDeviceExtensionProperties(physical, NULL, &count, extensions));
    const int hardware_buffer =
        offered(extensions, count, VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME);
    const int foreign = offered(extensions, count, VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME);
    free(extensions);
    SAY("%s: %d\n", VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME, hardware_buffer);
    SAY("%s: %d\n", VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME, foreign);
    if (!hardware_buffer || !foreign) {
        return 1;
    }

    // The format properties of an image for the handle type.
    VkAndroidHardwareBufferUsageANDROID usage = {.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_USAGE_ANDROID};
    VkExternalImageFormatProperties external = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES,
                                                .pNext = &usage};
    VkImageFormatProperties2 format_properties = {.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
                                                  .pNext = &external};
    const VkPhysicalDeviceExternalImageFormatInfo handle = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID};
    const VkPhysicalDeviceImageFormatInfo2 format_info = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
        .pNext = &handle,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .type = VK_IMAGE_TYPE_2D,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT};
    result = vkGetPhysicalDeviceImageFormatProperties2(physical, &format_info, &format_properties);
    const VkExternalMemoryFeatureFlags features = external.externalMemoryProperties.externalMemoryFeatures;
    SAY("image format properties: %d, dedicated only %d, exportable %d, importable %d, usage sampled %d\n", result,
           (features & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT) != 0,
           (features & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) != 0,
           (features & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) != 0,
           (usage.androidHardwareBufferUsage & AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE) != 0);

    count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, NULL);
    VkQueueFamilyProperties *families = calloc(count, sizeof *families);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families);
    family = UINT32_MAX;
    for (uint32_t index = 0; index < count && family == UINT32_MAX; index++) {
        if ((families[index].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0) {
            family = index;
        }
    }
    free(families);
    const float priority = 1.0f;
    const VkDeviceQueueCreateInfo queue_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                                .queueFamilyIndex = family,
                                                .queueCount = 1,
                                                .pQueuePriorities = &priority};
    const char *const enabled[] = {VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME,
                                   VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME};
    const VkDeviceCreateInfo device_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                            .queueCreateInfoCount = 1,
                                            .pQueueCreateInfos = &queue_info,
                                            .enabledExtensionCount = 2,
                                            .ppEnabledExtensionNames = enabled};
    check("vkCreateDevice", vkCreateDevice(physical, &device_info, NULL, &device));
    vkGetDeviceQueue(device, family, 0, &queue);
    properties_of = (PFN_vkGetAndroidHardwareBufferPropertiesANDROID)vkGetDeviceProcAddr(
        device, "vkGetAndroidHardwareBufferPropertiesANDROID");
    buffer_of = (PFN_vkGetMemoryAndroidHardwareBufferANDROID)vkGetDeviceProcAddr(
        device, "vkGetMemoryAndroidHardwareBufferANDROID");
    const VkCommandPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                               .queueFamilyIndex = family};
    check("vkCreateCommandPool", vkCreateCommandPool(device, &pool_info, NULL, &pool));

    print_properties("R8G8B8A8_UNORM", AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM);
    print_properties("R8G8B8X8_UNORM", AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM);
    print_properties("R5G6B5_UNORM", AHARDWAREBUFFER_FORMAT_R5G6B5_UNORM);
    import();
    export();

    vkDestroyCommandPool(device, pool, NULL);
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);
    SAY("done\n");
    return 0;
}
