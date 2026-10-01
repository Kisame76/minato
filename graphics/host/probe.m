/* ADR-0030 V3's check for the iPhone; see minato_graphics.h. Host code only: it opens MoltenVK as
   graphics/host's "vulkan" provider does (providers.c), and no guest code runs. Also the launch hook of
   ADR-0044 a)'s probe, which debug builds of the app get with this file. */
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_METAL_EXT
#include <vulkan/vulkan.h>

#import <Foundation/Foundation.h>
#import <QuartzCore/CAMetalLayer.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "minato_graphics.h"

/* The layer's size in points and its scale: the surface then reports 192 x 192 pixels. */
enum { kPoints = 64, kScale = 3 };

static int failed(const char *what, VkResult result) {
    fprintf(stderr, "graphics probe: %s failed: %d\n", what, result);
    return 1;
}

/* A function from MoltenVK, or NULL after saying which one is missing. */
static PFN_vkVoidFunction load(PFN_vkGetInstanceProcAddr get, VkInstance instance, const char *name,
                               int *missing) {
    PFN_vkVoidFunction function = get(instance, name);
    if (function == NULL) {
        fprintf(stderr, "graphics probe: MoltenVK has no %s\n", name);
        *missing = 1;
    }
    return function;
}
#define LOAD(name) ((PFN_##name)load(get, instance, #name, &missing))

/* The GPU, then a surface on a layer of our own, which it destroys again. */
static int check_surface(PFN_vkGetInstanceProcAddr get, VkInstance instance) {
    int missing = 0;
    PFN_vkEnumeratePhysicalDevices enumerate_devices = LOAD(vkEnumeratePhysicalDevices);
    PFN_vkGetPhysicalDeviceProperties2 get_properties = LOAD(vkGetPhysicalDeviceProperties2);
    PFN_vkCreateMetalSurfaceEXT create_surface = LOAD(vkCreateMetalSurfaceEXT);
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR get_support = LOAD(vkGetPhysicalDeviceSurfaceSupportKHR);
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR get_capabilities =
        LOAD(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
    PFN_vkDestroySurfaceKHR destroy_surface = LOAD(vkDestroySurfaceKHR);
    if (missing) {
        return 1;
    }

    uint32_t count = 1;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkResult result = enumerate_devices(instance, &count, &physical);
    if ((result != VK_SUCCESS && result != VK_INCOMPLETE) || count == 0) {
        return failed("vkEnumeratePhysicalDevices", result);
    }
    VkPhysicalDeviceDriverProperties driver = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 properties = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                                              .pNext = &driver};
    get_properties(physical, &properties);
    const uint32_t version = properties.properties.apiVersion;
    printf("graphics probe: GPU %s, driver %s %s, device at Vulkan %u.%u.%u\n", properties.properties.deviceName,
           driver.driverName, driver.driverInfo, VK_API_VERSION_MAJOR(version), VK_API_VERSION_MINOR(version),
           VK_API_VERSION_PATCH(version));

    int status = 0;
    @autoreleasepool {
        CAMetalLayer *layer = [[CAMetalLayer alloc] init];
        layer.bounds = CGRectMake(0, 0, kPoints, kPoints);
        layer.contentsScale = kScale;
        VkMetalSurfaceCreateInfoEXT surface_info = {.sType = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT,
                                                    .pLayer = layer};
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        result = create_surface(instance, &surface_info, NULL, &surface);
        if (result != VK_SUCCESS) {
            status = failed("vkCreateMetalSurfaceEXT", result);
        } else {
            VkBool32 presents = VK_FALSE;
            VkSurfaceCapabilitiesKHR capabilities = {0};
            result = get_support(physical, 0, surface, &presents);
            if (result != VK_SUCCESS) {
                status = failed("vkGetPhysicalDeviceSurfaceSupportKHR", result);
            } else if ((result = get_capabilities(physical, surface, &capabilities)) != VK_SUCCESS) {
                status = failed("vkGetPhysicalDeviceSurfaceCapabilitiesKHR", result);
            } else {
                printf("graphics probe: a Metal surface on a CAMetalLayer of %d x %d points at scale %d, "
                       "%u x %u pixels, %s from queue family 0\n",
                       kPoints, kPoints, kScale, capabilities.currentExtent.width,
                       capabilities.currentExtent.height, presents ? "presentable" : "not presentable");
            }
            destroy_surface(instance, surface, NULL);
        }
        [layer release];
    }
    return status;
}

