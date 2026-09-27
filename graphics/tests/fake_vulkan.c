/* A host library for test 6 of ADR-0030 (graphics.vulkan_fake), which graphics/host opens as MoltenVK: the
   functions that libvulkan's functions written by hand reach, with lists of extensions and layers that
   hold names libvulkan must hide. What arrives is printed to stderr, so the test sees what libvulkan passes
   on: the create infos with their chains, the semaphores, the batches of every submit, the waits and their
   timeouts, the flags of the queries it reads, the debug callbacks. Its vkGet*ProcAddr gate the debug
   extensions and VK_KHR_draw_indirect_count by what was enabled, as MoltenVK does.
   - Its waits answer VK_TIMEOUT to a timeout of 10 ms twice, then VK_SUCCESS, and VK_TIMEOUT to any other
     timeout at once; its queries answer VK_NOT_READY twice without VK_QUERY_RESULT_WAIT_BIT, then
     VK_SUCCESS.
   - It sends every debug callback it gets a message on the calling thread and one from a thread of its own,
     where graphics/host must not call the app: those of vkCreateInstance's chain from vkCreateInstance and
     vkDestroyInstance, the others when the app sends a message.
   - Its fences are markers (it makes none): FAILING fails a submit; SCHEDULED keeps a submit here while a
     thread of the fake's own asks graphics/host for "scheduled", and the next wait prints its answer and
     whether it came after the submit left. */
#define VK_USE_PLATFORM_METAL_EXT
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define EXPORTED __attribute__((visibility("default")))

#define FAILING ((VkFence)(uintptr_t)0xfa11)
#define SCHEDULED ((VkFence)(uintptr_t)0x5c4ed)
#define SLICE 10000000u /* ns */

/* Its lists, in no particular order, with names libvulkan offers and names it hides, each with a name of the
   other kind among them. */
static const VkExtensionProperties instance_extensions[] = {
    {"VK_KHR_surface", 25}, {"VK_EXT_debug_utils", 2}, {"VK_EXT_metal_surface", 1},
    {"VK_KHR_get_physical_device_properties2", 2}, {"VK_EXT_layer_settings", 2}, {"VK_EXT_debug_report", 10},
    {"VK_KHR_portability_enumeration", 1}, {"VK_MVK_macos_surface", 3}, {"VK_KHR_get_surface_capabilities2", 1},
    {"VK_EXT_headless_surface", 1}, {"VK_KHR_device_group_creation", 1}, {"VK_EXT_surface_maintenance1", 1},
    {"VK_KHR_16bit_storage", 1},
};
static const VkExtensionProperties device_extensions[] = {
    {"VK_KHR_swapchain", 70}, {"VK_KHR_16bit_storage", 1}, {"VK_KHR_portability_subset", 1},
    {"VK_KHR_incremental_present", 2}, {"VK_EXT_metal_objects", 2}, {"VK_GOOGLE_display_timing", 1},
    {"VK_KHR_draw_indirect_count", 1}, {"VK_KHR_present_id", 1}, {"VK_EXT_calibrated_timestamps", 2},
    {"VK_KHR_maintenance1", 2}, {"VK_EXT_debug_utils", 2},
};
static const VkLayerProperties layer = {"VK_LAYER_fake", VK_API_VERSION_1_4, 1, "a layer to hide"};

struct instance {
    int debug_report, debug_utils;
};
struct queue {
    uint32_t family, index;
};
struct device {
    int draw_indirect_count;
    struct queue queues[4][4];
};
static int physical_device;
static uint32_t api = VK_API_VERSION_1_0;
static unsigned semaphores;  /* how many it made: semaphore n is the handle n, printed as sn */

static VkResult list(const void *items, size_t size, uint32_t length, uint32_t *count, void *properties) {
    if (properties == NULL) {
        *count = length;
        return VK_SUCCESS;
    }
    const uint32_t copied = *count < length ? *count : length;
    memcpy(properties, items, copied * size);
    *count = copied;
    return copied < length ? VK_INCOMPLETE : VK_SUCCESS;
}

static int enabled(uint32_t count, const char *const *names, const char *name) {
    for (uint32_t index = 0; index < count; index++) {
        if (strcmp(names[index], name) == 0) {
            return 1;
        }
    }
    return 0;
}

static const char *text(const char *string) {
    return string != NULL ? string : "NULL";
}

