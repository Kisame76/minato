/* Test 6 of ADR-0030 without the window, against graphics/tests/fake_vulkan.c in MoltenVK's place
   (graphics.vulkan_fake). It prints what libvulkan's functions written by hand answer themselves: the
   version, layers and extension lists (decision 11), what vkCreateInstance and vkCreateDevice refuse, and
   what vkGetInstanceProcAddr and vkGetDeviceProcAddr hand out (decision 10). The fake prints to stderr what
   they pass on to it: the pinned settings (decision 8), the chains without the app's settings and
   callbacks (decision 5), the extensions libvulkan swaps and adds, and Vulkan 1.3 (decision 11). */
#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

/* Whose a function from vkGet*ProcAddr is: for an exported name, whether it is the export; else whether
   it lies in libvulkan.so, which the loader tells by the address. */
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

#define INSTANCE(instance, name) printf(" %s %s", #name, whose(vkGetInstanceProcAddr(instance, #name), NULL))
#define INSTANCE_EXPORT(instance, name) \
    printf(" %s %s", #name, whose(vkGetInstanceProcAddr(instance, #name), (PFN_vkVoidFunction)name))
#define DEVICE(device, name) printf(" %s %s", #name, whose(vkGetDeviceProcAddr(device, #name), NULL))
#define DEVICE_EXPORT(device, name) \
    printf(" %s %s", #name, whose(vkGetDeviceProcAddr(device, #name), (PFN_vkVoidFunction)name))

static void list(const char *what, VkResult result, uint32_t count, const VkExtensionProperties *properties) {
    printf("%s: %d, %u\n", what, result, count);
    for (uint32_t index = 0; index < count; index++) {
        printf("  %s %u\n", properties[index].extensionName, properties[index].specVersion);
    }
}

static VkBool32 messages(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT types,
                         const VkDebugUtilsMessengerCallbackDataEXT *data, void *context) {
    (void)severity, (void)types, (void)data, (void)context;
    printf("the app's messenger was called\n");
    return VK_FALSE;
}

static VkBool32 reports(VkDebugReportFlagsEXT flags, VkDebugReportObjectTypeEXT type, uint64_t object, size_t location,
                        int32_t code, const char *prefix, const char *message, void *context) {
    (void)flags, (void)type, (void)object, (void)location, (void)code, (void)prefix, (void)message, (void)context;
    printf("the app's report callback was called\n");
    return VK_FALSE;
}

static void *allocate(void *context, size_t size, size_t alignment, VkSystemAllocationScope scope) {
    (void)context, (void)size, (void)alignment, (void)scope;
    printf("the app's allocator was called\n");
    return NULL;
}

static void release(void *context, void *memory) {
    (void)context, (void)memory;
    printf("the app's allocator was called\n");
}

static const VkAllocationCallbacks allocator = {.pfnAllocation = allocate, .pfnFree = release};
static const VkDebugUtilsMessengerCreateInfoEXT messenger = {
    .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
    .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
    .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT,
    .pfnUserCallback = messages,
};

int main(void) {
    uint32_t version = 0, count = 0;
    VkResult result = vkEnumerateInstanceVersion(&version);
    printf("vkEnumerateInstanceVersion: %d, %u.%u.%u\n", result, VK_API_VERSION_MAJOR(version),
           VK_API_VERSION_MINOR(version), VK_API_VERSION_PATCH(version));
    result = vkEnumerateInstanceLayerProperties(&count, NULL);
    printf("vkEnumerateInstanceLayerProperties: %d, %u layers\n", result, count);
    printf("vkEnumerateInstanceExtensionProperties of a layer: %d\n",
           vkEnumerateInstanceExtensionProperties("VK_LAYER_fake", &count, NULL));
    VkExtensionProperties properties[64];
    count = 64;
    result = vkEnumerateInstanceExtensionProperties(NULL, &count, properties);
    list("instance extensions", result, count, properties);
    count = 3;
    result = vkEnumerateInstanceExtensionProperties(NULL, &count, properties);
    list("instance extensions, 3 asked for", result, count, properties);

    /* What vkCreateInstance refuses before MoltenVK sees it. */
    VkInstance instance = VK_NULL_HANDLE, bare = VK_NULL_HANDLE;
    const char *layer[] = {"VK_LAYER_fake"}, *apple[] = {"VK_MVK_macos_surface"}, *device_only[] = {"VK_KHR_swapchain"};
    VkInstanceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .enabledLayerCount = 1,
                                 .ppEnabledLayerNames = layer};
    printf("vkCreateInstance with a layer: %d\n", vkCreateInstance(&info, NULL, &instance));
    info = (VkInstanceCreateInfo){.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .enabledExtensionCount = 1,
                                  .ppEnabledExtensionNames = apple};
    printf("vkCreateInstance with VK_MVK_macos_surface: %d\n", vkCreateInstance(&info, NULL, &instance));
    info.ppEnabledExtensionNames = device_only;
    printf("vkCreateInstance with VK_KHR_swapchain: %d\n", vkCreateInstance(&info, NULL, &instance));
    const VkBaseInStructure unknown = {.sType = (VkStructureType)1000999000, .pNext = (const void *)&messenger};
    info = (VkInstanceCreateInfo){.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pNext = &unknown};
    printf("vkCreateInstance with a structure of unknown size before a debug messenger: %d\n",
           vkCreateInstance(&info, NULL, &instance));

    /* An instance with the surfaces, the app's own MoltenVK settings, its callbacks and validation
       structures before, between and after them, at Vulkan 1.0; then one with nothing. */
    const VkValidationCheckEXT checks[] = {VK_VALIDATION_CHECK_SHADERS_EXT, VK_VALIDATION_CHECK_ALL_EXT};
    const VkValidationFlagsEXT flags = {.sType = VK_STRUCTURE_TYPE_VALIDATION_FLAGS_EXT,
                                        .disabledValidationCheckCount = 2, .pDisabledValidationChecks = checks};
    const VkDirectDriverLoadingListLUNARG loading = {.sType = VK_STRUCTURE_TYPE_DIRECT_DRIVER_LOADING_LIST_LUNARG,
                                                     .pNext = &flags,
                                                     .mode = VK_DIRECT_DRIVER_LOADING_MODE_EXCLUSIVE_LUNARG};
    const VkDebugReportCallbackCreateInfoEXT report = {.sType = VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT,
                                                       .pNext = &loading, .flags = VK_DEBUG_REPORT_ERROR_BIT_EXT,
                                                       .pfnCallback = reports};
    VkDebugUtilsMessengerCreateInfoEXT callbacks = messenger;
    callbacks.pNext = &report;
    const VkBool32 asynchronous = VK_FALSE;
    const VkLayerSettingEXT theirs[] = {
        {"MoltenVK", "MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS", VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &asynchronous}};
    const VkLayerSettingsCreateInfoEXT settings = {.sType = VK_STRUCTURE_TYPE_LAYER_SETTINGS_CREATE_INFO_EXT,
                                                   .pNext = &callbacks, .settingCount = 1, .pSettings = theirs};
    const VkValidationFeatureEnableEXT enabled[] = {VK_VALIDATION_FEATURE_ENABLE_BEST_PRACTICES_EXT};
    const VkValidationFeaturesEXT features = {.sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT, .pNext = &settings,
                                              .enabledValidationFeatureCount = 1,
                                              .pEnabledValidationFeatures = enabled};
    const VkApplicationInfo application = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                           .pApplicationName = "vulkan-fake-test", .applicationVersion = 7,
                                           .apiVersion = VK_API_VERSION_1_0};
    const char *extensions[] = {"VK_KHR_surface", "VK_KHR_android_surface", "VK_KHR_get_surface_capabilities2",
                                "VK_EXT_debug_utils"};
    info = (VkInstanceCreateInfo){.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pNext = &features,
                                  .pApplicationInfo = &application, .enabledExtensionCount = 4,
                                  .ppEnabledExtensionNames = extensions};
    printf("vkCreateInstance: %d\n", vkCreateInstance(&info, &allocator, &instance));
    info = (VkInstanceCreateInfo){.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    printf("vkCreateInstance without application info or extensions: %d\n", vkCreateInstance(&info, NULL, &bare));
    /* One with the surface only, whose chain starts with a structure to take out. */
    VkInstance surface_only = VK_NULL_HANDLE;
    const char *surface[] = {"VK_KHR_surface", "VK_GOOGLE_surfaceless_query"};
    const VkValidationFlagsEXT one_flag = {.sType = VK_STRUCTURE_TYPE_VALIDATION_FLAGS_EXT,
                                           .disabledValidationCheckCount = 1, .pDisabledValidationChecks = checks};
    VkDebugReportCallbackCreateInfoEXT first = report;
    first.pNext = &one_flag;
    info = (VkInstanceCreateInfo){.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pNext = &first,
                                  .enabledExtensionCount = 2, .ppEnabledExtensionNames = surface};
    printf("vkCreateInstance with the surface only: %d\n", vkCreateInstance(&info, NULL, &surface_only));

    printf("vkGetInstanceProcAddr without an instance:");
    INSTANCE_EXPORT(VK_NULL_HANDLE, vkCreateInstance);
    INSTANCE_EXPORT(VK_NULL_HANDLE, vkEnumerateInstanceVersion);
    INSTANCE_EXPORT(VK_NULL_HANDLE, vkEnumerateInstanceExtensionProperties);
    INSTANCE_EXPORT(VK_NULL_HANDLE, vkEnumerateInstanceLayerProperties);
    INSTANCE_EXPORT(VK_NULL_HANDLE, vkGetInstanceProcAddr);
    INSTANCE_EXPORT(VK_NULL_HANDLE, vkCreateDevice);
    INSTANCE(VK_NULL_HANDLE, vkNotAFunction);
    printf("\nvkGetInstanceProcAddr:");
    INSTANCE_EXPORT(instance, vkGetInstanceProcAddr);
    INSTANCE_EXPORT(instance, vkCreateInstance);
    INSTANCE_EXPORT(instance, vkEnumeratePhysicalDevices);
    INSTANCE_EXPORT(instance, vkCmdDraw);
    INSTANCE(instance, vkCmdPushDescriptorSet);
    INSTANCE_EXPORT(instance, vkCreateAndroidSurfaceKHR);
    INSTANCE_EXPORT(instance, vkDestroySurfaceKHR);
    INSTANCE(instance, vkGetPhysicalDeviceSurfaceCapabilities2KHR);
    INSTANCE_EXPORT(instance, vkCreateSwapchainKHR);
    INSTANCE_EXPORT(instance, vkGetPhysicalDevicePresentRectanglesKHR);
    INSTANCE(instance, vkCreateDebugUtilsMessengerEXT);
    INSTANCE(instance, vkCreateDebugReportCallbackEXT);
    printf("\nvkGetInstanceProcAddr of the instance with nothing:");
    INSTANCE_EXPORT(bare, vkCreateAndroidSurfaceKHR);
    INSTANCE_EXPORT(bare, vkDestroySurfaceKHR);
    INSTANCE(bare, vkGetPhysicalDeviceSurfaceCapabilities2KHR);
    INSTANCE_EXPORT(bare, vkCreateSwapchainKHR);
    INSTANCE(bare, vkCreateDebugUtilsMessengerEXT);
    printf("\nvkGetInstanceProcAddr of the instance with the surface only:");
    INSTANCE_EXPORT(surface_only, vkDestroySurfaceKHR);
    INSTANCE_EXPORT(surface_only, vkCreateAndroidSurfaceKHR);
    INSTANCE(surface_only, vkGetPhysicalDeviceSurfaceCapabilities2KHR);
    const VkInstance unknown_instance = (VkInstance)(void *)&count;
    printf("\nvkGetInstanceProcAddr of an instance libvulkan did not make:");
    INSTANCE_EXPORT(unknown_instance, vkEnumeratePhysicalDevices);
    printf("\n");

    /* A debug messenger stays libvulkan's: the app's callback never reaches MoltenVK (decision 5). */
    PFN_vkCreateDebugUtilsMessengerEXT create_messenger =
        (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");
    PFN_vkDestroyDebugUtilsMessengerEXT destroy_messenger =
        (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT");
    if (strcmp(whose((PFN_vkVoidFunction)create_messenger, NULL), "ours") != 0 ||
        strcmp(whose((PFN_vkVoidFunction)destroy_messenger, NULL), "ours") != 0) {
        printf("vkCreateDebugUtilsMessengerEXT: not libvulkan's own\n");
    } else {
        VkDebugUtilsMessengerEXT handle = VK_NULL_HANDLE;
        result = create_messenger(instance, &messenger, &allocator, &handle);
        printf("vkCreateDebugUtilsMessengerEXT: %d, %s\n", result, handle != VK_NULL_HANDLE ? "a handle" : "no handle");
        destroy_messenger(instance, handle, &allocator);
    }

    VkPhysicalDevice physical = VK_NULL_HANDLE;
    count = 1;
    result = vkEnumeratePhysicalDevices(instance, &count, &physical);
    VkPhysicalDeviceProperties device_properties;
    vkGetPhysicalDeviceProperties(physical, &device_properties);
    printf("vkEnumeratePhysicalDevices: %d, %u: %s at Vulkan %u.%u for an app that asked for 1.0\n", result, count,
           device_properties.deviceName, VK_API_VERSION_MAJOR(device_properties.apiVersion),
           VK_API_VERSION_MINOR(device_properties.apiVersion));
    result = vkEnumerateDeviceLayerProperties(physical, &count, NULL);
    printf("vkEnumerateDeviceLayerProperties: %d, %u layers\n", result, count);
    printf("vkEnumerateDeviceExtensionProperties of a layer: %d\n",
           vkEnumerateDeviceExtensionProperties(physical, "VK_LAYER_fake", &count, NULL));
    count = 64;
    result = vkEnumerateDeviceExtensionProperties(physical, NULL, &count, properties);
    list("device extensions", result, count, properties);

    /* What vkCreateDevice refuses, then a device with the swapchain, a structure before the app's memory
       report and a (long ignored) device layer, and a device with nothing. */
    const float priority = 1;
    const VkDeviceQueueCreateInfo queue = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1,
                                           .pQueuePriorities = &priority};
    const char *portability[] = {"VK_KHR_portability_subset"}, *instance_only[] = {"VK_EXT_debug_utils"};
    VkDevice device = VK_NULL_HANDLE, plain = VK_NULL_HANDLE;
    VkDeviceCreateInfo device_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
                                      .pQueueCreateInfos = &queue, .enabledExtensionCount = 1,
                                      .ppEnabledExtensionNames = portability};
    printf("vkCreateDevice with VK_KHR_portability_subset: %d\n", vkCreateDevice(physical, &device_info, NULL, &device));
    device_info.ppEnabledExtensionNames = instance_only;
    printf("vkCreateDevice with VK_EXT_debug_utils: %d\n", vkCreateDevice(physical, &device_info, NULL, &device));
    VkDeviceDeviceMemoryReportCreateInfoEXT memory_report = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_DEVICE_MEMORY_REPORT_CREATE_INFO_EXT};
    const VkPhysicalDevice16BitStorageFeatures storage = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES,
                                                          .pNext = &memory_report, .storageBuffer16BitAccess = VK_TRUE};
    const char *device_extensions[] = {"VK_KHR_swapchain", "VK_KHR_incremental_present", "VK_KHR_16bit_storage",
                                       "VK_KHR_draw_indirect_count"};
    device_info = (VkDeviceCreateInfo){.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &storage,
                                       .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue, .enabledLayerCount = 1,
                                       .ppEnabledLayerNames = layer, .enabledExtensionCount = 4,
                                       .ppEnabledExtensionNames = device_extensions};
    printf("vkCreateDevice: %d\n", vkCreateDevice(physical, &device_info, &allocator, &device));
    device_info = (VkDeviceCreateInfo){.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
                                       .pQueueCreateInfos = &queue};
    printf("vkCreateDevice with nothing: %d\n", vkCreateDevice(physical, &device_info, NULL, &plain));

    printf("vkGetDeviceProcAddr:");
    DEVICE_EXPORT(device, vkGetDeviceProcAddr);
    DEVICE_EXPORT(device, vkQueueSubmit);
    DEVICE_EXPORT(device, vkCmdDraw);
    DEVICE(device, vkCmdPushDescriptorSet);
    DEVICE_EXPORT(device, vkGetPhysicalDeviceProperties);
    DEVICE_EXPORT(device, vkGetInstanceProcAddr);
    DEVICE_EXPORT(device, vkCreateInstance);
    DEVICE_EXPORT(device, vkCreateSwapchainKHR);
    DEVICE_EXPORT(device, vkAcquireNextImage2KHR);
    DEVICE(device, vkCmdDrawIndirectCountKHR);
    printf("\nvkGetDeviceProcAddr of the device with nothing:");
    DEVICE_EXPORT(plain, vkCreateSwapchainKHR);
    DEVICE_EXPORT(plain, vkQueuePresentKHR);
    DEVICE_EXPORT(plain, vkQueueSubmit);
    DEVICE(plain, vkCmdDrawIndirectCountKHR);
    const VkDevice unknown_device = (VkDevice)(void *)&count;
    printf("\nvkGetDeviceProcAddr of a device libvulkan did not make:");
    DEVICE_EXPORT(unknown_device, vkQueueSubmit);
    printf("\n");

    /* AHardwareBuffer's two functions while its extension is not offered (decision 10). */
    VkAndroidHardwareBufferPropertiesANDROID buffer_properties = {
        .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID};
    const VkMemoryGetAndroidHardwareBufferInfoANDROID memory = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_GET_ANDROID_HARDWARE_BUFFER_INFO_ANDROID};
    struct AHardwareBuffer *buffer = NULL;
    printf("vkGetAndroidHardwareBufferPropertiesANDROID: %d, vkGetMemoryAndroidHardwareBufferANDROID: %d\n",
           vkGetAndroidHardwareBufferPropertiesANDROID(device, NULL, &buffer_properties),
           vkGetMemoryAndroidHardwareBufferANDROID(device, &memory, &buffer));

    vkDestroyDevice(device, &allocator);
    vkDestroyDevice(plain, NULL);
    vkDestroyInstance(instance, &allocator);
    vkDestroyInstance(bare, NULL);
    vkDestroyInstance(surface_only, NULL);
    printf("done\n");
    return 0;
}
