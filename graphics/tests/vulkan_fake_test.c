/* Test 6 of ADR-0030 without the window, against graphics/tests/fake_vulkan.c in MoltenVK's place
   (graphics.vulkan_fake). It prints what libvulkan's functions written by hand answer themselves: the
   version, layers and extension lists (decision 11), what vkCreateInstance and vkCreateDevice refuse, what
   vkGetInstanceProcAddr and vkGetDeviceProcAddr hand out (decision 10), what the queue's functions and the
   waits answer, and the messages the app's debug callbacks get. The fake prints to stderr what they pass on
   to it:
   - the pinned settings (decision 8), the chains without the app's settings, with graphics/host's callbacks
     in place of the app's (decision 5) and with timeline semaphores enabled (decision 11), the extensions
     libvulkan swaps and adds, and Vulkan 1.3 (decision 11);
   - a timeline semaphore per queue, signaled last in every submit, and the waits in slices of 10 ms that
     vkQueueWaitIdle, vkDeviceWaitIdle and the app's waits become (decisions 4 and 13);
   - that graphics/host's "scheduled" waits until a submit has left MoltenVK (decision 13);
   - the messages the fake sends from a thread of its own, which graphics/host logs instead of calling the
     app (decision 5). */
#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

/* The fake's marker fences, which it makes none of: a submit with FAILING fails, one with SCHEDULED stays in
   the fake while it asks graphics/host for "scheduled". */
#define FAILING ((VkFence)(uintptr_t)0xfa11)
#define SCHEDULED ((VkFence)(uintptr_t)0x5c4ed)
#define GIVEN ((VkFence)(uintptr_t)0xf)

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

/* What the app gives its callbacks. */
static int context;

static VkBool32 messages(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT types,
                         const VkDebugUtilsMessengerCallbackDataEXT *data, void *user) {
    printf("the app's messenger: \"%s\", severity 0x%x, types 0x%x, id %s %d, its context: %s\n", data->pMessage,
           severity, types, data->pMessageIdName, data->messageIdNumber, user == &context ? "yes" : "no");
    return VK_FALSE;
}

static VkBool32 reports(VkDebugReportFlagsEXT flags, VkDebugReportObjectTypeEXT type, uint64_t object, size_t location,
                        int32_t code, const char *prefix, const char *message, void *user) {
    printf("the app's report callback: \"%s\", flags 0x%x, object type %d, object %llu, location %zu, code %d, "
           "prefix %s, its context: %s\n", message, flags, type, (unsigned long long)object, location, code, prefix,
           user == &context ? "yes" : "no");
    return VK_FALSE;
}

static void *allocate(void *user, size_t size, size_t alignment, VkSystemAllocationScope scope) {
    (void)user, (void)size, (void)alignment, (void)scope;
    printf("the app's allocator was called\n");
    return NULL;
}

static void release(void *user, void *memory) {
    (void)user, (void)memory;
    printf("the app's allocator was called\n");
}

static const VkAllocationCallbacks allocator = {.pfnAllocation = allocate, .pfnFree = release};
static const VkDebugUtilsMessengerCreateInfoEXT messenger = {
    .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
    .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
    .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT,
    .pfnUserCallback = messages,
    .pUserData = &context,
};

/* The debug callbacks registered through their create functions: the app's messages reach them on the
   app's thread, and the fake's from its own thread only reach graphics/host's log (decision 5). */