static const void *find(const void *chain, VkStructureType type) {
    for (const VkBaseInStructure *structure = chain; structure != NULL; structure = structure->pNext) {
        if (structure->sType == type) {
            return structure;
        }
    }
    return NULL;
}

/* Whether a function lies in graphics-run, whose graphics/host opened the fake, rather than in the app. */
static const char *whose(const void *function) {
    Dl_info info;
    return dladdr(function, &info) && info.dli_fbase == (const void *)_dyld_get_image_header(0) ? "graphics/host's"
                                                                                                 : "not graphics/host's";
}

static void names(const char *what, uint32_t count, const char *const *given) {
    fprintf(stderr, "fake:   %s:", what);
    for (uint32_t index = 0; index < count; index++) {
        fprintf(stderr, " %s", given[index]);
    }
    fprintf(stderr, "%s\n", count == 0 ? " none" : "");
}

/* A pNext chain, one structure per line. */
static void chain(const void *next) {
    fprintf(stderr, "fake:   chain:%s\n", next == NULL ? " empty" : "");
    for (const VkBaseInStructure *structure = next; structure != NULL; structure = structure->pNext) {
        switch (structure->sType) {
        case VK_STRUCTURE_TYPE_LAYER_SETTINGS_CREATE_INFO_EXT: {
            const VkLayerSettingsCreateInfoEXT *settings = (const void *)structure;
            fprintf(stderr, "fake:     layer settings, %u:\n", settings->settingCount);
            for (uint32_t index = 0; index < settings->settingCount; index++) {
                const VkLayerSettingEXT *setting = &settings->pSettings[index];
                fprintf(stderr, "fake:       %s %s type %d, %u values, first %u\n", setting->pLayerName,
                        setting->pSettingName, setting->type, setting->valueCount,
                        *(const uint32_t *)setting->pValues);
            }
            break;
        }
        case VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT: {
            const VkValidationFeaturesEXT *features = (const void *)structure;
            fprintf(stderr, "fake:     validation features, %u enabled, %u disabled\n",
                    features->enabledValidationFeatureCount, features->disabledValidationFeatureCount);
            break;
        }
        case VK_STRUCTURE_TYPE_VALIDATION_FLAGS_EXT:
            fprintf(stderr, "fake:     validation flags, %u disabled\n",
                    ((const VkValidationFlagsEXT *)(const void *)structure)->disabledValidationCheckCount);
            break;
        case VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT: {
            const VkDebugUtilsMessengerCreateInfoEXT *messenger = (const void *)structure;
            fprintf(stderr, "fake:     debug utils messenger, severities 0x%x, types 0x%x, callback %s, user data %s\n",
                    messenger->messageSeverity, messenger->messageType, whose((const void *)messenger->pfnUserCallback),
                    messenger->pUserData != NULL ? "given" : "NULL");
            break;
        }
        case VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT: {
            const VkDebugReportCallbackCreateInfoEXT *report = (const void *)structure;
            fprintf(stderr, "fake:     debug report callback, flags 0x%x, callback %s, user data %s\n", report->flags,
                    whose((const void *)report->pfnCallback), report->pUserData != NULL ? "given" : "NULL");
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES:
            fprintf(stderr, "fake:     16-bit storage features, storageBuffer16BitAccess %u\n",
                    ((const VkPhysicalDevice16BitStorageFeatures *)(const void *)structure)->storageBuffer16BitAccess);
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES: {
            const VkPhysicalDeviceVulkan12Features *features = (const void *)structure;
            fprintf(stderr, "fake:     Vulkan 1.2 features, timelineSemaphore %u, drawIndirectCount %u\n",
                    features->timelineSemaphore, features->drawIndirectCount);
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES:
            fprintf(stderr, "fake:     timeline semaphore features, timelineSemaphore %u\n",
                    ((const VkPhysicalDeviceTimelineSemaphoreFeatures *)(const void *)structure)->timelineSemaphore);
            break;
        default:
            fprintf(stderr, "fake:     sType %d\n", structure->sType);
            break;
        }
    }
}

/* A debug callback it got: from vkCreateInstance's chain (for that instance), or from a create function (with
   the handle it gave). */
struct listener {
    struct listener *next;
    const void *instance;
    uint64_t handle;
    PFN_vkDebugUtilsMessengerCallbackEXT messenger;
    PFN_vkDebugReportCallbackEXT report;
    void *user;
};
static struct listener *listeners;
static uint64_t handles;

static void listen(const void *instance, uint64_t handle, PFN_vkDebugUtilsMessengerCallbackEXT messenger,
                   PFN_vkDebugReportCallbackEXT report, void *user) {
    struct listener *listener = calloc(1, sizeof *listener);
    *listener = (struct listener){listeners, instance, handle, messenger, report, user};
    listeners = listener;
}

static void forget(const void *instance, uint64_t handle) {
    for (struct listener **listener = &listeners; *listener != NULL;) {
        if ((*listener)->instance == instance && (*listener)->handle == handle) {
            struct listener *gone = *listener;
            *listener = gone->next;
            free(gone);
        } else {
            listener = &(*listener)->next;
        }
    }
}

struct message {
    const struct listener *listener;
    const char *text;
};

static void *deliver(void *context) {
    const struct message *message = context;
    const struct listener *listener = message->listener;
    if (listener->messenger != NULL) {
        const VkDebugUtilsMessengerCallbackDataEXT data = {
            .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CALLBACK_DATA_EXT, .pMessageIdName = "fake",
            .messageIdNumber = 7, .pMessage = message->text};
        listener->messenger(VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT, VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT,
                            &data, listener->user);
    } else {
        listener->report(VK_DEBUG_REPORT_ERROR_BIT_EXT, VK_DEBUG_REPORT_OBJECT_TYPE_INSTANCE_EXT, 42, 3, 7, "fake",
                         message->text, listener->user);
    }
    return NULL;
}

/* A message to the listeners of an instance's chain (handle 0) or to those with handles: first on the calling
   thread, then from a thread of the fake's own. */
static void send(const void *instance, int with_handles, int messengers, const char *text) {
    for (const struct listener *listener = listeners; listener != NULL; listener = listener->next) {
        if ((instance == NULL || listener->instance == instance) && (listener->handle != 0) == with_handles &&
            (listener->messenger != NULL) == messengers) {
            struct message message = {listener, text};
            pthread_t thread;
            deliver(&message);
            char other[128];
            snprintf(other, sizeof other, "%s, from another thread", text);
            message.text = other;
            pthread_create(&thread, NULL, deliver, &message);
            pthread_join(thread, NULL);
        }
    }
}

EXPORTED VkResult vkEnumerateInstanceVersion(uint32_t *version) {
    *version = VK_MAKE_API_VERSION(0, 1, 4, 999);
    return VK_SUCCESS;
}

EXPORTED VkResult vkEnumerateInstanceLayerProperties(uint32_t *count, VkLayerProperties *properties) {
    return list(&layer, sizeof layer, 1, count, properties);
}

EXPORTED VkResult vkEnumerateInstanceExtensionProperties(const char *name, uint32_t *count,
                                                         VkExtensionProperties *properties) {
    if (name != NULL) {
        return VK_ERROR_LAYER_NOT_PRESENT;
    }
    return list(instance_extensions, sizeof *instance_extensions,
                sizeof instance_extensions / sizeof *instance_extensions, count, properties);
}

EXPORTED VkResult vkCreateInstance(const VkInstanceCreateInfo *info, const VkAllocationCallbacks *allocator,
                                   VkInstance *instance) {
    const VkApplicationInfo *application = info->pApplicationInfo;
    fprintf(stderr, "fake: vkCreateInstance, allocator %s, flags 0x%x\n", allocator != NULL ? "given" : "NULL",
            info->flags);
    if (application == NULL) {
        fprintf(stderr, "fake:   no application info\n");
    } else {
        fprintf(stderr, "fake:   application %s %u, engine %s %u, API %u.%u.%u\n",
                text(application->pApplicationName), application->applicationVersion,
                text(application->pEngineName), application->engineVersion, VK_API_VERSION_MAJOR(application->apiVersion),
                VK_API_VERSION_MINOR(application->apiVersion), VK_API_VERSION_PATCH(application->apiVersion));
        api = application->apiVersion;
    }
    names("layers", info->enabledLayerCount, info->ppEnabledLayerNames);
    names("extensions", info->enabledExtensionCount, info->ppEnabledExtensionNames);
    chain(info->pNext);
    struct instance *created = calloc(1, sizeof *created);
    created->debug_report = enabled(info->enabledExtensionCount, info->ppEnabledExtensionNames, "VK_EXT_debug_report");
    created->debug_utils = enabled(info->enabledExtensionCount, info->ppEnabledExtensionNames, "VK_EXT_debug_utils");
    /* The callbacks of the chain hear from vkCreateInstance and vkDestroyInstance, as MoltenVK's do. */
    for (const VkBaseInStructure *structure = info->pNext; structure != NULL; structure = structure->pNext) {
        if (structure->sType == VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT) {
            const VkDebugUtilsMessengerCreateInfoEXT *messenger = (const void *)structure;
            listen(created, 0, messenger->pfnUserCallback, NULL, messenger->pUserData);
        } else if (structure->sType == VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT) {
            const VkDebugReportCallbackCreateInfoEXT *report = (const void *)structure;
            listen(created, 0, NULL, report->pfnCallback, report->pUserData);
        }
    }
    send(created, 0, 1, "a message from vkCreateInstance");
    send(created, 0, 0, "a report from vkCreateInstance");
    *instance = (VkInstance)(void *)created;
    return VK_SUCCESS;
}

EXPORTED void vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks *allocator) {
    fprintf(stderr, "fake: vkDestroyInstance, allocator %s\n", allocator != NULL ? "given" : "NULL");
    send(instance, 0, 1, "a message from vkDestroyInstance");
    send(instance, 0, 0, "a report from vkDestroyInstance");
    forget(instance, 0);
    free((void *)instance);
}

EXPORTED VkResult vkEnumeratePhysicalDevices(VkInstance instance, uint32_t *count, VkPhysicalDevice *devices) {
    (void)instance;
    VkPhysicalDevice device = (VkPhysicalDevice)(void *)&physical_device;
    return list(&device, sizeof device, 1, count, devices);
}

EXPORTED void vkGetPhysicalDeviceProperties(VkPhysicalDevice physical, VkPhysicalDeviceProperties *properties) {
    (void)physical;
    memset(properties, 0, sizeof *properties);
    properties->apiVersion = VK_MAKE_API_VERSION(0, VK_API_VERSION_MAJOR(api), VK_API_VERSION_MINOR(api), 999);
    strcpy(properties->deviceName, "Minato's fake Vulkan");
}

EXPORTED VkResult vkEnumerateDeviceLayerProperties(VkPhysicalDevice physical, uint32_t *count,
                                                   VkLayerProperties *properties) {
    (void)physical;
    return list(&layer, sizeof layer, 1, count, properties);
}

EXPORTED VkResult vkEnumerateDeviceExtensionProperties(VkPhysicalDevice physical, const char *name, uint32_t *count,
                                                       VkExtensionProperties *properties) {
    (void)physical;
    if (name != NULL) {
        return VK_ERROR_LAYER_NOT_PRESENT;
    }
    return list(device_extensions, sizeof *device_extensions, sizeof device_extensions / sizeof *device_extensions,
                count, properties);
}

EXPORTED VkResult vkCreateDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo *info,
                                 const VkAllocationCallbacks *allocator, VkDevice *device) {
    (void)physical;
    fprintf(stderr, "fake: vkCreateDevice, allocator %s, %u queue families, features %s\n",
            allocator != NULL ? "given" : "NULL", info->queueCreateInfoCount,
            info->pEnabledFeatures != NULL ? "given" : "NULL");
    names("layers", info->enabledLayerCount, info->ppEnabledLayerNames);
    names("extensions", info->enabledExtensionCount, info->ppEnabledExtensionNames);
    chain(info->pNext);
    struct device *created = calloc(1, sizeof *created);
    created->draw_indirect_count =
        enabled(info->enabledExtensionCount, info->ppEnabledExtensionNames, "VK_KHR_draw_indirect_count");
    *device = (VkDevice)(void *)created;
    return VK_SUCCESS;
}

