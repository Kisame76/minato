/* vulkan-indexing-test: descriptor indexing on the Vulkan front over MoltenVK, with and without Metal argument
   buffers (graphics.vulkan_indexing and graphics.vulkan_indexing_plain_buffers; the ES 3.1 path's speed). An 8 x 1
   target is drawn twice from 8 textures (one layer each of an 8-layer image, texel colour of layer i fixed):
   - indexed: one draw whose pixel x samples textures[nonuniformEXT(x)] of a runtime array, in a set of a variable
     count, partially bound, bound before its descriptors are written (update after bind);
   - plain: a fixed array of 8, one draw per pixel with a scissor of that pixel and the index as a push constant.
   It prints the features it asks for and both rows of pixels; both must be the 8 colours in order. graphics-run's
   Metal hook (MINATO_METAL_HOOK=fences) adds the render encoders' fence waits at the end. */
#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The shaders, SPIR-V from the NDK's glslc as C initializers (graphics/guest/CMakeLists.txt). */
static const uint32_t vulkan_indexing_vertex[] =
#include "vulkan_indexing_test.vert.inc"
    ;
static const uint32_t vulkan_indexing_fragment[] =
#include "vulkan_indexing_test.frag.inc"
    ;
static const uint32_t vulkan_indexing_plain[] =
#include "vulkan_indexing_plain.frag.inc"
    ;

enum { COUNT = 8 };

static VkDevice device;
static VkPhysicalDevice physical;

#define CHECK(call)                                                       \
    do {                                                                  \
        const VkResult result_ = (call);                                  \
        if (result_ != VK_SUCCESS) {                                      \
            printf("%s failed: %d\n", #call, (int)result_);               \
            exit(1);                                                      \
        }                                                                 \
    } while (0)

static void colour(uint32_t i, uint8_t out[4]) {
    out[0] = (uint8_t)(20 + 30 * i), out[1] = (uint8_t)(200 - 20 * i), out[2] = (uint8_t)((53 * i) % 256);
    out[3] = 255;
}

static uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags wanted) {
    VkPhysicalDeviceMemoryProperties memory;
    vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    for (uint32_t i = 0; i < memory.memoryTypeCount; i++) {
        if ((bits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & wanted) == wanted) return i;
    }
    printf("no memory type\n");
    exit(1);
}

static VkDeviceMemory bind_image(VkImage image) {
    VkMemoryRequirements needs;
    vkGetImageMemoryRequirements(device, image, &needs);
    const VkMemoryAllocateInfo info = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, needs.size,
                                       memory_type(needs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    VkDeviceMemory memory;
    CHECK(vkAllocateMemory(device, &info, NULL, &memory));
    CHECK(vkBindImageMemory(device, image, memory, 0));
    return memory;
}

static VkBuffer host_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkDeviceMemory *memory, void **mapped) {
    const VkBufferCreateInfo info = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, size, usage,
                                     VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    VkBuffer buffer;
    CHECK(vkCreateBuffer(device, &info, NULL, &buffer));
    VkMemoryRequirements needs;
    vkGetBufferMemoryRequirements(device, buffer, &needs);
    const VkMemoryAllocateInfo allocate = {
        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, needs.size,
        memory_type(needs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    CHECK(vkAllocateMemory(device, &allocate, NULL, memory));
    CHECK(vkBindBufferMemory(device, buffer, *memory, 0));
    CHECK(vkMapMemory(device, *memory, 0, size, 0, mapped));
    return buffer;
}

static void barrier(VkCommandBuffer commands, VkImage image, uint32_t layers, VkImageLayout from, VkImageLayout to,
                    VkAccessFlags before, VkAccessFlags after, VkPipelineStageFlags source,
                    VkPipelineStageFlags destination) {
    const VkImageMemoryBarrier one = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, before, after, from, to,
                                      VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, image,
                                      {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers}};
    vkCmdPipelineBarrier(commands, source, destination, 0, 0, NULL, 0, NULL, 1, &one);
}

static VkShaderModule module(const uint32_t *code, size_t size) {
    const VkShaderModuleCreateInfo info = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, size, code};
    VkShaderModule made;
    CHECK(vkCreateShaderModule(device, &info, NULL, &made));
    return made;
}

