/* libvulkan's instance: its version, layers and extensions, vkCreateInstance with the pinned settings, and the
   debug callbacks' functions (ADR-0030, decisions 5, 8 and 11). */
#include <stdlib.h>
#include <string.h>

#include "libvulkan.h"
#include "moltenvk_settings.h"

MINATO_HOST(vkEnumerateInstanceExtensionProperties);
MINATO_HOST(vkCreateInstance);
MINATO_HOST(vkDestroyInstance);
MINATO_HOST(vkCreateDebugUtilsMessengerEXT);
MINATO_HOST(vkDestroyDebugUtilsMessengerEXT);
MINATO_HOST(vkCreateDebugReportCallbackEXT);
MINATO_HOST(vkDestroyDebugReportCallbackEXT);

/* Android's own instance extensions, in its loader's order (decision 11). */
static const VkExtensionProperties own[] = {
    {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_SURFACE_SPEC_VERSION},
    {VK_KHR_SURFACE_PROTECTED_CAPABILITIES_EXTENSION_NAME, VK_KHR_SURFACE_PROTECTED_CAPABILITIES_SPEC_VERSION},
    {VK_KHR_ANDROID_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_SPEC_VERSION},
    {VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME, VK_EXT_SWAPCHAIN_COLOR_SPACE_SPEC_VERSION},
    {VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME, VK_KHR_GET_SURFACE_CAPABILITIES_2_SPEC_VERSION},
    {VK_GOOGLE_SURFACELESS_QUERY_EXTENSION_NAME, VK_GOOGLE_SURFACELESS_QUERY_SPEC_VERSION},
};
#define OWN (sizeof own / sizeof *own)

/* Taken out of vkCreateInstance's chain: the app's own settings (decision 8) and the direct driver loading,
   whose extension is not offered. The app's debug callbacks stay, as libvulkan's copies, which
   graphics/host's vkCreateInstance points at its own functions: the app's never reach MoltenVK (decision 5). */
static const VkStructureType taken_out[] = {
    VK_STRUCTURE_TYPE_LAYER_SETTINGS_CREATE_INFO_EXT,
    VK_STRUCTURE_TYPE_DIRECT_DRIVER_LOADING_LIST_LUNARG,
};
static const VkStructureType callbacks[] = {
    VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT,
    VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
};

VkResult vkEnumerateInstanceVersion(uint32_t *version) {
    *version = VK_API_VERSION_1_3;  /* as Android 15 (decision 11) */
    return VK_SUCCESS;
}

VkResult vkEnumerateInstanceLayerProperties(uint32_t *count, VkLayerProperties *properties) {
    (void)properties;
    *count = 0;  /* none, as for a release app on Android (decision 10) */
    return VK_SUCCESS;
}