EXPORTED void vkDestroyDevice(VkDevice device, const VkAllocationCallbacks *allocator) {
    fprintf(stderr, "fake: vkDestroyDevice, allocator %s\n", allocator != NULL ? "given" : "NULL");
    free((void *)device);
}

EXPORTED void vkGetDeviceQueue(VkDevice device, uint32_t family, uint32_t index, VkQueue *queue) {
    struct device *fake = (void *)device;
    fake->queues[family][index] = (struct queue){family, index};
    *queue = (VkQueue)(void *)&fake->queues[family][index];
}

EXPORTED void vkGetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2 *info, VkQueue *queue) {
    vkGetDeviceQueue(device, info->queueFamilyIndex, info->queueIndex, queue);
}

/* A queue's name: its family and index. */
static const char *named(VkQueue queue) {
    static char name[32];
    const struct queue *fake = (const void *)queue;
    snprintf(name, sizeof name, "queue %u.%u", fake->family, fake->index);
    return name;
}

EXPORTED void vkExportMetalObjectsEXT(VkDevice device, VkExportMetalObjectsInfoEXT *info) {
    (void)device;
    for (VkBaseOutStructure *structure = (void *)info->pNext; structure != NULL; structure = structure->pNext) {
        if (structure->sType == VK_STRUCTURE_TYPE_EXPORT_METAL_COMMAND_QUEUE_INFO_EXT) {
            VkExportMetalCommandQueueInfoEXT *exported = (void *)structure;
            fprintf(stderr, "fake: vkExportMetalObjectsEXT, the command queue of %s\n", named(exported->queue));
            exported->mtlCommandQueue = NULL;  /* none: the fake has no Metal */
        } else {
            fprintf(stderr, "fake: vkExportMetalObjectsEXT, sType %d\n", structure->sType);
        }
    }
}

