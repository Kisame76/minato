/* ADR-0065 step 4, against graphics/tests/fake_vulkan.c in MoltenVK's place (graphics.vulkan_profile and
   graphics.vulkan_profile_pixel): what vkGetPhysicalDeviceProperties and vkGetPhysicalDeviceProperties2 answer for
   the device's name and vendor. Without a device profile they are the driver's own; under one (the properties
   ro.minato.gpu.vk_device_name and ro.minato.gpu.vk_vendor_id, which the runtime writes) libvulkan replaces those two
   and nothing else: the device ID and type, the limits, the API version and the driver's own name and ID in the
   chain stay the driver's. */
#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>

#include <stdio.h>
#include <stdlib.h>

static void show(const char *what, const VkPhysicalDeviceProperties *properties) {
    printf("%s: \"%s\", vendor 0x%x, device 0x%x, type %d, maxImageDimension2D %u, api %u.%u\n", what,
           properties->deviceName, properties->vendorID, properties->deviceID, (int)properties->deviceType,
           properties->limits.maxImageDimension2D, VK_API_VERSION_MAJOR(properties->apiVersion),
           VK_API_VERSION_MINOR(properties->apiVersion));
}

int main(void) {
    const VkInstanceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    VkInstance instance;
    if (vkCreateInstance(&info, NULL, &instance) != VK_SUCCESS) {
        printf("vkCreateInstance failed\n");
        return 1;
    }
    VkPhysicalDevice physical;
    uint32_t count = 1;
    vkEnumeratePhysicalDevices(instance, &count, &physical);
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(physical, &properties);
    show("vkGetPhysicalDeviceProperties", &properties);
    VkPhysicalDeviceDriverProperties driver = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 properties2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                                               .pNext = &driver};
    vkGetPhysicalDeviceProperties2(physical, &properties2);
    show("vkGetPhysicalDeviceProperties2", &properties2.properties);
    printf("driver in the chain: \"%s\", id %d\n", driver.driverName, (int)driver.driverID);
    vkDestroyInstance(instance, NULL);
    return 0;
}
