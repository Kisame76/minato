/* Test 6 of ADR-0030 without the window, against MoltenVK (graphics.vulkan_moltenvk). CTest runs it with
   MoltenVK's environment variables set against what libvulkan pins (decision 8): asynchronous submits,
   prefilled command buffers, semaphores by callbacks and Vulkan 1.0 advertised; the app passes its own
   MoltenVK settings as well. It prints what libvulkan hands out: the start of both extension lists and
   none of what they hide (decision 11), a device at Vulkan 1.3 for an app that asks for 1.0, and a few
   answers of vkGet*ProcAddr (decision 10). Then it submits one fill with a semaphore to signal, with
   markers on stderr, between which graphics-run's Metal hook prints what Metal sees: the settings hold
   when the command buffer is made, signaled with MTLEvents (the app's semaphore, then libvulkan's timeline
   semaphore of the queue, decision 13) and committed inside vkQueueSubmit, on the calling thread, a guest
   thread. */
#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The names neither list may hold (decision 11). */
static const char *const hidden[] = {
    "VK_EXT_metal_surface", "VK_EXT_headless_surface", "VK_EXT_metal_objects", "VK_EXT_external_memory_metal",
    "VK_KHR_portability_subset", "VK_KHR_portability_enumeration", "VK_EXT_layer_settings",
    "VK_GOOGLE_display_timing", "VK_KHR_present_id", "VK_KHR_present_id2", "VK_KHR_present_wait",
    "VK_KHR_present_wait2", "VK_EXT_swapchain_maintenance1", "VK_KHR_swapchain_maintenance1",
    "VK_EXT_surface_maintenance1", "VK_KHR_surface_maintenance1", "VK_KHR_swapchain_mutable_format",
    "VK_EXT_hdr_metadata", "VK_EXT_calibrated_timestamps", "VK_KHR_calibrated_timestamps",
};

static const char *whose(PFN_vkVoidFunction function, PFN_vkVoidFunction exported) {
    Dl_info info;
    if (function == NULL) {
        return "NULL";
    }
    if (exported != NULL) {
        return function == exported ? "ours" : "not the export";
    }
    return dladdr((const void *)function, &info) && info.dli_fname != NULL &&
                   strstr(info.dli_fname, "libvulkan.so") != NULL
               ? "ours"
               : "not ours";
}

static void check(const char *what, VkResult result) {
    if (result != VK_SUCCESS) {
        printf("%s: %d\n", what, result);
        exit(1);
    }
}

/* Whether a list starts with `first` and holds nothing hidden, each name once. */
static void examine(const char *what, const VkExtensionProperties *list, uint32_t count, const char *const *first,
                    uint32_t firsts) {
    printf("%s start with", what);
    for (uint32_t index = 0; index < firsts; index++) {
        printf(" %s", first[index]);
    }
    int starts = count >= firsts, clean = 1;
    for (uint32_t index = 0; index < count; index++) {
        const char *name = list[index].extensionName;
        starts &= index >= firsts || strcmp(name, first[index]) == 0;
        for (size_t other = 0; other < sizeof hidden / sizeof *hidden; other++) {
            clean &= strcmp(name, hidden[other]) != 0;
        }
        clean &= strncmp(name, "VK_MVK_", 7) != 0;
        for (uint32_t later = index + 1; later < count; later++) {
            clean &= strcmp(name, list[later].extensionName) != 0;
        }
    }
    printf(": %s; none hidden, each once: %s\n", starts ? "yes" : "no", clean ? "yes" : "no");
}