EXPORTED VkResult vkCreateSemaphore(VkDevice device, const VkSemaphoreCreateInfo *info,
                                    const VkAllocationCallbacks *allocator, VkSemaphore *semaphore) {
    (void)device;
    const VkSemaphoreTypeCreateInfo *type = find(info->pNext, VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO);
    *semaphore = (VkSemaphore)(uintptr_t)++semaphores;
    if (type != NULL && type->semaphoreType == VK_SEMAPHORE_TYPE_TIMELINE) {
        fprintf(stderr, "fake: vkCreateSemaphore, allocator %s, a timeline from %llu: s%u\n",
                allocator != NULL ? "given" : "NULL", (unsigned long long)type->initialValue, semaphores);
    } else {
        fprintf(stderr, "fake: vkCreateSemaphore, allocator %s, binary: s%u\n", allocator != NULL ? "given" : "NULL",
                semaphores);
    }
    return VK_SUCCESS;
}

EXPORTED void vkDestroySemaphore(VkDevice device, VkSemaphore semaphore, const VkAllocationCallbacks *allocator) {
    (void)device;
    fprintf(stderr, "fake: vkDestroySemaphore s%u, allocator %s\n", (unsigned)(uintptr_t)semaphore,
            allocator != NULL ? "given" : "NULL");
}

