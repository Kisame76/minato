// Test 5 of ADR-0030: MoltenVK as released, on the Mac. A host program without guest code opens
// build/moltenvk/macos/libMoltenVK.dylib with dlopen, as graphics/host will, creates an instance and a
// device, draws the left half of a 64 x 64 R8G8B8A8 image red on blue and reads it back.
//
// The two shaders were compiled once with the NDK's glslc (shaderc v2022.3):
//   glslc --target-env=vulkan1.0 -O -o left_half.vert.spv left_half.vert
//   glslc --target-env=vulkan1.0 -O -o red.frag.spv red.frag
// left_half.vert:
//   #version 450
//   const vec2 corners[6] = vec2[](vec2(-1.0, -1.0), vec2(0.0, -1.0), vec2(-1.0, 1.0),
//                                  vec2(-1.0, 1.0), vec2(0.0, -1.0), vec2(0.0, 1.0));
//   void main() { gl_Position = vec4(corners[gl_VertexIndex], 0.0, 1.0); }
// red.frag:
//   #version 450
//   layout(location = 0) out vec4 color;
//   void main() { color = vec4(1.0, 0.0, 0.0, 1.0); }
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

const uint32_t kLeftHalfVertex[] = {
    0x07230203, 0x00010000, 0x000d000a, 0x00000028, 0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x0007000f, 0x00000000, 0x00000004, 0x6e69616d, 0x00000000, 0x0000000d, 0x0000001c, 0x00050048,
    0x0000000b, 0x00000000, 0x0000000b, 0x00000000, 0x00050048, 0x0000000b, 0x00000001, 0x0000000b,
    0x00000001, 0x00050048, 0x0000000b, 0x00000002, 0x0000000b, 0x00000003, 0x00050048, 0x0000000b,
    0x00000003, 0x0000000b, 0x00000004, 0x00030047, 0x0000000b, 0x00000002, 0x00040047, 0x0000001c,
    0x0000000b, 0x0000002a, 0x00020013, 0x00000002, 0x00030021, 0x00000003, 0x00000002, 0x00030016,
    0x00000006, 0x00000020, 0x00040017, 0x00000007, 0x00000006, 0x00000004, 0x00040015, 0x00000008,
    0x00000020, 0x00000000, 0x0004002b, 0x00000008, 0x00000009, 0x00000001, 0x0004001c, 0x0000000a,
    0x00000006, 0x00000009, 0x0006001e, 0x0000000b, 0x00000007, 0x00000006, 0x0000000a, 0x0000000a,
    0x00040020, 0x0000000c, 0x00000003, 0x0000000b, 0x0004003b, 0x0000000c, 0x0000000d, 0x00000003,
    0x00040015, 0x0000000e, 0x00000020, 0x00000001, 0x0004002b, 0x0000000e, 0x0000000f, 0x00000000,
    0x00040017, 0x00000010, 0x00000006, 0x00000002, 0x0004002b, 0x00000008, 0x00000011, 0x00000006,
    0x0004001c, 0x00000012, 0x00000010, 0x00000011, 0x0004002b, 0x00000006, 0x00000013, 0xbf800000,
    0x0005002c, 0x00000010, 0x00000014, 0x00000013, 0x00000013, 0x0004002b, 0x00000006, 0x00000015,
    0x00000000, 0x0005002c, 0x00000010, 0x00000016, 0x00000015, 0x00000013, 0x0004002b, 0x00000006,
    0x00000017, 0x3f800000, 0x0005002c, 0x00000010, 0x00000018, 0x00000013, 0x00000017, 0x0005002c,
    0x00000010, 0x00000019, 0x00000015, 0x00000017, 0x0009002c, 0x00000012, 0x0000001a, 0x00000014,
    0x00000016, 0x00000018, 0x00000018, 0x00000016, 0x00000019, 0x00040020, 0x0000001b, 0x00000001,
    0x0000000e, 0x0004003b, 0x0000001b, 0x0000001c, 0x00000001, 0x00040020, 0x0000001e, 0x00000007,
    0x00000012, 0x00040020, 0x00000020, 0x00000007, 0x00000010, 0x00040020, 0x00000026, 0x00000003,
    0x00000007, 0x00050036, 0x00000002, 0x00000004, 0x00000000, 0x00000003, 0x000200f8, 0x00000005,
    0x0004003b, 0x0000001e, 0x0000001f, 0x00000007, 0x0004003d, 0x0000000e, 0x0000001d, 0x0000001c,
    0x0003003e, 0x0000001f, 0x0000001a, 0x00050041, 0x00000020, 0x00000021, 0x0000001f, 0x0000001d,
    0x0004003d, 0x00000010, 0x00000022, 0x00000021, 0x00050051, 0x00000006, 0x00000023, 0x00000022,
    0x00000000, 0x00050051, 0x00000006, 0x00000024, 0x00000022, 0x00000001, 0x00070050, 0x00000007,
    0x00000025, 0x00000023, 0x00000024, 0x00000015, 0x00000017, 0x00050041, 0x00000026, 0x00000027,
    0x0000000d, 0x0000000f, 0x0003003e, 0x00000027, 0x00000025, 0x000100fd, 0x00010038,
};