int main(void) {
    uint32_t version = 0, count = 0;
    check("vkEnumerateInstanceVersion", vkEnumerateInstanceVersion(&version));
    printf("vkEnumerateInstanceVersion: %u.%u.%u\n", VK_API_VERSION_MAJOR(version), VK_API_VERSION_MINOR(version),
           VK_API_VERSION_PATCH(version));
    VkExtensionProperties properties[512];
    count = 512;
    check("vkEnumerateInstanceExtensionProperties", vkEnumerateInstanceExtensionProperties(NULL, &count, properties));
    static const char *const instance_first[] = {
        "VK_KHR_surface", "VK_KHR_surface_protected_capabilities", "VK_KHR_android_surface",
        "VK_EXT_swapchain_colorspace", "VK_KHR_get_surface_capabilities2", "VK_GOOGLE_surfaceless_query",
        "VK_EXT_debug_report"};
    examine("instance extensions", properties, count, instance_first, 7);

    /* The app's own settings for MoltenVK, which libvulkan takes out. */
    const VkBool32 asynchronous = VK_FALSE;
    const int32_t prefill = 1, callbacks = 3;
    const uint32_t old = VK_API_VERSION_1_0;
    const VkLayerSettingEXT theirs[] = {
        {"MoltenVK", "MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS", VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &asynchronous},
        {"MoltenVK", "MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS", VK_LAYER_SETTING_TYPE_INT32_EXT, 1, &prefill},
        {"MoltenVK", "MVK_CONFIG_VK_SEMAPHORE_SUPPORT_STYLE", VK_LAYER_SETTING_TYPE_INT32_EXT, 1, &callbacks},
        {"MoltenVK", "MVK_CONFIG_API_VERSION_TO_ADVERTISE", VK_LAYER_SETTING_TYPE_UINT32_EXT, 1, &old},
    };
    const VkLayerSettingsCreateInfoEXT settings = {.sType = VK_STRUCTURE_TYPE_LAYER_SETTINGS_CREATE_INFO_EXT,
                                                   .settingCount = 4, .pSettings = theirs};
    const VkApplicationInfo application = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                           .pApplicationName = "vulkan-moltenvk-test",
                                           .apiVersion = VK_API_VERSION_1_0};
    const char *extensions[] = {"VK_KHR_surface", "VK_KHR_android_surface"};
    const VkInstanceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pNext = &settings,
                                       .pApplicationInfo = &application, .enabledExtensionCount = 2,
                                       .ppEnabledExtensionNames = extensions};
    VkInstance instance;
    check("vkCreateInstance", vkCreateInstance(&info, NULL, &instance));
    printf("vkCreateInstance with the surfaces and the app's own MoltenVK settings: 0\n");

    VkPhysicalDevice physical;
    count = 1;
    VkResult result = vkEnumeratePhysicalDevices(instance, &count, &physical);
    if (result != VK_INCOMPLETE) {
        check("vkEnumeratePhysicalDevices", result);
    }
    VkPhysicalDeviceProperties device_properties;
    vkGetPhysicalDeviceProperties(physical, &device_properties);
    printf("a device at Vulkan %u.%u for an app that asked for 1.0\n",
           VK_API_VERSION_MAJOR(device_properties.apiVersion), VK_API_VERSION_MINOR(device_properties.apiVersion));
    count = 512;
    check("vkEnumerateDeviceExtensionProperties",
          vkEnumerateDeviceExtensionProperties(physical, NULL, &count, properties));
    static const char *const device_first[] = {"VK_KHR_incremental_present"};
    examine("device extensions", properties, count, device_first, 1);

    const float priority = 1;
    const VkDeviceQueueCreateInfo queue_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                                .queueCount = 1, .pQueuePriorities = &priority};
    const char *device_extensions[] = {"VK_KHR_swapchain"};
    const VkDeviceCreateInfo device_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
                                            .pQueueCreateInfos = &queue_info, .enabledExtensionCount = 1,
                                            .ppEnabledExtensionNames = device_extensions};
    VkDevice device;
    check("vkCreateDevice", vkCreateDevice(physical, &device_info, NULL, &device));
    printf("vkCreateDevice with the swapchain: 0\n");
    printf("vkGetInstanceProcAddr: vkCreateAndroidSurfaceKHR %s, vkGetPhysicalDeviceSurfaceCapabilities2KHR %s, "
           "vkCmdPushDescriptorSet %s\n",
           whose(vkGetInstanceProcAddr(instance, "vkCreateAndroidSurfaceKHR"), (PFN_vkVoidFunction)vkCreateAndroidSurfaceKHR),
           whose(vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceSurfaceCapabilities2KHR"), NULL),
           whose(vkGetInstanceProcAddr(instance, "vkCmdPushDescriptorSet"), NULL));
    printf("vkGetDeviceProcAddr: vkCmdDraw %s, vkCmdPushDescriptorSet %s, vkGetPhysicalDeviceProperties %s, "
           "vkCreateSwapchainKHR %s\n",
           whose(vkGetDeviceProcAddr(device, "vkCmdDraw"), (PFN_vkVoidFunction)vkCmdDraw),
           whose(vkGetDeviceProcAddr(device, "vkCmdPushDescriptorSet"), NULL),
           whose(vkGetDeviceProcAddr(device, "vkGetPhysicalDeviceProperties"), NULL),
           whose(vkGetDeviceProcAddr(device, "vkCreateSwapchainKHR"), (PFN_vkVoidFunction)vkCreateSwapchainKHR));

    /* One fill into a small buffer, submitted with a semaphore to signal and a fence. */
    VkQueue queue;
    vkGetDeviceQueue(device, 0, 0, &queue);
    const VkBufferCreateInfo buffer_info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 256,
                                            .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VkBuffer buffer;
    check("vkCreateBuffer", vkCreateBuffer(device, &buffer_info, NULL, &buffer));
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(device, buffer, &requirements);
    const VkMemoryAllocateInfo allocate_info = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                                .allocationSize = requirements.size,
                                                .memoryTypeIndex = (uint32_t)__builtin_ctz(requirements.memoryTypeBits)};
    VkDeviceMemory memory;
    check("vkAllocateMemory", vkAllocateMemory(device, &allocate_info, NULL, &memory));
    check("vkBindBufferMemory", vkBindBufferMemory(device, buffer, memory, 0));
    const VkCommandPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    VkCommandPool pool;
    check("vkCreateCommandPool", vkCreateCommandPool(device, &pool_info, NULL, &pool));
    const VkCommandBufferAllocateInfo command_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                                      .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                                      .commandBufferCount = 1};
    VkCommandBuffer commands;
    check("vkAllocateCommandBuffers", vkAllocateCommandBuffers(device, &command_info, &commands));
    const VkSemaphoreCreateInfo semaphore_info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore semaphore;
    check("vkCreateSemaphore", vkCreateSemaphore(device, &semaphore_info, NULL, &semaphore));
    const VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    check("vkCreateFence", vkCreateFence(device, &fence_info, NULL, &fence));

    const VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    fprintf(stderr, "vulkan-moltenvk-test: vkBeginCommandBuffer\n");
    check("vkBeginCommandBuffer", vkBeginCommandBuffer(commands, &begin));
    vkCmdFillBuffer(commands, buffer, 0, 256, 0x01020304);
    fprintf(stderr, "vulkan-moltenvk-test: vkEndCommandBuffer\n");
    check("vkEndCommandBuffer", vkEndCommandBuffer(commands));
    const VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1,
                                 .pCommandBuffers = &commands, .signalSemaphoreCount = 1,
                                 .pSignalSemaphores = &semaphore};
    fprintf(stderr, "vulkan-moltenvk-test: vkQueueSubmit\n");
    result = vkQueueSubmit(queue, 1, &submit, fence);
    fprintf(stderr, "vulkan-moltenvk-test: vkQueueSubmit returned\n");
    printf("vkQueueSubmit: %d, vkWaitForFences: %d\n", result, vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));

    vkDestroyFence(device, fence, NULL);
    vkDestroySemaphore(device, semaphore, NULL);
    vkDestroyCommandPool(device, pool, NULL);
    vkDestroyBuffer(device, buffer, NULL);
    vkFreeMemory(device, memory, NULL);
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);
    printf("done\n");
    return 0;
}