/* "Scheduled" asked while a submit is here: graphics/host found through the process, since it is linked into
   graphics-run. */
static pthread_t asker;
static int asked, answer, answered_after;
static atomic_int asking, left;

static void *ask(void *unused) {
    (void)unused;
    int (*scheduled)(uint64_t) = (int (*)(uint64_t))dlsym(RTLD_DEFAULT, "minato_graphics_vulkan_scheduled");
    atomic_store(&asking, 1);
    answer = scheduled != NULL ? scheduled(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + 1000000000u) : -1;
    answered_after = atomic_load(&left);
    return NULL;
}

/* A submit's end by its marker fence. With SCHEDULED it leaves only 50 ms after the asking thread started to
   ask, so an answer before it left shows. */
static VkResult submitted(VkFence fence) {
    if (fence == FAILING) {
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    if (fence == SCHEDULED) {
        atomic_store(&asking, 0);
        atomic_store(&left, 0);
        pthread_create(&asker, NULL, ask, NULL);
        asked = 1;
        while (!atomic_load(&asking)) {
            usleep(100);
        }
        usleep(50000);
        atomic_store(&left, 1);
    }
    return VK_SUCCESS;
}

/* The answer to the last "scheduled", printed by the wait after the submit: never inside a submit, which
   graphics/host counts, so "scheduled" would wait for it. */
static void settle(void) {
    if (asked) {
        pthread_join(asker, NULL);
        asked = 0;
        fprintf(stderr, "fake: \"scheduled\" answered %d, %s the submit left\n", answer,
                answered_after ? "after" : "before");
    }
}

static const char *fenced(VkFence fence) {
    return fence == VK_NULL_HANDLE ? "none" : fence == FAILING ? "failing" : fence == SCHEDULED ? "scheduled" : "given";
}

/* Semaphores with their values, where given. */
static void semaphore_list(const char *what, uint32_t count, const VkSemaphore *given, uint32_t values,
                           const uint64_t *value) {
    fprintf(stderr, " %s", what);
    for (uint32_t index = 0; index < count; index++) {
        fprintf(stderr, " s%u", (unsigned)(uintptr_t)given[index]);
        if (index < values) {
            fprintf(stderr, "=%llu", (unsigned long long)value[index]);
        }
    }
    fprintf(stderr, "%s", count == 0 ? " none" : "");
}

/* A batch's semaphores, with the values of its VkTimelineSemaphoreSubmitInfo and the device indices of its
   VkDeviceGroupSubmitInfo, and each count that differs from its array's. */
static void batch(uint32_t index, const void *next, uint32_t waits, const VkSemaphore *waited, uint32_t buffers,
                  uint32_t signals, const VkSemaphore *signaled) {
    const VkTimelineSemaphoreSubmitInfo *values = find(next, VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO);
    const VkDeviceGroupSubmitInfo *group = find(next, VK_STRUCTURE_TYPE_DEVICE_GROUP_SUBMIT_INFO);
    fprintf(stderr, "fake:   %u:", index);
    semaphore_list("waits", waits, waited, values != NULL ? values->waitSemaphoreValueCount : 0,
                   values != NULL ? values->pWaitSemaphoreValues : NULL);
    fprintf(stderr, ", %u command buffers,", buffers);
    semaphore_list("signals", signals, signaled, values != NULL ? values->signalSemaphoreValueCount : 0,
                   values != NULL ? values->pSignalSemaphoreValues : NULL);
    if (values != NULL && values->waitSemaphoreValueCount != 0 && values->waitSemaphoreValueCount != waits) {
        fprintf(stderr, ", %u wait values", values->waitSemaphoreValueCount);
    }
    if (values != NULL && values->signalSemaphoreValueCount != signals) {
        fprintf(stderr, ", %u signal values", values->signalSemaphoreValueCount);
    }
    if (group != NULL) {
        fprintf(stderr, ", on devices");
        for (uint32_t signal = 0; signal < group->signalSemaphoreCount; signal++) {
            fprintf(stderr, " %u", group->pSignalSemaphoreDeviceIndices[signal]);
        }
        if (group->signalSemaphoreCount != signals) {
            fprintf(stderr, " (%u)", group->signalSemaphoreCount);
        }
    }
    fprintf(stderr, "\n");
}

EXPORTED VkResult vkQueueSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo *submits, VkFence fence) {
    fprintf(stderr, "fake: vkQueueSubmit on %s, %u batches, fence %s\n", named(queue), count, fenced(fence));
    for (uint32_t index = 0; index < count; index++) {
        const VkSubmitInfo *submit = &submits[index];
        batch(index, submit->pNext, submit->waitSemaphoreCount, submit->pWaitSemaphores, submit->commandBufferCount,
              submit->signalSemaphoreCount, submit->pSignalSemaphores);
    }
    return submitted(fence);
}