static VkPipeline pipeline(VkPipelineLayout layout, VkRenderPass pass, VkShaderModule vertex, VkShaderModule fragment) {
    const VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vertex, "main",
         NULL},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fragment, "main",
         NULL},
    };
    const VkPipelineVertexInputStateCreateInfo input = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    const VkPipelineInputAssemblyStateCreateInfo assembly = {
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0};
    const VkPipelineViewportStateCreateInfo viewport = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                                        NULL, 0, 1, NULL, 1, NULL};
    const VkPipelineRasterizationStateCreateInfo raster = {
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, NULL, 0, 0, 0, VK_POLYGON_MODE_FILL,
        VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE, 0, 0, 0, 0, 1.0f};
    const VkPipelineMultisampleStateCreateInfo multisample = {
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0, VK_SAMPLE_COUNT_1_BIT};
    const VkPipelineColorBlendAttachmentState attachment = {.colorWriteMask = 0xf};
    const VkPipelineColorBlendStateCreateInfo blend = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                                       NULL, 0, 0, VK_LOGIC_OP_COPY, 1, &attachment};
    const VkDynamicState dynamic_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    const VkPipelineDynamicStateCreateInfo dynamic = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0,
                                                      2, dynamic_states};
    const VkGraphicsPipelineCreateInfo info = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                               NULL,
                                               0,
                                               2,
                                               stages,
                                               &input,
                                               &assembly,
                                               NULL,
                                               &viewport,
                                               &raster,
                                               &multisample,
                                               NULL,
                                               &blend,
                                               &dynamic,
                                               layout,
                                               pass,
                                               0,
                                               VK_NULL_HANDLE,
                                               -1};
    VkPipeline made;
    CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, NULL, &made));
    return made;
}