const uint32_t kRedFragment[] = {
    0x07230203, 0x00010000, 0x000d000a, 0x0000000d, 0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x0006000f, 0x00000004, 0x00000004, 0x6e69616d, 0x00000000, 0x00000009, 0x00030010, 0x00000004,
    0x00000007, 0x00040047, 0x00000009, 0x0000001e, 0x00000000, 0x00020013, 0x00000002, 0x00030021,
    0x00000003, 0x00000002, 0x00030016, 0x00000006, 0x00000020, 0x00040017, 0x00000007, 0x00000006,
    0x00000004, 0x00040020, 0x00000008, 0x00000003, 0x00000007, 0x0004003b, 0x00000008, 0x00000009,
    0x00000003, 0x0004002b, 0x00000006, 0x0000000a, 0x3f800000, 0x0004002b, 0x00000006, 0x0000000b,
    0x00000000, 0x0007002c, 0x00000007, 0x0000000c, 0x0000000a, 0x0000000b, 0x0000000b, 0x0000000a,
    0x00050036, 0x00000002, 0x00000004, 0x00000000, 0x00000003, 0x000200f8, 0x00000005, 0x0003003e,
    0x00000009, 0x0000000c, 0x000100fd, 0x00010038,
};

constexpr uint32_t kSize = 64;

[[noreturn]] void fail(const std::string &what) {
    std::fprintf(stderr, "moltenvk-smoke: %s\n", what.c_str());
    std::exit(1);
}

void check(VkResult result, const char *what) {
    if (result != VK_SUCCESS) {
        fail(std::string(what) + " failed: " + std::to_string(result));
    }
}

PFN_vkGetInstanceProcAddr get_instance_proc = nullptr;
PFN_vkGetDeviceProcAddr get_device_proc = nullptr;
VkInstance instance = VK_NULL_HANDLE;
VkDevice device = VK_NULL_HANDLE;

template <typename Function>
Function instance_function(const char *name) {
    auto function = reinterpret_cast<Function>(get_instance_proc(instance, name));
    if (function == nullptr) {
        fail(std::string("MoltenVK has no ") + name);
    }
    return function;
}

template <typename Function>
Function device_function(const char *name) {
    auto function = reinterpret_cast<Function>(get_device_proc(device, name));
    if (function == nullptr) {
        fail(std::string("MoltenVK has no ") + name + " for the device");
    }
    return function;
}

#define VKI(name) instance_function<PFN_vk##name>("vk" #name)
#define VKD(name) device_function<PFN_vk##name>("vk" #name)

