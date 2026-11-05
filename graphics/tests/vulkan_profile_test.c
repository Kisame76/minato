/* ADR-0065 step 4, against graphics/tests/fake_vulkan.c in MoltenVK's place (graphics.vulkan_profile and
   graphics.vulkan_profile_pixel): what vkGetPhysicalDeviceProperties and vkGetPhysicalDeviceProperties2 answer for
   the device's identity. Without a device profile it is the driver's own; under one (the properties
   ro.minato.gpu.vk_*, which the runtime writes; ADR-0065's amendment) libvulkan answers the profile's real device's
   name, vendor, device ID, driver version, and in the chain (VkPhysicalDeviceDriverProperties, and
   VkPhysicalDeviceVulkan12Properties asked apart, as the two may not share a chain) its driver's ID, name and info;
   the device type, the limits and the API version stay the driver's. */
#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>

#include <stdio.h>
#include <stdlib.h>

static void show(const char *what, const VkPhysicalDeviceProperties *properties) {
    printf("%s: \"%s\", vendor 0x%x, device 0x%x, driver 0x%x, type %d, maxImageDimension2D %u, api %u.%u\n", what,
           properties->deviceName, properties->vendorID, properties->deviceID, properties->driverVersion,
           (int)properties->deviceType, properties->limits.maxImageDimension2D,
           VK_API_VERSION_MAJOR(properties->apiVersion), VK_API_VERSION_MINOR(properties->apiVersion));
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
    printf("driver in the chain: \"%s\", id %d, info \"%s\"\n", driver.driverName, (int)driver.driverID,
           driver.driverInfo);
    VkPhysicalDeviceVulkan12Properties vulkan12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
    VkPhysicalDeviceProperties2 properties12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                                                .pNext = &vulkan12};
    vkGetPhysicalDeviceProperties2(physical, &properties12);
    printf("Vulkan 1.2's driver in the chain: \"%s\", id %d, info \"%s\"\n", vulkan12.driverName,
           (int)vulkan12.driverID, vulkan12.driverInfo);
    vkDestroyInstance(instance, NULL);
    return 0;
}