int minato_graphics_probe(const char *moltenvk) {
    /* The flags of graphics/host's providers. */
    void *library = dlopen(moltenvk, RTLD_NOW | RTLD_LOCAL | RTLD_FIRST);
    if (library == NULL) {
        fprintf(stderr, "graphics probe: %s\n", dlerror());
        return 1;
    }
    PFN_vkGetInstanceProcAddr get = (PFN_vkGetInstanceProcAddr)dlsym(library, "vkGetInstanceProcAddr");
    if (get == NULL) {
        fprintf(stderr, "graphics probe: %s exports no vkGetInstanceProcAddr\n", moltenvk);
        return 1;
    }
    printf("graphics probe: MoltenVK opened from %s\n", moltenvk);

    int missing = 0;
    VkInstance instance = VK_NULL_HANDLE;
    PFN_vkEnumerateInstanceVersion enumerate_version = LOAD(vkEnumerateInstanceVersion);
    PFN_vkCreateInstance create_instance = LOAD(vkCreateInstance);
    if (missing) {
        return 1;
    }
    uint32_t offered = 0;
    VkResult result = enumerate_version(&offered);
    if (result != VK_SUCCESS) {
        return failed("vkEnumerateInstanceVersion", result);
    }
    const char *const extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_EXT_METAL_SURFACE_EXTENSION_NAME};
    const VkApplicationInfo application = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                           .pApplicationName = "minato graphics probe",
                                           .apiVersion = VK_API_VERSION_1_3};
    const VkInstanceCreateInfo instance_info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                                .pApplicationInfo = &application,
                                                .enabledExtensionCount = 2,
                                                .ppEnabledExtensionNames = extensions};
    result = create_instance(&instance_info, NULL, &instance);
    if (result != VK_SUCCESS) {
        return failed("vkCreateInstance", result);
    }
    printf("graphics probe: an instance at Vulkan 1.3 with %s and %s, MoltenVK offers %u.%u.%u\n", extensions[0],
           extensions[1], VK_API_VERSION_MAJOR(offered), VK_API_VERSION_MINOR(offered), VK_API_VERSION_PATCH(offered));

    PFN_vkDestroyInstance destroy_instance = LOAD(vkDestroyInstance);
    if (missing) {
        return 1;
    }
    const int status = check_surface(get, instance);
    destroy_instance(instance, NULL);
    if (status == 0) {
        printf("graphics probe: surface and instance destroyed\n");
    }
    return status;
}

/* A debug build of the app, launched with MINATO_GRAPHICS_PROBE set, runs the probe on the MoltenVK in
   its Frameworks before main and ends with the probe's result. Without the variable it does nothing.
   _exit, after flushing, runs no other image's teardown in the middle of the launch. */
__attribute__((constructor)) static void probe_on_launch(void) {
    if (getenv("MINATO_GRAPHICS_PROBE") == NULL) {
        return;
    }
    int status;
    @autoreleasepool {
        NSString *path = [NSBundle.mainBundle.privateFrameworksPath
            stringByAppendingPathComponent:@"MoltenVK.framework/MoltenVK"];
        status = minato_graphics_probe(path.fileSystemRepresentation);
    }
    fflush(stdout);
    fflush(stderr);
    _exit(status);
}

/* ADR-0044 a): a debug build of the app, launched with MINATO_PROVOKING_PROBE set, runs the probe of the last vertex
   (provoking.m) once the app is active, since iOS refuses GPU work from an app that is not in the foreground, and
   ends with the probe's result. Without the variable it does nothing. */
__attribute__((constructor)) static void provoking_probe_on_launch(void) {
    if (getenv("MINATO_PROVOKING_PROBE") == NULL) {
        return;
    }
    @autoreleasepool {
        // UIKit's name for the notification, from UIKit itself: graphics/host does not link it.
        NSString *const *active = dlsym(RTLD_DEFAULT, "UIApplicationDidBecomeActiveNotification");
        if (active == NULL) {
            fprintf(stderr, "provoking probe: no UIKit app to wait for\n");
            fflush(stderr);
            _exit(1);
        }
        [NSNotificationCenter.defaultCenter addObserverForName:*active
                                                        object:nil
                                                         queue:nil
                                                    usingBlock:^(NSNotification *notification) {
                                                        (void)notification;
                                                        const int status = minato_provoking_probe();
                                                        fflush(stdout);
                                                        fflush(stderr);
                                                        _exit(status);
                                                    }];
    }
}