uint32_t memory_type(const VkPhysicalDeviceMemoryProperties &properties, uint32_t allowed,
                     VkMemoryPropertyFlags wanted) {
    for (uint32_t index = 0; index < properties.memoryTypeCount; ++index) {
        if ((allowed & (1u << index)) != 0 && (properties.memoryTypes[index].propertyFlags & wanted) == wanted) {
            return index;
        }
    }
    fail("no memory type with the flags " + std::to_string(wanted));
}

}  // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        fail("usage: moltenvk-smoke <libMoltenVK.dylib>");
    }
    if (access(argv[1], F_OK) != 0) {
        std::printf("skipped: %s is not fetched, see tools/fetch-moltenvk.sh\n", argv[1]);
        return 0;
    }
    void *library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (library == nullptr) {
        fail(std::string("dlopen: ") + dlerror());
    }
    get_instance_proc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(library, "vkGetInstanceProcAddr"));
    if (get_instance_proc == nullptr) {
        fail("libMoltenVK does not export vkGetInstanceProcAddr");
    }

    VkApplicationInfo application = {};
    application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application.pApplicationName = "moltenvk-smoke";
    application.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo instance_info = {};
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.pApplicationInfo = &application;
    check(VKI(CreateInstance)(&instance_info, nullptr, &instance), "vkCreateInstance");

    uint32_t count = 0;
    VKI(EnumeratePhysicalDevices)(instance, &count, nullptr);
    if (count == 0) {
        fail("no physical device");
    }
    std::vector<VkPhysicalDevice> physical_devices(count);
    check(VKI(EnumeratePhysicalDevices)(instance, &count, physical_devices.data()), "vkEnumeratePhysicalDevices");
    VkPhysicalDevice physical = physical_devices[0];
    VkPhysicalDeviceProperties properties = {};
    VKI(GetPhysicalDeviceProperties)(physical, &properties);

    VKI(GetPhysicalDeviceQueueFamilyProperties)(physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    VKI(GetPhysicalDeviceQueueFamilyProperties)(physical, &count, families.data());
    uint32_t family = count;
    for (uint32_t index = 0; index < count; ++index) {
        if ((families[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
            family = index;
            break;
        }
    }
    if (family == count) {
        fail("no graphics queue family");
    }

    // The specification asks an app to enable VK_KHR_portability_subset where the device offers it.
    VKI(EnumerateDeviceExtensionProperties)(physical, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> extensions(count);
    VKI(EnumerateDeviceExtensionProperties)(physical, nullptr, &count, extensions.data());
    std::vector<const char *> enabled;
    for (const VkExtensionProperties &extension : extensions) {
        if (std::strcmp(extension.extensionName, "VK_KHR_portability_subset") == 0) {
            enabled.push_back("VK_KHR_portability_subset");
        }
    }
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = {};
    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;
    VkDeviceCreateInfo device_info = {};
    device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    device_info.enabledExtensionCount = static_cast<uint32_t>(enabled.size());
    device_info.ppEnabledExtensionNames = enabled.data();
    check(VKI(CreateDevice)(physical, &device_info, nullptr, &device), "vkCreateDevice");
    get_device_proc = VKI(GetDeviceProcAddr);
    VkQueue queue = VK_NULL_HANDLE;
    VKD(GetDeviceQueue)(device, family, 0, &queue);
    VkPhysicalDeviceMemoryProperties memory = {};
    VKI(GetPhysicalDeviceMemoryProperties)(physical, &memory);

    // The target: 64 x 64 R8G8B8A8, cleared blue, its left half drawn red.
    const VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    VkImageCreateInfo image_info = {};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = format;
    image_info.extent = {kSize, kSize, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage image = VK_NULL_HANDLE;
    check(VKD(CreateImage)(device, &image_info, nullptr, &image), "vkCreateImage");
    VkMemoryRequirements requirements = {};
    VKD(GetImageMemoryRequirements)(device, image, &requirements);
    VkMemoryAllocateInfo allocation = {};
    allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex =
        memory_type(memory, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkDeviceMemory image_memory = VK_NULL_HANDLE;
    check(VKD(AllocateMemory)(device, &allocation, nullptr, &image_memory), "vkAllocateMemory");
    check(VKD(BindImageMemory)(device, image, image_memory, 0), "vkBindImageMemory");
    VkImageViewCreateInfo view_info = {};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = format;
    view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView view = VK_NULL_HANDLE;
    check(VKD(CreateImageView)(device, &view_info, nullptr, &view), "vkCreateImageView");

    VkAttachmentDescription attachment = {};
    attachment.format = format;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkAttachmentReference reference = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &reference;
    VkSubpassDependency dependency = {};
    dependency.srcSubpass = 0;
    dependency.dstSubpass = VK_SUBPASS_EXTERNAL;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    dependency.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependency.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    VkRenderPassCreateInfo pass_info = {};
    pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    pass_info.attachmentCount = 1;
    pass_info.pAttachments = &attachment;
    pass_info.subpassCount = 1;
    pass_info.pSubpasses = &subpass;
    pass_info.dependencyCount = 1;
    pass_info.pDependencies = &dependency;
    VkRenderPass pass = VK_NULL_HANDLE;
    check(VKD(CreateRenderPass)(device, &pass_info, nullptr, &pass), "vkCreateRenderPass");
    VkFramebufferCreateInfo framebuffer_info = {};
    framebuffer_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebuffer_info.renderPass = pass;
    framebuffer_info.attachmentCount = 1;
    framebuffer_info.pAttachments = &view;
    framebuffer_info.width = kSize;
    framebuffer_info.height = kSize;
    framebuffer_info.layers = 1;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    check(VKD(CreateFramebuffer)(device, &framebuffer_info, nullptr, &framebuffer), "vkCreateFramebuffer");

    VkShaderModuleCreateInfo module_info = {};
    module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    module_info.codeSize = sizeof(kLeftHalfVertex);
    module_info.pCode = kLeftHalfVertex;
    VkShaderModule vertex = VK_NULL_HANDLE;
    check(VKD(CreateShaderModule)(device, &module_info, nullptr, &vertex), "vkCreateShaderModule");
    module_info.codeSize = sizeof(kRedFragment);
    module_info.pCode = kRedFragment;
    VkShaderModule fragment = VK_NULL_HANDLE;
    check(VKD(CreateShaderModule)(device, &module_info, nullptr, &fragment), "vkCreateShaderModule");
    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment;
    stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vertex_input = {};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo assembly = {};
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    const VkViewport viewport = {0.0f, 0.0f, static_cast<float>(kSize), static_cast<float>(kSize), 0.0f, 1.0f};
    const VkRect2D scissor = {{0, 0}, {kSize, kSize}};
    VkPipelineViewportStateCreateInfo viewport_state = {};
    viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.pViewports = &viewport;
    viewport_state.scissorCount = 1;
    viewport_state.pScissors = &scissor;
    VkPipelineRasterizationStateCreateInfo rasterization = {};
    rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization.cullMode = VK_CULL_MODE_NONE;
    rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterization.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample = {};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend_attachment = {};
    blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend = {};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_attachment;
    VkPipelineLayoutCreateInfo layout_info = {};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    check(VKD(CreatePipelineLayout)(device, &layout_info, nullptr, &layout), "vkCreatePipelineLayout");
    VkGraphicsPipelineCreateInfo pipeline_info = {};
    pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_info.stageCount = 2;
    pipeline_info.pStages = stages;
    pipeline_info.pVertexInputState = &vertex_input;
    pipeline_info.pInputAssemblyState = &assembly;
    pipeline_info.pViewportState = &viewport_state;
    pipeline_info.pRasterizationState = &rasterization;
    pipeline_info.pMultisampleState = &multisample;
    pipeline_info.pColorBlendState = &blend;
    pipeline_info.layout = layout;
    pipeline_info.renderPass = pass;
    VkPipeline pipeline = VK_NULL_HANDLE;
    check(VKD(CreateGraphicsPipelines)(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline),
          "vkCreateGraphicsPipelines");

    // The readback buffer, visible to the host.
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(kSize) * kSize * 4;
    VkBufferCreateInfo buffer_info = {};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = bytes;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buffer = VK_NULL_HANDLE;
    check(VKD(CreateBuffer)(device, &buffer_info, nullptr, &buffer), "vkCreateBuffer");
    VKD(GetBufferMemoryRequirements)(device, buffer, &requirements);
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memory_type(memory, requirements.memoryTypeBits,
                                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory buffer_memory = VK_NULL_HANDLE;
    check(VKD(AllocateMemory)(device, &allocation, nullptr, &buffer_memory), "vkAllocateMemory");
    check(VKD(BindBufferMemory)(device, buffer, buffer_memory, 0), "vkBindBufferMemory");

    VkCommandPoolCreateInfo pool_info = {};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.queueFamilyIndex = family;
    VkCommandPool pool = VK_NULL_HANDLE;
    check(VKD(CreateCommandPool)(device, &pool_info, nullptr, &pool), "vkCreateCommandPool");
    VkCommandBufferAllocateInfo command_info = {};
    command_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    command_info.commandPool = pool;
    command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_info.commandBufferCount = 1;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    check(VKD(AllocateCommandBuffers)(device, &command_info, &commands), "vkAllocateCommandBuffers");

    VkCommandBufferBeginInfo begin = {};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(VKD(BeginCommandBuffer)(commands, &begin), "vkBeginCommandBuffer");
    VkClearValue clear = {};
    clear.color.float32[2] = 1.0f;
    clear.color.float32[3] = 1.0f;
    VkRenderPassBeginInfo pass_begin = {};
    pass_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    pass_begin.renderPass = pass;
    pass_begin.framebuffer = framebuffer;
    pass_begin.renderArea = scissor;
    pass_begin.clearValueCount = 1;
    pass_begin.pClearValues = &clear;
    VKD(CmdBeginRenderPass)(commands, &pass_begin, VK_SUBPASS_CONTENTS_INLINE);
    VKD(CmdBindPipeline)(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    VKD(CmdDraw)(commands, 6, 1, 0, 0);
    VKD(CmdEndRenderPass)(commands);
    VkBufferImageCopy region = {};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {kSize, kSize, 1};
    VKD(CmdCopyImageToBuffer)(commands, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
    VkBufferMemoryBarrier to_host = {};
    to_host.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    to_host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    to_host.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_host.buffer = buffer;
    to_host.size = VK_WHOLE_SIZE;
    VKD(CmdPipelineBarrier)(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1,
                            &to_host, 0, nullptr);
    check(VKD(EndCommandBuffer)(commands), "vkEndCommandBuffer");

    VkFenceCreateInfo fence_info = {};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    check(VKD(CreateFence)(device, &fence_info, nullptr, &fence), "vkCreateFence");
    VkSubmitInfo submit = {};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commands;
    check(VKD(QueueSubmit)(queue, 1, &submit, fence), "vkQueueSubmit");
    check(VKD(WaitForFences)(device, 1, &fence, VK_TRUE, 10'000'000'000ull), "vkWaitForFences");

    void *mapped = nullptr;
    check(VKD(MapMemory)(device, buffer_memory, 0, bytes, 0, &mapped), "vkMapMemory");
    const auto *pixels = static_cast<const unsigned char *>(mapped);
    for (uint32_t y = 0; y < kSize; ++y) {
        for (uint32_t x = 0; x < kSize; ++x) {
            const unsigned char *pixel = &pixels[(y * kSize + x) * 4];
            const bool red = x < kSize / 2;
            if (pixel[0] != (red ? 255 : 0) || pixel[1] != 0 || pixel[2] != (red ? 0 : 255) || pixel[3] != 255) {
                fail("pixel (" + std::to_string(x) + ", " + std::to_string(y) + ") is " + std::to_string(pixel[0]) +
                     " " + std::to_string(pixel[1]) + " " + std::to_string(pixel[2]) + " " +
                     std::to_string(pixel[3]));
            }
        }
    }
    VKD(UnmapMemory)(device, buffer_memory);
    std::printf("moltenvk-smoke: %s, Vulkan %u.%u.%u, driver %u: 64 x 64 drawn and read back\n", properties.deviceName,
                VK_API_VERSION_MAJOR(properties.apiVersion), VK_API_VERSION_MINOR(properties.apiVersion),
                VK_API_VERSION_PATCH(properties.apiVersion), properties.driverVersion);

    VKD(DestroyFence)(device, fence, nullptr);
    VKD(DestroyCommandPool)(device, pool, nullptr);
    VKD(DestroyBuffer)(device, buffer, nullptr);
    VKD(FreeMemory)(device, buffer_memory, nullptr);
    VKD(DestroyPipeline)(device, pipeline, nullptr);
    VKD(DestroyPipelineLayout)(device, layout, nullptr);
    VKD(DestroyShaderModule)(device, fragment, nullptr);
    VKD(DestroyShaderModule)(device, vertex, nullptr);
    VKD(DestroyFramebuffer)(device, framebuffer, nullptr);
    VKD(DestroyRenderPass)(device, pass, nullptr);
    VKD(DestroyImageView)(device, view, nullptr);
    VKD(DestroyImage)(device, image, nullptr);
    VKD(FreeMemory)(device, image_memory, nullptr);
    VKD(DestroyDevice)(device, nullptr);
    VKI(DestroyInstance)(instance, nullptr);
    return 0;
}