static void print_row(const char *name, const uint8_t *pixels) {
    printf("%s:", name);
    int right = 1;
    for (uint32_t i = 0; i < COUNT; i++) {
        uint8_t want[4];
        colour(i, want);
        right = right && memcmp(pixels + 4 * i, want, 4) == 0;
        printf(" %02x%02x%02x%02x", pixels[4 * i], pixels[4 * i + 1], pixels[4 * i + 2], pixels[4 * i + 3]);
    }
    printf("; the 8 colours in order: %s\n", right ? "yes" : "no");
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vulkan-indexing-test", 1, NULL, 0,
                                   VK_API_VERSION_1_2};
    const VkInstanceCreateInfo instance_info = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app};
    VkInstance instance;
    CHECK(vkCreateInstance(&instance_info, NULL, &instance));
    uint32_t count = 1;
    CHECK(vkEnumeratePhysicalDevices(instance, &count, &physical));
    VkPhysicalDeviceVulkan12Features offered = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 features = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &offered};
    vkGetPhysicalDeviceFeatures2(physical, &features);
    const int indexing = offered.descriptorIndexing && offered.runtimeDescriptorArray &&
                         offered.shaderSampledImageArrayNonUniformIndexing && offered.descriptorBindingPartiallyBound &&
                         offered.descriptorBindingVariableDescriptorCount &&
                         offered.descriptorBindingSampledImageUpdateAfterBind;
    printf("descriptor indexing offered: %s\n", indexing ? "yes" : "no");
    if (!indexing) return 1;

    uint32_t family = 0, families = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &families, NULL);
    VkQueueFamilyProperties family_list[8];
    families = families > 8 ? 8 : families;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &families, family_list);
    while (family < families && !(family_list[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) family++;
    const float priority = 1.0f;
    const VkDeviceQueueCreateInfo queue_info = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, family, 1,
                                                &priority};
    VkPhysicalDeviceVulkan12Features asked = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    asked.descriptorIndexing = asked.runtimeDescriptorArray = asked.shaderSampledImageArrayNonUniformIndexing = 1;
    asked.descriptorBindingPartiallyBound = asked.descriptorBindingVariableDescriptorCount = 1;
    asked.descriptorBindingSampledImageUpdateAfterBind = 1;
    const VkDeviceCreateInfo device_info = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &asked, 0, 1, &queue_info};
    CHECK(vkCreateDevice(physical, &device_info, NULL, &device));
    VkQueue queue;
    vkGetDeviceQueue(device, family, 0, &queue);
    printf("a device with descriptor indexing, update after bind and variable counts\n");

    // The 8 textures: one layer each of an 8-layer 1 x 1 image, filled from a buffer.
    const VkImageCreateInfo layers_info = {
        VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {1, 1, 1}, 1, COUNT,
        VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL,
        VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage layers;
    CHECK(vkCreateImage(device, &layers_info, NULL, &layers));
    bind_image(layers);
    VkImageView views[COUNT];
    for (uint32_t i = 0; i < COUNT; i++) {
        const VkImageViewCreateInfo view = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, layers,
                                            VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {0},
                                            {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, i, 1}};
        CHECK(vkCreateImageView(device, &view, NULL, &views[i]));
    }
    const VkSamplerCreateInfo sampler_info = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, NULL, 0, VK_FILTER_NEAREST,
                                              VK_FILTER_NEAREST};
    VkSampler sampler;
    CHECK(vkCreateSampler(device, &sampler_info, NULL, &sampler));

    // The target, its pass and its framebuffer.
    const VkImageCreateInfo target_info = {
        VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {COUNT, 1, 1}, 1, 1,
        VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL,
        VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage target;
    CHECK(vkCreateImage(device, &target_info, NULL, &target));
    bind_image(target);
    const VkImageViewCreateInfo target_view_info = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, target,
                                                    VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {0},
                                                    {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkImageView target_view;
    CHECK(vkCreateImageView(device, &target_view_info, NULL, &target_view));
    const VkAttachmentDescription attachment = {0, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT,
                                                VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
                                                VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
    const VkAttachmentReference reference = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    const VkSubpassDescription subpass = {0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, NULL, 1, &reference};
    const VkRenderPassCreateInfo pass_info = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, NULL, 0, 1, &attachment, 1,
                                              &subpass};
    VkRenderPass pass;
    CHECK(vkCreateRenderPass(device, &pass_info, NULL, &pass));
    const VkFramebufferCreateInfo framebuffer_info = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, NULL, 0, pass, 1,
                                                      &target_view, COUNT, 1, 1};
    VkFramebuffer framebuffer;
    CHECK(vkCreateFramebuffer(device, &framebuffer_info, NULL, &framebuffer));

    // The indexed set: a runtime array of a variable count, partially bound, updated after it is bound.
    const VkDescriptorBindingFlags indexed_flags = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                                                   VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT |
                                                   VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
    const VkDescriptorSetLayoutBindingFlagsCreateInfo flags_info = {
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO, NULL, 1, &indexed_flags};
    const VkDescriptorSetLayoutBinding indexed_binding = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, COUNT,
                                                          VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
    const VkDescriptorSetLayoutCreateInfo indexed_layout_info = {
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, &flags_info,
        VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT, 1, &indexed_binding};
    VkDescriptorSetLayout indexed_layout;
    CHECK(vkCreateDescriptorSetLayout(device, &indexed_layout_info, NULL, &indexed_layout));
    const VkDescriptorSetLayoutBinding plain_binding = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, COUNT,
                                                        VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
    const VkDescriptorSetLayoutCreateInfo plain_layout_info = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                               NULL, 0, 1, &plain_binding};
    VkDescriptorSetLayout plain_layout;
    CHECK(vkCreateDescriptorSetLayout(device, &plain_layout_info, NULL, &plain_layout));
    const VkDescriptorPoolSize sizes = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, COUNT};
    const VkDescriptorPoolCreateInfo indexed_pool_info = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL,
                                                          VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT, 1, 1,
                                                          &sizes};
    const VkDescriptorPoolCreateInfo plain_pool_info = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 1, 1,
                                                        &sizes};
    VkDescriptorPool indexed_pool, plain_pool;
    CHECK(vkCreateDescriptorPool(device, &indexed_pool_info, NULL, &indexed_pool));
    CHECK(vkCreateDescriptorPool(device, &plain_pool_info, NULL, &plain_pool));
    const uint32_t variable = COUNT;
    const VkDescriptorSetVariableDescriptorCountAllocateInfo variable_info = {
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO, NULL, 1, &variable};
    const VkDescriptorSetAllocateInfo indexed_allocate = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                                          &variable_info, indexed_pool, 1, &indexed_layout};
    const VkDescriptorSetAllocateInfo plain_allocate = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL,
                                                        plain_pool, 1, &plain_layout};
    VkDescriptorSet indexed_set, plain_set;
    CHECK(vkAllocateDescriptorSets(device, &indexed_allocate, &indexed_set));
    CHECK(vkAllocateDescriptorSets(device, &plain_allocate, &plain_set));
    VkDescriptorImageInfo images[COUNT];
    for (uint32_t i = 0; i < COUNT; i++) {
        images[i] = (VkDescriptorImageInfo){sampler, views[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    }
    const VkWriteDescriptorSet plain_write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, plain_set, 0, 0, COUNT,
                                              VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, images};
    vkUpdateDescriptorSets(device, 1, &plain_write, 0, NULL);

    const VkPipelineLayoutCreateInfo indexed_pipeline_layout_info = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                                                     NULL, 0, 1, &indexed_layout};
    const VkPushConstantRange push = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4};
    const VkPipelineLayoutCreateInfo plain_pipeline_layout_info = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                                                   NULL, 0, 1, &plain_layout, 1, &push};
    VkPipelineLayout indexed_pipeline_layout, plain_pipeline_layout;
    CHECK(vkCreatePipelineLayout(device, &indexed_pipeline_layout_info, NULL, &indexed_pipeline_layout));
    CHECK(vkCreatePipelineLayout(device, &plain_pipeline_layout_info, NULL, &plain_pipeline_layout));
    const VkShaderModule vertex = module(vulkan_indexing_vertex, sizeof vulkan_indexing_vertex);
    const VkPipeline indexed_pipeline = pipeline(indexed_pipeline_layout, pass, vertex,
                                                 module(vulkan_indexing_fragment, sizeof vulkan_indexing_fragment));
    const VkPipeline plain_pipeline = pipeline(plain_pipeline_layout, pass, vertex,
                                               module(vulkan_indexing_plain, sizeof vulkan_indexing_plain));

    // The commands: the upload, the indexed pass, its readback, the plain pass, its readback.
    VkDeviceMemory upload_memory, read_memory;
    void *upload_bits, *read_bits;
    const VkBuffer upload = host_buffer(4 * COUNT, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &upload_memory, &upload_bits);
    const VkBuffer read = host_buffer(2 * 4 * COUNT, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &read_memory, &read_bits);
    for (uint32_t i = 0; i < COUNT; i++) colour(i, (uint8_t *)upload_bits + 4 * i);
    memset(read_bits, 0, 2 * 4 * COUNT);
    const VkCommandPoolCreateInfo pool_info = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, family};
    VkCommandPool pool;
    CHECK(vkCreateCommandPool(device, &pool_info, NULL, &pool));
    const VkCommandBufferAllocateInfo buffer_info = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool,
                                                     VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer commands;
    CHECK(vkAllocateCommandBuffers(device, &buffer_info, &commands));
    const VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
                                            VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    CHECK(vkBeginCommandBuffer(commands, &begin));
    barrier(commands, layers, COUNT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy copies[COUNT];
    for (uint32_t i = 0; i < COUNT; i++) {
        copies[i] = (VkBufferImageCopy){4 * i, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, i, 1}, {0, 0, 0}, {1, 1, 1}};
    }
    vkCmdCopyBufferToImage(commands, upload, layers, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, COUNT, copies);
    barrier(commands, layers, COUNT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    const VkClearValue clear = {{{0, 0, 0, 0}}};
    const VkRenderPassBeginInfo pass_begin = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL, pass, framebuffer,
                                              {{0, 0}, {COUNT, 1}}, 1, &clear};
    const VkViewport whole = {0, 0, COUNT, 1, 0, 1};
    const VkRect2D all = {{0, 0}, {COUNT, 1}};
    vkCmdBeginRenderPass(commands, &pass_begin, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, indexed_pipeline);
    vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, indexed_pipeline_layout, 0, 1, &indexed_set,
                            0, NULL);
    vkCmdSetViewport(commands, 0, 1, &whole);
    vkCmdSetScissor(commands, 0, 1, &all);
    vkCmdDraw(commands, 3, 1, 0, 0);
    vkCmdEndRenderPass(commands);
    const VkBufferImageCopy row = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {COUNT, 1, 1}};
    vkCmdCopyImageToBuffer(commands, target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, read, 1, &row);
    vkCmdBeginRenderPass(commands, &pass_begin, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, plain_pipeline);
    vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, plain_pipeline_layout, 0, 1, &plain_set, 0,
                            NULL);
    vkCmdSetViewport(commands, 0, 1, &whole);
    for (int32_t i = 0; i < COUNT; i++) {
        const VkRect2D pixel = {{i, 0}, {1, 1}};
        vkCmdSetScissor(commands, 0, 1, &pixel);
        vkCmdPushConstants(commands, plain_pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4, &i);
        vkCmdDraw(commands, 3, 1, 0, 0);
    }
    vkCmdEndRenderPass(commands);
    const VkBufferImageCopy second_row = {4 * COUNT, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0},
                                          {COUNT, 1, 1}};
    vkCmdCopyImageToBuffer(commands, target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, read, 1, &second_row);
    CHECK(vkEndCommandBuffer(commands));
    // Update after bind: the indexed set's descriptors are written only now, after the commands that bind it.
    const VkWriteDescriptorSet indexed_write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, indexed_set, 0, 0,
                                                COUNT, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, images};
    vkUpdateDescriptorSets(device, 1, &indexed_write, 0, NULL);
    const VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &commands};
    fprintf(stderr, "--- submit\n");
    CHECK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
    CHECK(vkQueueWaitIdle(queue));
    fprintf(stderr, "--- done\n");
    print_row("indexed", (const uint8_t *)read_bits);
    print_row("plain", (const uint8_t *)read_bits + 4 * COUNT);
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);
    return 0;
}