EXPORTED VkResult vkQueueSubmit2(VkQueue queue, uint32_t count, const VkSubmitInfo2 *submits, VkFence fence) {
    fprintf(stderr, "fake: vkQueueSubmit2 on %s, %u batches, fence %s\n", named(queue), count, fenced(fence));
    for (uint32_t index = 0; index < count; index++) {
        const VkSubmitInfo2 *submit = &submits[index];
        fprintf(stderr, "fake:   %u: waits", index);
        for (uint32_t wait = 0; wait < submit->waitSemaphoreInfoCount; wait++) {
            const VkSemaphoreSubmitInfo *info = &submit->pWaitSemaphoreInfos[wait];
            fprintf(stderr, " s%u=%llu", (unsigned)(uintptr_t)info->semaphore, (unsigned long long)info->value);
        }
        fprintf(stderr, "%s, %u command buffers, signals", submit->waitSemaphoreInfoCount == 0 ? " none" : "",
                submit->commandBufferInfoCount);
        for (uint32_t signal = 0; signal < submit->signalSemaphoreInfoCount; signal++) {
            const VkSemaphoreSubmitInfo *info = &submit->pSignalSemaphoreInfos[signal];
            fprintf(stderr, " s%u=%llu at 0x%llx on device %u", (unsigned)(uintptr_t)info->semaphore,
                    (unsigned long long)info->value, (unsigned long long)info->stageMask, info->deviceIndex);
        }
        fprintf(stderr, "\n");
    }
    return submitted(fence);
}

EXPORTED VkResult vkQueueSubmit2KHR(VkQueue queue, uint32_t count, const VkSubmitInfo2 *submits, VkFence fence) {
    fprintf(stderr, "fake: vkQueueSubmit2KHR\n");
    return vkQueueSubmit2(queue, count, submits, fence);
}

