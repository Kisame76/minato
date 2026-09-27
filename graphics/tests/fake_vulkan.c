/* A host library for test 6 of ADR-0030 (graphics.vulkan_fake), which graphics/host opens as MoltenVK: the
   functions that libvulkan's instance and device functions reach, with lists of extensions and layers that
   hold names libvulkan must hide. What arrives at vkCreateInstance, vkCreateDevice and the destroy calls is
   printed to stderr, so the test sees what libvulkan passes on. Its vkGet*ProcAddr gate the debug
   extensions and VK_KHR_draw_indirect_count by what was enabled, as MoltenVK does. */
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EXPORTED __attribute__((visibility("default")))

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
struct device {
    int draw_indirect_count;
};
static int physical_device;
static uint32_t api = VK_API_VERSION_1_0;

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
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES:
            fprintf(stderr, "fake:     16-bit storage features, storageBuffer16BitAccess %u\n",
                    ((const VkPhysicalDevice16BitStorageFeatures *)(const void *)structure)->storageBuffer16BitAccess);
            break;
        default:
            fprintf(stderr, "fake:     sType %d\n", structure->sType);
            break;
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
    *instance = (VkInstance)(void *)created;
    return VK_SUCCESS;
}

EXPORTED void vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks *allocator) {
    fprintf(stderr, "fake: vkDestroyInstance, allocator %s\n", allocator != NULL ? "given" : "NULL");
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

/* Functions for vkGet*ProcAddr to find, which say so if they are ever called. */
EXPORTED VkResult vkCreateDebugUtilsMessengerEXT(VkInstance instance, const VkDebugUtilsMessengerCreateInfoEXT *info,
                                                 const VkAllocationCallbacks *allocator,
                                                 VkDebugUtilsMessengerEXT *messenger) {
    (void)instance, (void)info, (void)allocator, (void)messenger;
    fprintf(stderr, "fake: vkCreateDebugUtilsMessengerEXT\n");
    return VK_ERROR_FEATURE_NOT_PRESENT;
}

EXPORTED void vkDestroyDebugUtilsMessengerEXT(VkInstance instance, VkDebugUtilsMessengerEXT messenger,
                                              const VkAllocationCallbacks *allocator) {
    (void)instance, (void)messenger, (void)allocator;
    fprintf(stderr, "fake: vkDestroyDebugUtilsMessengerEXT\n");
}

EXPORTED VkResult vkCreateDebugReportCallbackEXT(VkInstance instance, const VkDebugReportCallbackCreateInfoEXT *info,
                                                 const VkAllocationCallbacks *allocator,
                                                 VkDebugReportCallbackEXT *callback) {
    (void)instance, (void)info, (void)allocator, (void)callback;
    fprintf(stderr, "fake: vkCreateDebugReportCallbackEXT\n");
    return VK_ERROR_FEATURE_NOT_PRESENT;
}

EXPORTED VkResult vkQueueSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo *submits, VkFence fence) {
    (void)queue, (void)count, (void)submits, (void)fence;
    fprintf(stderr, "fake: vkQueueSubmit\n");
    return VK_SUCCESS;
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
