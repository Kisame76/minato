/* libvulkan's device: its layers and extensions, vkCreateDevice with the queues' timeline semaphores, and
   AHardwareBuffer's two functions (ADR-0030, decisions 5, 10, 11 and 13). */
#include <stdlib.h>
#include <string.h>

#include "libvulkan.h"

MINATO_HOST(vkEnumerateDeviceExtensionProperties);
MINATO_HOST(vkCreateDevice);
MINATO_HOST(vkDestroyDevice);

/* Taken out of vkCreateDevice's chain: the memory report's callback, whose extension is not offered
   (decision 5). Copied, to enable timeline semaphores there: the app's features of Vulkan 1.2 or of
   timeline semaphores (decisions 11 and 13). */
static const VkStructureType taken_out[] = {VK_STRUCTURE_TYPE_DEVICE_DEVICE_MEMORY_REPORT_CREATE_INFO_EXT};
static const VkStructureType features[] = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                           VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};

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
    VkExtensionProperties *list = malloc((theirs + 1) * sizeof *list);
    if (moltenvk == NULL || list == NULL ||
        (result = minato_host_vkEnumerateDeviceExtensionProperties(physical, NULL, &theirs, moltenvk)) < 0) {
        free(moltenvk);
        free(list);
        return result < 0 ? result : VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    /* VK_KHR_incremental_present, whose regions libvulkan ignores, then MoltenVK's offered device
       extensions in its order (decision 11). */
    list[0] = (VkExtensionProperties){VK_KHR_INCREMENTAL_PRESENT_EXTENSION_NAME, VK_KHR_INCREMENTAL_PRESENT_SPEC_VERSION};
    uint32_t length = 1;
    for (uint32_t index = 0; index < theirs; index++) {
        const struct minato_extension *extension = minato_extension(moltenvk[index].extensionName);
        if (extension != NULL && extension->device && extension->owner != MINATO_OWNER(VK_KHR_incremental_present)) {
            list[length++] = moltenvk[index];
        }
    }
    result = minato_hand_out(list, length, count, properties);
    free(moltenvk);
    free(list);
    return result;
}

VkResult vkCreateDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo *info,
                        const VkAllocationCallbacks *allocator, VkDevice *device) {
    (void)allocator;  /* never passed on (decision 5) */
    /* The app's extensions that libvulkan offers, without the one it implements alone; then what it
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
        if (extension->owner != MINATO_OWNER(VK_KHR_incremental_present)) {
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

/* AHardwareBuffer's functions, while VK_ANDROID_external_memory_android_hardware_buffer is not offered
   (decision 10). */
VkResult vkGetAndroidHardwareBufferPropertiesANDROID(VkDevice device, const struct AHardwareBuffer *buffer,
                                                     VkAndroidHardwareBufferPropertiesANDROID *properties) {
    (void)device, (void)buffer, (void)properties;
    return VK_ERROR_INVALID_EXTERNAL_HANDLE;
}

VkResult vkGetMemoryAndroidHardwareBufferANDROID(VkDevice device, const VkMemoryGetAndroidHardwareBufferInfoANDROID *info,
                                                 struct AHardwareBuffer **buffer) {
    (void)device, (void)info, (void)buffer;
    return VK_ERROR_INVALID_EXTERNAL_HANDLE;
}