EXPORTED VkResult vkQueueBindSparse(VkQueue queue, uint32_t count, const VkBindSparseInfo *binds, VkFence fence) {
    fprintf(stderr, "fake: vkQueueBindSparse on %s, %u batches, fence %s\n", named(queue), count, fenced(fence));
    for (uint32_t index = 0; index < count; index++) {
        const VkBindSparseInfo *bind = &binds[index];
        batch(index, bind->pNext, bind->waitSemaphoreCount, bind->pWaitSemaphores, 0, bind->signalSemaphoreCount,
              bind->pSignalSemaphores);
    }
    return submitted(fence);
}

EXPORTED VkResult vkQueueWaitIdle(VkQueue queue) {
    fprintf(stderr, "fake: vkQueueWaitIdle on %s\n", named(queue));
    return VK_SUCCESS;
}

EXPORTED VkResult vkDeviceWaitIdle(VkDevice device) {
    (void)device;
    fprintf(stderr, "fake: vkDeviceWaitIdle\n");
    return VK_SUCCESS;
}

/* A wait's answer: a slice of 10 ms times out twice, then succeeds; any other timeout times out at once. */
static unsigned slices;

static VkResult waited(uint64_t timeout) {
    if (timeout != SLICE) {
        return VK_TIMEOUT;
    }
    if (++slices < 3) {
        return VK_TIMEOUT;
    }
    slices = 0;
    return VK_SUCCESS;
}

EXPORTED VkResult vkWaitSemaphores(VkDevice device, const VkSemaphoreWaitInfo *info, uint64_t timeout) {
    (void)device;
    settle();
    fprintf(stderr, "fake: vkWaitSemaphores, %s:", info->flags & VK_SEMAPHORE_WAIT_ANY_BIT ? "any" : "all");
    for (uint32_t index = 0; index < info->semaphoreCount; index++) {
        fprintf(stderr, " s%u=%llu", (unsigned)(uintptr_t)info->pSemaphores[index],
                (unsigned long long)info->pValues[index]);
    }
    fprintf(stderr, ", timeout %llu\n", (unsigned long long)timeout);
    return waited(timeout);
}

EXPORTED VkResult vkWaitSemaphoresKHR(VkDevice device, const VkSemaphoreWaitInfo *info, uint64_t timeout) {
    fprintf(stderr, "fake: vkWaitSemaphoresKHR\n");
    return vkWaitSemaphores(device, info, timeout);
}

EXPORTED VkResult vkWaitForFences(VkDevice device, uint32_t count, const VkFence *fences, VkBool32 all,
                                  uint64_t timeout) {
    (void)device, (void)fences;
    settle();
    fprintf(stderr, "fake: vkWaitForFences, %u fences, %s, timeout %llu\n", count, all ? "all" : "any",
            (unsigned long long)timeout);
    return waited(timeout);
}

/* A query's answer: without VK_QUERY_RESULT_WAIT_BIT not ready twice, then ready. */
static unsigned polls;

EXPORTED VkResult vkGetQueryPoolResults(VkDevice device, VkQueryPool pool, uint32_t first, uint32_t count, size_t size,
                                        void *data, VkDeviceSize stride, VkQueryResultFlags flags) {
    (void)device, (void)pool, (void)size, (void)data, (void)stride;
    fprintf(stderr, "fake: vkGetQueryPoolResults, queries %u to %u, flags 0x%x\n", first, first + count - 1, flags);
    if (!(flags & VK_QUERY_RESULT_WAIT_BIT) && ++polls < 3) {
        return VK_NOT_READY;
    }
    polls = 0;
    return VK_SUCCESS;
}

EXPORTED VkResult vkCreateDebugUtilsMessengerEXT(VkInstance instance, const VkDebugUtilsMessengerCreateInfoEXT *info,
                                                 const VkAllocationCallbacks *allocator,
                                                 VkDebugUtilsMessengerEXT *messenger) {
    fprintf(stderr, "fake: vkCreateDebugUtilsMessengerEXT, allocator %s, severities 0x%x, types 0x%x, callback %s, "
            "user data %s\n", allocator != NULL ? "given" : "NULL", info->messageSeverity, info->messageType,
            whose((const void *)info->pfnUserCallback), info->pUserData != NULL ? "given" : "NULL");
    *messenger = (VkDebugUtilsMessengerEXT)(uintptr_t)++handles;
    listen(instance, handles, info->pfnUserCallback, NULL, info->pUserData);
    return VK_SUCCESS;
}