static void callbacks(VkInstance instance) {
    PFN_vkCreateDebugUtilsMessengerEXT create_messenger =
        (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");
    PFN_vkDestroyDebugUtilsMessengerEXT destroy_messenger =
        (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT");
    PFN_vkSubmitDebugUtilsMessageEXT submit_message =
        (PFN_vkSubmitDebugUtilsMessageEXT)vkGetInstanceProcAddr(instance, "vkSubmitDebugUtilsMessageEXT");
    PFN_vkCreateDebugReportCallbackEXT create_report =
        (PFN_vkCreateDebugReportCallbackEXT)vkGetInstanceProcAddr(instance, "vkCreateDebugReportCallbackEXT");
    PFN_vkDestroyDebugReportCallbackEXT destroy_report =
        (PFN_vkDestroyDebugReportCallbackEXT)vkGetInstanceProcAddr(instance, "vkDestroyDebugReportCallbackEXT");
    PFN_vkDebugReportMessageEXT report_message =
        (PFN_vkDebugReportMessageEXT)vkGetInstanceProcAddr(instance, "vkDebugReportMessageEXT");
    printf("the debug functions: %s %s %s %s %s %s\n", whose((PFN_vkVoidFunction)create_messenger, NULL),
           whose((PFN_vkVoidFunction)destroy_messenger, NULL), whose((PFN_vkVoidFunction)submit_message, NULL),
           whose((PFN_vkVoidFunction)create_report, NULL), whose((PFN_vkVoidFunction)destroy_report, NULL),
           whose((PFN_vkVoidFunction)report_message, NULL));
    if (create_messenger == NULL || destroy_messenger == NULL || submit_message == NULL || create_report == NULL ||
        destroy_report == NULL || report_message == NULL) {
        return;
    }
    VkDebugUtilsMessengerEXT handle = VK_NULL_HANDLE;
    VkResult result = create_messenger(instance, &messenger, &allocator, &handle);
    printf("vkCreateDebugUtilsMessengerEXT: %d, %s\n", result, handle != VK_NULL_HANDLE ? "a handle" : "no handle");
    const VkDebugUtilsMessengerCallbackDataEXT data = {.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CALLBACK_DATA_EXT,
                                                       .pMessageIdName = "app", .messageIdNumber = 1,
                                                       .pMessage = "a message of the app"};
    submit_message(instance, VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT, VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT,
                   &data);
    destroy_messenger(instance, handle, &allocator);
    const VkDebugReportCallbackCreateInfoEXT report = {.sType = VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT,
                                                       .flags = VK_DEBUG_REPORT_ERROR_BIT_EXT, .pfnCallback = reports,
                                                       .pUserData = &context};
    VkDebugReportCallbackEXT callback = VK_NULL_HANDLE;
    result = create_report(instance, &report, &allocator, &callback);
    printf("vkCreateDebugReportCallbackEXT: %d, %s\n", result, callback != VK_NULL_HANDLE ? "a handle" : "no handle");
    report_message(instance, VK_DEBUG_REPORT_ERROR_BIT_EXT, VK_DEBUG_REPORT_OBJECT_TYPE_DEVICE_EXT, 9, 8, 7, "app",
                   "a report of the app");
    destroy_report(instance, callback, &allocator);
    /* After their destruction, the app's messages reach no callback. */
    submit_message(instance, VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT, VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT,
                   &data);
}

/* The queue's functions (decisions 4 and 13): libvulkan's timeline semaphore of each queue is signaled last
   in the last batch of every submit that succeeds, vkQueueWaitIdle and vkDeviceWaitIdle wait for it, and
   every wait goes to MoltenVK in slices of 10 ms. */
static void queues(VkDevice device) {
    VkQueue first = VK_NULL_HANDLE, second = VK_NULL_HANDLE, other = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, 0, 0, &first);
    vkGetDeviceQueue(device, 0, 1, &second);
    const VkDeviceQueueInfo2 other_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2, .queueFamilyIndex = 1};
    vkGetDeviceQueue2(device, &other_info, &other);
    const VkSemaphoreTypeCreateInfo timeline_type = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                                     .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE, .initialValue = 4};
    const VkSemaphoreCreateInfo binary_info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    const VkSemaphoreCreateInfo timeline_info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
                                                 .pNext = &timeline_type};
    VkSemaphore waited, signaled, binary, timeline;
    vkCreateSemaphore(device, &binary_info, NULL, &waited);
    vkCreateSemaphore(device, &binary_info, NULL, &signaled);
    vkCreateSemaphore(device, &binary_info, NULL, &binary);
    vkCreateSemaphore(device, &timeline_info, NULL, &timeline);

    /* Two batches, the last with the app's timeline values and device group. */
    const VkCommandBuffer commands = (VkCommandBuffer)(uintptr_t)0xc0;
    const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    const VkSemaphore last_signals[] = {binary, timeline};
    const uint64_t values[] = {0, 5};
    const uint32_t indices[] = {0, 0}, masks[] = {1};
    /* Not const, so the checks below read what libvulkan may have written. */
    VkDeviceGroupSubmitInfo group = {.sType = VK_STRUCTURE_TYPE_DEVICE_GROUP_SUBMIT_INFO, .commandBufferCount = 1,
                                     .pCommandBufferDeviceMasks = masks, .signalSemaphoreCount = 2,
                                     .pSignalSemaphoreDeviceIndices = indices};
    VkTimelineSemaphoreSubmitInfo timeline_values = {.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
                                                     .pNext = &group, .signalSemaphoreValueCount = 2,
                                                     .pSignalSemaphoreValues = values};
    VkSubmitInfo batches[] = {
        {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1, .pWaitSemaphores = &waited,
         .pWaitDstStageMask = &stage, .commandBufferCount = 1, .pCommandBuffers = &commands,
         .signalSemaphoreCount = 1, .pSignalSemaphores = &signaled},
        {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &timeline_values, .commandBufferCount = 1,
         .pCommandBuffers = &commands, .signalSemaphoreCount = 2, .pSignalSemaphores = last_signals},
    };
    printf("vkQueueSubmit with two batches: %d\n", vkQueueSubmit(first, 2, batches, GIVEN));
    printf("  the app's batches as they were: %s\n",
           batches[1].pNext == &timeline_values && batches[1].signalSemaphoreCount == 2 &&
                   batches[1].pSignalSemaphores == last_signals && timeline_values.pNext == &group &&
                   timeline_values.signalSemaphoreValueCount == 2 && timeline_values.pSignalSemaphoreValues == values &&
                   group.signalSemaphoreCount == 2 && group.pSignalSemaphoreDeviceIndices == indices
               ? "yes"
               : "no");
    printf("vkQueueSubmit with a fence only: %d\n", vkQueueSubmit(first, 0, NULL, GIVEN));
    printf("vkQueueSubmit with nothing: %d\n", vkQueueSubmit(first, 0, NULL, VK_NULL_HANDLE));
    printf("vkQueueSubmit with binary semaphores only: %d\n", vkQueueSubmit(first, 1, batches, VK_NULL_HANDLE));
    printf("vkQueueSubmit that fails: %d\n", vkQueueSubmit(first, 1, batches, FAILING));
    printf("vkQueueSubmit after it: %d\n", vkQueueSubmit(first, 1, batches, VK_NULL_HANDLE));

    const VkCommandBufferSubmitInfo command_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                                                    .commandBuffer = commands};
    const VkSemaphoreSubmitInfo signal_info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = timeline,
                                               .value = 6, .stageMask = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT};
    const VkSubmitInfo2 batch2 = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = 1,
                                  .pCommandBufferInfos = &command_info, .signalSemaphoreInfoCount = 1,
                                  .pSignalSemaphoreInfos = &signal_info};
    PFN_vkQueueSubmit2 submit2 = (PFN_vkQueueSubmit2)vkGetDeviceProcAddr(device, "vkQueueSubmit2KHR");
    printf("vkQueueSubmit2KHR: %s\n", whose((PFN_vkVoidFunction)submit2, (PFN_vkVoidFunction)vkQueueSubmit2));
    printf("vkQueueSubmit2KHR with a batch: %d\n", submit2(first, 1, &batch2, VK_NULL_HANDLE));
    printf("vkQueueSubmit2 with a fence only: %d\n", vkQueueSubmit2(first, 0, NULL, GIVEN));
    const VkBindSparseInfo bind = {.sType = VK_STRUCTURE_TYPE_BIND_SPARSE_INFO, .waitSemaphoreCount = 1,
                                   .pWaitSemaphores = &waited, .signalSemaphoreCount = 1,
                                   .pSignalSemaphores = &signaled};
    printf("vkQueueBindSparse: %d\n", vkQueueBindSparse(first, 1, &bind, VK_NULL_HANDLE));

    /* graphics/host's "scheduled" waits until no call is in MoltenVK (decision 13): the fake holds each of
       these while it asks, and the wait after it, which graphics/host does not count, prints the answer. */
    const VkFence given = GIVEN;
    printf("while graphics/host is asked for \"scheduled\": vkQueueSubmit %d,",
           vkQueueSubmit(first, 1, batches, SCHEDULED));
    vkWaitForFences(device, 1, &given, VK_TRUE, 0);
    printf(" vkQueueSubmit2 %d,", vkQueueSubmit2(first, 1, &batch2, SCHEDULED));
    vkWaitForFences(device, 1, &given, VK_TRUE, 0);
    printf(" vkQueueBindSparse %d\n", vkQueueBindSparse(first, 1, &bind, SCHEDULED));
    vkWaitForFences(device, 1, &given, VK_TRUE, 0);

    printf("vkQueueWaitIdle: %d\n", vkQueueWaitIdle(first));
    printf("vkQueueWaitIdle of a queue without submits: %d\n", vkQueueWaitIdle(second));
    printf("vkQueueSubmit on the other family: %d\n", vkQueueSubmit(other, 1, batches, VK_NULL_HANDLE));
    printf("vkDeviceWaitIdle: %d\n", vkDeviceWaitIdle(device));

    /* The app's waits, in slices (decision 4). */
    const VkFence fences[] = {GIVEN, GIVEN};
    printf("vkWaitForFences without a timeout: %d\n", vkWaitForFences(device, 2, fences, VK_TRUE, UINT64_MAX));
    printf("vkWaitForFences for 0 ns: %d\n", vkWaitForFences(device, 2, fences, VK_FALSE, 0));
    printf("vkWaitForFences for 5 ms: %d\n", vkWaitForFences(device, 1, fences, VK_TRUE, 5000000));
    const uint64_t five = 5;
    const VkSemaphoreWaitInfo wait_info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
                                           .flags = VK_SEMAPHORE_WAIT_ANY_BIT, .semaphoreCount = 1,
                                           .pSemaphores = &timeline, .pValues = &five};
    PFN_vkWaitSemaphores wait_semaphores = (PFN_vkWaitSemaphores)vkGetDeviceProcAddr(device, "vkWaitSemaphoresKHR");
    printf("vkWaitSemaphoresKHR: %s\n", whose((PFN_vkVoidFunction)wait_semaphores, (PFN_vkVoidFunction)vkWaitSemaphores));
    printf("vkWaitSemaphoresKHR without a timeout: %d\n", wait_semaphores(device, &wait_info, UINT64_MAX));
    printf("vkWaitSemaphores for 0 ns: %d\n", vkWaitSemaphores(device, &wait_info, 0));

    /* A query read with VK_QUERY_RESULT_WAIT_BIT is polled without it (decision 4). */
    uint64_t results[4];
    const VkQueryPool pool = (VkQueryPool)(uintptr_t)0x9;
    printf("vkGetQueryPoolResults, waiting: %d\n",
           vkGetQueryPoolResults(device, pool, 0, 2, sizeof results, results, 16,
                                 VK_QUERY_RESULT_WAIT_BIT | VK_QUERY_RESULT_PARTIAL_BIT |
                                     VK_QUERY_RESULT_WITH_AVAILABILITY_BIT | VK_QUERY_RESULT_64_BIT));
    printf("vkGetQueryPoolResults: %d\n",
           vkGetQueryPoolResults(device, pool, 0, 2, sizeof results, results, 16, VK_QUERY_RESULT_64_BIT));

    vkDestroySemaphore(device, waited, NULL);
    vkDestroySemaphore(device, signaled, NULL);
    vkDestroySemaphore(device, binary, NULL);
    vkDestroySemaphore(device, timeline, NULL);
}

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
       structures before, between and after them, at Vulkan 1.0; then one with nothing. The fake sends the
       callbacks of the chain a message from vkCreateInstance and vkDestroyInstance. */
    const VkValidationCheckEXT checks[] = {VK_VALIDATION_CHECK_SHADERS_EXT, VK_VALIDATION_CHECK_ALL_EXT};
    const VkValidationFlagsEXT flags = {.sType = VK_STRUCTURE_TYPE_VALIDATION_FLAGS_EXT,
                                        .disabledValidationCheckCount = 2, .pDisabledValidationChecks = checks};
    const VkDirectDriverLoadingListLUNARG loading = {.sType = VK_STRUCTURE_TYPE_DIRECT_DRIVER_LOADING_LIST_LUNARG,
                                                     .pNext = &flags,
                                                     .mode = VK_DIRECT_DRIVER_LOADING_MODE_EXCLUSIVE_LUNARG};
    /* The callbacks not const, so the check after vkCreateInstance reads what libvulkan may have written. */
    VkDebugReportCallbackCreateInfoEXT report = {.sType = VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT,
                                                 .pNext = &loading, .flags = VK_DEBUG_REPORT_ERROR_BIT_EXT,
                                                 .pfnCallback = reports, .pUserData = &context};
    VkDebugUtilsMessengerCreateInfoEXT chained = messenger;
    chained.pNext = &report;
    const VkBool32 asynchronous = VK_FALSE;
    const VkLayerSettingEXT theirs[] = {
        {"MoltenVK", "MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS", VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &asynchronous}};
    const VkLayerSettingsCreateInfoEXT settings = {.sType = VK_STRUCTURE_TYPE_LAYER_SETTINGS_CREATE_INFO_EXT,
                                                   .pNext = &chained, .settingCount = 1, .pSettings = theirs};
    const VkValidationFeatureEnableEXT enabled[] = {VK_VALIDATION_FEATURE_ENABLE_BEST_PRACTICES_EXT};
    const VkValidationFeaturesEXT features = {.sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT, .pNext = &settings,
                                              .enabledValidationFeatureCount = 1,
                                              .pEnabledValidationFeatures = enabled};
    const VkApplicationInfo application = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                           .pApplicationName = "vulkan-fake-test", .applicationVersion = 7,
                                           .apiVersion = VK_API_VERSION_1_0};
    const char *extensions[] = {"VK_KHR_surface", "VK_KHR_android_surface", "VK_KHR_get_surface_capabilities2",
                                "VK_EXT_debug_utils", "VK_EXT_debug_report"};
    info = (VkInstanceCreateInfo){.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pNext = &features,
                                  .pApplicationInfo = &application, .enabledExtensionCount = 5,
                                  .ppEnabledExtensionNames = extensions};
    printf("vkCreateInstance: %d\n", vkCreateInstance(&info, &allocator, &instance));
    printf("  the app's callbacks as they were: %s\n",
           chained.pfnUserCallback == messages && chained.pUserData == &context && chained.pNext == &report &&
                   report.pfnCallback == reports && report.pUserData == &context && report.pNext == &loading
               ? "yes"
               : "no");
    info = (VkInstanceCreateInfo){.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    printf("vkCreateInstance without application info or extensions: %d\n", vkCreateInstance(&info, NULL, &bare));
    /* One with the surface only, whose chain starts with a callback. */
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
    INSTANCE(bare, vkCreateDebugReportCallbackEXT);
    printf("\nvkGetInstanceProcAddr of the instance with the surface only:");
    INSTANCE_EXPORT(surface_only, vkDestroySurfaceKHR);
    INSTANCE_EXPORT(surface_only, vkCreateAndroidSurfaceKHR);
    INSTANCE(surface_only, vkGetPhysicalDeviceSurfaceCapabilities2KHR);
    const VkInstance unknown_instance = (VkInstance)(void *)&count;
    printf("\nvkGetInstanceProcAddr of an instance libvulkan did not make:");
    INSTANCE_EXPORT(unknown_instance, vkEnumeratePhysicalDevices);
    printf("\n");

    callbacks(instance);

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

    /* What vkCreateDevice refuses, then a device with two queue families, the swapchain, the app's Vulkan 1.2
       features without timeline semaphores, a structure before the app's memory report and a (long ignored)
       device layer; a device with nothing; one with the app's timeline semaphore features off. */
    const float priorities[] = {1, 1};
    const VkDeviceQueueCreateInfo queue = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1,
                                           .pQueuePriorities = priorities};
    const VkDeviceQueueCreateInfo two_families[] = {
        {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0, .queueCount = 2,
         .pQueuePriorities = priorities},
        {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 1, .queueCount = 1,
         .pQueuePriorities = priorities},
    };
    const char *portability[] = {"VK_KHR_portability_subset"}, *instance_only[] = {"VK_EXT_debug_utils"};
    VkDevice device = VK_NULL_HANDLE, plain = VK_NULL_HANDLE, timelines = VK_NULL_HANDLE;
    VkDeviceCreateInfo device_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
                                      .pQueueCreateInfos = &queue, .enabledExtensionCount = 1,
                                      .ppEnabledExtensionNames = portability};
    printf("vkCreateDevice with VK_KHR_portability_subset: %d\n", vkCreateDevice(physical, &device_info, NULL, &device));
    device_info.ppEnabledExtensionNames = instance_only;
    printf("vkCreateDevice with VK_EXT_debug_utils: %d\n", vkCreateDevice(physical, &device_info, NULL, &device));
    VkDeviceDeviceMemoryReportCreateInfoEXT memory_report = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_DEVICE_MEMORY_REPORT_CREATE_INFO_EXT};
    VkPhysicalDevice16BitStorageFeatures storage = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES,
                                                    .pNext = &memory_report, .storageBuffer16BitAccess = VK_TRUE};
    /* The features not const, so the check after vkCreateDevice reads what libvulkan may have written. */
    VkPhysicalDeviceVulkan12Features vulkan12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                                 .pNext = &storage, .drawIndirectCount = VK_TRUE};
    const char *device_extensions[] = {"VK_KHR_swapchain", "VK_KHR_incremental_present", "VK_KHR_16bit_storage",
                                       "VK_KHR_draw_indirect_count"};
    device_info = (VkDeviceCreateInfo){.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &vulkan12,
                                       .queueCreateInfoCount = 2, .pQueueCreateInfos = two_families,
                                       .enabledLayerCount = 1, .ppEnabledLayerNames = layer, .enabledExtensionCount = 4,
                                       .ppEnabledExtensionNames = device_extensions};
    printf("vkCreateDevice: %d\n", vkCreateDevice(physical, &device_info, &allocator, &device));
    device_info = (VkDeviceCreateInfo){.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
                                       .pQueueCreateInfos = &queue};
    printf("vkCreateDevice with nothing: %d\n", vkCreateDevice(physical, &device_info, NULL, &plain));
    VkPhysicalDeviceTimelineSemaphoreFeatures timeline_off = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
    device_info.pNext = &timeline_off;
    printf("vkCreateDevice with the app's timeline semaphore features off: %d\n",
           vkCreateDevice(physical, &device_info, NULL, &timelines));
    printf("  the app's features as they were: %s\n",
           vulkan12.timelineSemaphore == VK_FALSE && vulkan12.pNext == &storage &&
                   timeline_off.timelineSemaphore == VK_FALSE && timeline_off.pNext == NULL
               ? "yes"
               : "no");
    vkDestroyDevice(timelines, NULL);

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

    queues(device);

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