VkResult vkEnumerateInstanceExtensionProperties(const char *layer, uint32_t *count, VkExtensionProperties *properties) {
    if (layer != NULL) {
        return VK_ERROR_LAYER_NOT_PRESENT;
    }
    uint32_t theirs = 0;
    VkResult result = minato_host_vkEnumerateInstanceExtensionProperties(NULL, &theirs, NULL);
    if (result != VK_SUCCESS) {
        return result;
    }
    VkExtensionProperties *moltenvk = malloc((theirs + 1) * sizeof *moltenvk);
    VkExtensionProperties *list = malloc((OWN + theirs) * sizeof *list);
    if (moltenvk == NULL || list == NULL ||
        (result = minato_host_vkEnumerateInstanceExtensionProperties(NULL, &theirs, moltenvk)) < 0) {
        free(moltenvk);
        free(list);
        return result < 0 ? result : VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    /* Android's own, then VK_EXT_debug_report, then MoltenVK's other offered instance extensions in its
       order (decision 11). */
    memcpy(list, own, sizeof own);
    uint32_t length = OWN;
    for (uint32_t index = 0; index < theirs; index++) {
        if (strcmp(moltenvk[index].extensionName, VK_EXT_DEBUG_REPORT_EXTENSION_NAME) == 0) {
            list[length++] = moltenvk[index];
        }
    }
    for (uint32_t index = 0; index < theirs; index++) {
        const struct minato_extension *extension = minato_extension(moltenvk[index].extensionName);
        if (extension != NULL && !extension->device && extension->owner == 0 &&
            strcmp(moltenvk[index].extensionName, VK_EXT_DEBUG_REPORT_EXTENSION_NAME) != 0) {
            list[length++] = moltenvk[index];
        }
    }
    result = minato_hand_out(list, length, count, properties);
    free(moltenvk);
    free(list);
    return result;
}

VkResult vkCreateInstance(const VkInstanceCreateInfo *info, const VkAllocationCallbacks *allocator,
                          VkInstance *instance) {
    (void)allocator;  /* never passed on (decision 5) */
    if (info->enabledLayerCount > 0) {
        return VK_ERROR_LAYER_NOT_PRESENT;
    }
    /* The app's extensions: those libvulkan offers, its own ones kept for itself; then what libvulkan
       needs from MoltenVK. */
    const char **names = malloc((info->enabledExtensionCount + 3) * sizeof *names);
    if (names == NULL) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    uint32_t count = 0, owned = 0;
    for (uint32_t index = 0; index < info->enabledExtensionCount; index++) {
        const struct minato_extension *extension = minato_extension(info->ppEnabledExtensionNames[index]);
        if (extension == NULL || extension->device) {
            free(names);
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        }
        owned |= extension->owner;
        if (extension->owner == 0) {
            names[count++] = info->ppEnabledExtensionNames[index];
        }
    }
    /* MoltenVK's surfaces on a CAMetalLayer stand behind Android's (decision 12). */
    if (owned & (MINATO_OWNER(VK_KHR_surface) | MINATO_OWNER(VK_KHR_android_surface))) {
        names[count++] = VK_KHR_SURFACE_EXTENSION_NAME;
    }
    if (owned & MINATO_OWNER(VK_KHR_android_surface)) {
        names[count++] = "VK_EXT_metal_surface";
    }
    names[count++] = VK_EXT_LAYER_SETTINGS_EXTENSION_NAME;

    const void *chain;
    void *copy;
    VkResult result = minato_chain_edit(info->pNext, taken_out, sizeof taken_out / sizeof *taken_out, callbacks,
                                        sizeof callbacks / sizeof *callbacks, &chain, &copy);
    if (result != VK_SUCCESS) {
        free(names);
        return result;
    }
    const VkLayerSettingsCreateInfoEXT settings = {.sType = VK_STRUCTURE_TYPE_LAYER_SETTINGS_CREATE_INFO_EXT,
                                                   .pNext = chain,
                                                   .settingCount = sizeof minato_moltenvk_settings /
                                                                   sizeof *minato_moltenvk_settings,
                                                   .pSettings = minato_moltenvk_settings};
    /* Vulkan 1.3 whatever the app asked for, so its devices report 1.3 (decision 11). */
    VkApplicationInfo application = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO};
    if (info->pApplicationInfo != NULL) {
        application = *info->pApplicationInfo;
    }
    application.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ours = *info;
    ours.pNext = &settings;
    ours.pApplicationInfo = &application;
    ours.enabledExtensionCount = count;
    ours.ppEnabledExtensionNames = names;
    result = minato_host_vkCreateInstance(&ours, NULL, instance);
    free(copy);
    free(names);
    if (result == VK_SUCCESS && minato_remember(*instance, owned, NULL) != 0) {
        minato_host_vkDestroyInstance(*instance, NULL);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    return result;
}

void vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks *allocator) {
    (void)allocator;
    if (instance != VK_NULL_HANDLE) {
        minato_forget(instance);
        minato_host_vkDestroyInstance(instance, NULL);
    }
}

/* The debug callbacks' functions reach graphics/host's, which registers a function of its own with MoltenVK
   that calls the app's on the app's threads only (decision 5). */
VkResult vkCreateDebugUtilsMessengerEXT(VkInstance instance, const VkDebugUtilsMessengerCreateInfoEXT *info,
                                        const VkAllocationCallbacks *allocator, VkDebugUtilsMessengerEXT *messenger) {
    (void)allocator;
    return minato_host_vkCreateDebugUtilsMessengerEXT(instance, info, NULL, messenger);
}

void vkDestroyDebugUtilsMessengerEXT(VkInstance instance, VkDebugUtilsMessengerEXT messenger,
                                     const VkAllocationCallbacks *allocator) {
    (void)allocator;
    minato_host_vkDestroyDebugUtilsMessengerEXT(instance, messenger, NULL);
}

VkResult vkCreateDebugReportCallbackEXT(VkInstance instance, const VkDebugReportCallbackCreateInfoEXT *info,
                                        const VkAllocationCallbacks *allocator, VkDebugReportCallbackEXT *callback) {
    (void)allocator;
    return minato_host_vkCreateDebugReportCallbackEXT(instance, info, NULL, callback);
}

void vkDestroyDebugReportCallbackEXT(VkInstance instance, VkDebugReportCallbackEXT callback,
                                     const VkAllocationCallbacks *allocator) {
    (void)allocator;
    minato_host_vkDestroyDebugReportCallbackEXT(instance, callback, NULL);
}
