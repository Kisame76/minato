/* libvulkan's device: its layers and extensions, and vkCreateDevice with the queues' timeline semaphores (ADR-0030,
   decisions 5, 10, 11 and 13). */
#include <stdlib.h>
#include <string.h>
#include <sys/system_properties.h>

#include "libvulkan.h"

MINATO_HOST(vkEnumerateDeviceExtensionProperties);
MINATO_HOST(vkCreateDevice);
MINATO_HOST(vkDestroyDevice);
MINATO_HOST(vkGetPhysicalDeviceProperties);
MINATO_HOST(vkGetPhysicalDeviceProperties2);

/* Taken out of vkCreateDevice's chain: the memory report's callback, whose extension is not offered
   (decision 5). Copied, to enable timeline semaphores there: the app's features of Vulkan 1.2 or of
   timeline semaphores (decisions 11 and 13). */
static const VkStructureType taken_out[] = {VK_STRUCTURE_TYPE_DEVICE_DEVICE_MEMORY_REPORT_CREATE_INFO_EXT};
static const VkStructureType features[] = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                           VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};

/* The own extensions MoltenVK does not have, which libvulkan adds to the list and takes out of vkCreateDevice's. */
#define LIBVULKAN_ONLY                                                                              \
    (MINATO_OWNER(VK_KHR_incremental_present) |                                                      \
     MINATO_OWNER(VK_ANDROID_external_memory_android_hardware_buffer) | MINATO_OWNER(VK_EXT_queue_family_foreign))

/* What VK_ANDROID_external_memory_android_hardware_buffer requires of MoltenVK (ADR-0058 decision 7), its own
   VK_EXT_queue_family_foreign aside, and the Metal objects graphics/host imports its IOSurfaces with. */
static const char *const hardware_buffer_needs[] = {"VK_KHR_sampler_ycbcr_conversion", "VK_KHR_external_memory",
                                                    "VK_KHR_dedicated_allocation", "VK_EXT_metal_objects"};

VkResult vkEnumerateDeviceLayerProperties(VkPhysicalDevice physical, uint32_t *count, VkLayerProperties *properties) {
    (void)physical, (void)properties;
    *count = 0;
    return VK_SUCCESS;
}