EXPORTED void vkDestroyDebugUtilsMessengerEXT(VkInstance instance, VkDebugUtilsMessengerEXT messenger,
                                              const VkAllocationCallbacks *allocator) {
    fprintf(stderr, "fake: vkDestroyDebugUtilsMessengerEXT, allocator %s\n", allocator != NULL ? "given" : "NULL");
    forget(instance, (uint64_t)(uintptr_t)messenger);
}

EXPORTED void vkSubmitDebugUtilsMessageEXT(VkInstance instance, VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                           VkDebugUtilsMessageTypeFlagsEXT types,
                                           const VkDebugUtilsMessengerCallbackDataEXT *data) {
    fprintf(stderr, "fake: vkSubmitDebugUtilsMessageEXT, severity 0x%x, types 0x%x\n", severity, types);
    send(instance, 1, 1, data->pMessage);
}

EXPORTED VkResult vkCreateDebugReportCallbackEXT(VkInstance instance, const VkDebugReportCallbackCreateInfoEXT *info,
                                                 const VkAllocationCallbacks *allocator,
                                                 VkDebugReportCallbackEXT *callback) {
    fprintf(stderr, "fake: vkCreateDebugReportCallbackEXT, allocator %s, flags 0x%x, callback %s, user data %s\n",
            allocator != NULL ? "given" : "NULL", info->flags, whose((const void *)info->pfnCallback),
            info->pUserData != NULL ? "given" : "NULL");
    *callback = (VkDebugReportCallbackEXT)(uintptr_t)++handles;
    listen(instance, handles, NULL, info->pfnCallback, info->pUserData);
    return VK_SUCCESS;
}

EXPORTED void vkDestroyDebugReportCallbackEXT(VkInstance instance, VkDebugReportCallbackEXT callback,
                                              const VkAllocationCallbacks *allocator) {
    fprintf(stderr, "fake: vkDestroyDebugReportCallbackEXT, allocator %s\n", allocator != NULL ? "given" : "NULL");
    forget(instance, (uint64_t)(uintptr_t)callback);
}

EXPORTED void vkDebugReportMessageEXT(VkInstance instance, VkDebugReportFlagsEXT flags,
                                      VkDebugReportObjectTypeEXT type, uint64_t object, size_t location, int32_t code,
                                      const char *prefix, const char *message) {
    (void)object, (void)location, (void)code;
    fprintf(stderr, "fake: vkDebugReportMessageEXT, flags 0x%x, object type %d, prefix %s\n", flags, type, prefix);
    send(instance, 1, 0, message);
}

EXPORTED void vkCmdDrawIndirectCountKHR(VkCommandBuffer buffer, VkBuffer draws, VkDeviceSize offset, VkBuffer counts,
                                        VkDeviceSize count_offset, uint32_t most, uint32_t stride) {
    (void)buffer, (void)draws, (void)offset, (void)counts, (void)count_offset, (void)most, (void)stride;
}

/* One of its own functions by name. RTLD_SELF would not do: a tail call to dlsym, as a release build makes
   it, would search from the caller's image. */
static PFN_vkVoidFunction own(const char *name) {
    static void *self;
    Dl_info info;
    if (self == NULL && dladdr(&physical_device, &info) != 0) {
        self = dlopen(info.dli_fname, RTLD_NOW | RTLD_NOLOAD | RTLD_FIRST);
    }
    return self != NULL ? (PFN_vkVoidFunction)dlsym(self, name) : NULL;
}

EXPORTED PFN_vkVoidFunction vkGetInstanceProcAddr(VkInstance instance, const char *name) {
    const struct instance *fake = (const void *)instance;
    if ((strstr(name, "DebugReport") != NULL && (fake == NULL || !fake->debug_report)) ||
        (strstr(name, "DebugUtilsMessenger") != NULL && (fake == NULL || !fake->debug_utils))) {
        return NULL;
    }
    return own(name);
}

EXPORTED PFN_vkVoidFunction vkGetDeviceProcAddr(VkDevice device, const char *name) {
    const struct device *fake = (const void *)device;
    if (strcmp(name, "vkCmdDrawIndirectCountKHR") == 0 && !fake->draw_indirect_count) {
        return NULL;
    }
    return own(name);
}