VkResult vkEnumerateDeviceExtensionProperties(VkPhysicalDevice physical, const char *layer, uint32_t *count,
                                              VkExtensionProperties *properties) {
    if (layer != NULL) {
        return VK_ERROR_LAYER_NOT_PRESENT;
    }
    uint32_t theirs = 0;
    VkResult result = minato_host_vkEnumerateDeviceExtensionProperties(physical, NULL, &theirs, NULL);
    if (result != VK_SUCCESS) {
        return result;
    }
    VkExtensionProperties *moltenvk = malloc((theirs + 1) * sizeof *moltenvk);
    VkExtensionProperties *list = malloc((theirs + 3) * sizeof *list);
    if (moltenvk == NULL || list == NULL ||
        (result = minato_host_vkEnumerateDeviceExtensionProperties(physical, NULL, &theirs, moltenvk)) < 0) {
        free(moltenvk);
        free(list);
        return result < 0 ? result : VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    /* VK_KHR_incremental_present, whose regions libvulkan ignores, then MoltenVK's offered device
       extensions in its order (decision 11), then AHardwareBuffer's two where MoltenVK has what they need
       (ADR-0058 decision 7). */
    list[0] = (VkExtensionProperties){VK_KHR_INCREMENTAL_PRESENT_EXTENSION_NAME, VK_KHR_INCREMENTAL_PRESENT_SPEC_VERSION};
    uint32_t length = 1, needs = 0;
    for (uint32_t index = 0; index < theirs; index++) {
        const struct minato_extension *extension = minato_extension(moltenvk[index].extensionName);
        for (size_t need = 0; need < sizeof hardware_buffer_needs / sizeof *hardware_buffer_needs; need++) {
            needs += strcmp(moltenvk[index].extensionName, hardware_buffer_needs[need]) == 0;
        }
        if (extension != NULL && extension->device && (extension->owner & LIBVULKAN_ONLY) == 0) {
            list[length++] = moltenvk[index];
        }
    }
    if (needs == sizeof hardware_buffer_needs / sizeof *hardware_buffer_needs) {
        list[length++] = (VkExtensionProperties){VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME,
                                                 VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_SPEC_VERSION};
        list[length++] = (VkExtensionProperties){VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
                                                 VK_EXT_QUEUE_FAMILY_FOREIGN_SPEC_VERSION};
    }
    result = minato_hand_out(list, length, count, properties);
    free(moltenvk);
    free(list);
    return result;
}

VkResult vkCreateDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo *info,
                        const VkAllocationCallbacks *allocator, VkDevice *device) {
    (void)allocator;  /* never passed on (decision 5) */
    /* The app's extensions that libvulkan offers, without the ones it implements alone; then what it
       enables itself (decision 11). Device layers are ignored, as the specification says. */
    const char **names = malloc((info->enabledExtensionCount + 2) * sizeof *names);
    if (names == NULL) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    uint32_t count = 0, owned = 0;
    for (uint32_t index = 0; index < info->enabledExtensionCount; index++) {
        const struct minato_extension *extension = minato_extension(info->ppEnabledExtensionNames[index]);
        if (extension == NULL || !extension->device) {
            free(names);
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        }
        owned |= extension->owner;
        if ((extension->owner & LIBVULKAN_ONLY) == 0) {
            names[count++] = info->ppEnabledExtensionNames[index];
        }
    }
    /* The specification asks the app for the portability subset, and Android apps do not ask; the
       queues' MTLCommandQueue for "scheduled" (decision 13). */
    names[count++] = "VK_KHR_portability_subset";
    names[count++] = "VK_EXT_metal_objects";

    const void *chain;
    void *copy;
    VkResult result = minato_chain_edit(info->pNext, taken_out, sizeof taken_out / sizeof *taken_out, features,
                                        sizeof features / sizeof *features, &chain, &copy);
    if (result != VK_SUCCESS) {
        free(names);
        return result;
    }
    /* Timeline semaphores, for the queues' own (decision 13): in the app's features, else in libvulkan's. */
    VkPhysicalDeviceVulkan12Features *vulkan12 =
        minato_chain_find(chain, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES);
    VkPhysicalDeviceTimelineSemaphoreFeatures *timeline =
        minato_chain_find(chain, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES);
    VkPhysicalDeviceTimelineSemaphoreFeatures own = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES, .pNext = (void *)chain,
        .timelineSemaphore = VK_TRUE};
    if (vulkan12 != NULL) {
        vulkan12->timelineSemaphore = VK_TRUE;
    } else if (timeline != NULL) {
        timeline->timelineSemaphore = VK_TRUE;
    } else {
        chain = &own;
    }
    VkDeviceCreateInfo ours = *info;
    ours.pNext = chain;
    ours.enabledLayerCount = 0;
    ours.ppEnabledLayerNames = NULL;
    ours.enabledExtensionCount = count;
    ours.ppEnabledExtensionNames = names;
    result = minato_host_vkCreateDevice(physical, &ours, NULL, device);
    free(copy);
    free(names);
    if (result != VK_SUCCESS) {
        return result;
    }
    struct minato_queues *queues = NULL;
    result = minato_queues_create(*device, physical, info, &queues);
    if (result == VK_SUCCESS && minato_remember(*device, owned, queues) != 0) {
        result = VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    if (result != VK_SUCCESS) {
        minato_queues_destroy(queues);
        minato_host_vkDestroyDevice(*device, NULL);
    }
    return result;
}

void vkDestroyDevice(VkDevice device, const VkAllocationCallbacks *allocator) {
    (void)allocator;
    uint32_t owned;
    struct minato_queues *queues = NULL;
    if (device != VK_NULL_HANDLE) {
        if (minato_recall(device, &owned, &queues)) {
            minato_forget(device);
            minato_queues_destroy(queues);
        }
        minato_host_vkDestroyDevice(device, NULL);
    }
}

/* The device's name and vendor under a device profile (ADR-0065 step 4): the properties
   ro.minato.gpu.vk_device_name and ro.minato.gpu.vk_vendor_id (decimal), which the runtime writes only for an app with
   a profile, in place of MoltenVK's; nothing else of the properties changes, not the device ID and type, the limits
   or the driver's name and ID in the chain. */
static void apply_profile(VkPhysicalDeviceProperties *properties) {
    char name[PROP_VALUE_MAX] = "", vendor[PROP_VALUE_MAX] = "";
    __system_property_get("ro.minato.gpu.vk_device_name", name);
    __system_property_get("ro.minato.gpu.vk_vendor_id", vendor);
    if (name[0] != '\0') {
        strncpy(properties->deviceName, name, VK_MAX_PHYSICAL_DEVICE_NAME_SIZE - 1);
        properties->deviceName[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE - 1] = '\0';
    }
    if (vendor[0] != '\0') {
        properties->vendorID = (uint32_t)strtoul(vendor, NULL, 10);
    }
}

void vkGetPhysicalDeviceProperties(VkPhysicalDevice physical, VkPhysicalDeviceProperties *properties) {
    minato_host_vkGetPhysicalDeviceProperties(physical, properties);
    apply_profile(properties);
}

void vkGetPhysicalDeviceProperties2(VkPhysicalDevice physical, VkPhysicalDeviceProperties2 *properties) {
    minato_host_vkGetPhysicalDeviceProperties2(physical, properties);
    apply_profile(&properties->properties);
}
