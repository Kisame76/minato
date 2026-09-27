/* Test 6 of ADR-0030, the window's part against the fake MoltenVK (graphics.vulkan_window), on graphics-run's
   window of 64 x 48 pixels (window_host.m, ID 1), with the fake playing the app's host side:
   - the surface on the window's layer and what it answers: support, capabilities from the window's size or
     its buffer geometry, formats, present modes and the rest of VK_KHR_surface, VK_KHR_swapchain's device
     group queries, VK_KHR_get_surface_capabilities2, VK_KHR_surface_protected_capabilities and
     VK_GOOGLE_surfaceless_query (decision 12);
   - swapchains the direct way and the copy way: what reaches MoltenVK, the copy's commands, the order the
     app's images come back in, a replaced swapchain, and VK_SUBOPTIMAL_KHR for the size (decision 12);
   - the gates (decision 13): a present waits at the closed gate and takes a signal meanwhile; in the
     background every call that commits waits, also through the KHR aliases, while "scheduled" comes
     through the runtime; a present waiting there ends when the window goes; then VK_ERROR_SURFACE_LOST_KHR
     at once.
   The fake's script steps come from marker fences given to vkResetFences (fake_vulkan.c). */
#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>

#include <android/native_window.h>
#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* The fake's script steps (fake_vulkan.c): the gate closes and opens 150 ms later; the background for
   150 ms, with "scheduled" asked; the background, the window gone after 100 ms, the foreground after 200; the
   background for 150 ms from the fake's next wait on. */
#define CLOSE_THEN_OPEN ((VkFence)(uintptr_t)0x6a7e0001)
#define BACKGROUND ((VkFence)(uintptr_t)0x6a7e0002)
#define BACKGROUND_THEN_GONE ((VkFence)(uintptr_t)0x6a7e0003)
#define BACKGROUND_IN_WAIT ((VkFence)(uintptr_t)0x6a7e0004)

#define MS 1000000u /* ns */

static VkInstance instance;
static VkPhysicalDevice physical;
static VkDevice device;
static VkQueue queue, other_queue;
static VkSurfaceKHR surface;
static pthread_t program;
static volatile uint64_t handled_at;

static uint64_t now(void) {
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return (uint64_t)time.tv_sec * 1000000000u + (uint64_t)time.tv_nsec;
}

static void handled(int signal) {
    (void)signal;
    handled_at = now();
}

static void check(const char *what, VkResult result) {
    if (result != VK_SUCCESS) {
        printf("%s: %d\n", what, result);
        exit(1);
    }
}

static const char *yes(int condition) {
    return condition ? "yes" : "no";
}

/* A window of the host window `id`, as activity-host makes it (libnativewindow's private constructor). */
static struct ANativeWindow *window_of(uint64_t id) {
    void *library = dlopen("libnativewindow.so", RTLD_NOW);
    struct ANativeWindow *(*create)(uint64_t, int32_t, int32_t) =
        library != NULL ? (struct ANativeWindow * (*)(uint64_t, int32_t, int32_t)) dlsym(library, "minato_ANativeWindow_create")
                        : NULL;
    if (create == NULL) {
        printf("no minato_ANativeWindow_create\n");
        exit(1);
    }
    return create(id, 64, 48);
}

static void marker(VkFence step) {
    check("vkResetFences", vkResetFences(device, 1, &step));
}

static void setup(void) {
    const char *extensions[] = {"VK_KHR_surface", "VK_KHR_android_surface", "VK_KHR_get_surface_capabilities2",
                                "VK_KHR_surface_protected_capabilities", "VK_GOOGLE_surfaceless_query"};
    const VkApplicationInfo application = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "window",
                                           .apiVersion = VK_API_VERSION_1_1};
    const VkInstanceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                       .pApplicationInfo = &application, .enabledExtensionCount = 5,
                                       .ppEnabledExtensionNames = extensions};
    check("vkCreateInstance", vkCreateInstance(&info, NULL, &instance));
    uint32_t count = 1;
    check("vkEnumeratePhysicalDevices", vkEnumeratePhysicalDevices(instance, &count, &physical));
    const float priority = 1;
    const VkDeviceQueueCreateInfo families[] = {
        {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0, .queueCount = 1,
         .pQueuePriorities = &priority},
        {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 1, .queueCount = 1,
         .pQueuePriorities = &priority}};
    const char *device_extensions[] = {"VK_KHR_swapchain", "VK_KHR_synchronization2"};
    const VkDeviceCreateInfo created = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 2,
                                        .pQueueCreateInfos = families, .enabledExtensionCount = 2,
                                        .ppEnabledExtensionNames = device_extensions};
    check("vkCreateDevice", vkCreateDevice(physical, &created, NULL, &device));
    vkGetDeviceQueue(device, 0, 0, &queue);
    vkGetDeviceQueue(device, 1, 0, &other_queue);
}

static void capabilities(const char *what) {
    VkSurfaceCapabilitiesKHR got;
    memset(&got, 0xff, sizeof got);
    check("vkGetPhysicalDeviceSurfaceCapabilitiesKHR", vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &got));
    printf("%s: %u to %u images, current %u x %u, extents %u x %u to %u x %u, %u layers, transforms 0x%x, "
           "current 0x%x, alpha 0x%x, usage 0x%x\n",
           what, got.minImageCount, got.maxImageCount, got.currentExtent.width, got.currentExtent.height,
           got.minImageExtent.width, got.minImageExtent.height, got.maxImageExtent.width, got.maxImageExtent.height,
           got.maxImageArrayLayers, got.supportedTransforms, got.currentTransform, got.supportedCompositeAlpha,
           got.supportedUsageFlags);
}

static void formats(const char *what, VkSurfaceKHR which) {
    uint32_t count = 0;
    VkSurfaceFormatKHR list[8];
    check("vkGetPhysicalDeviceSurfaceFormatsKHR", vkGetPhysicalDeviceSurfaceFormatsKHR(physical, which, &count, NULL));
    uint32_t room = 8;
    check("vkGetPhysicalDeviceSurfaceFormatsKHR", vkGetPhysicalDeviceSurfaceFormatsKHR(physical, which, &room, list));
    printf("%s: %u:", what, count);
    for (uint32_t index = 0; index < room; index++) {
        printf(" %d/%d", list[index].format, list[index].colorSpace);
    }
    printf("\n");
}

static void present_modes(const char *what, VkSurfaceKHR which) {
    uint32_t count = 4;
    VkPresentModeKHR modes[4];
    check("vkGetPhysicalDeviceSurfacePresentModesKHR",
          vkGetPhysicalDeviceSurfacePresentModesKHR(physical, which, &count, modes));
    printf("%s: %u:", what, count);
    for (uint32_t index = 0; index < count; index++) {
        printf(" %d", modes[index]);
    }
    printf("\n");
}

static void surface_part(void) {
    struct ANativeWindow *window = window_of(1);
    const VkAndroidSurfaceCreateInfoKHR info = {.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR,
                                                .window = window};
    check("vkCreateAndroidSurfaceKHR", vkCreateAndroidSurfaceKHR(instance, &info, NULL, &surface));
    printf("support:");
    for (uint32_t family = 0; family < 4; family++) {
        VkBool32 supported = 7;
        check("vkGetPhysicalDeviceSurfaceSupportKHR",
              vkGetPhysicalDeviceSurfaceSupportKHR(physical, family, surface, &supported));
        printf(" %u", supported);
    }
    printf("\n");
    capabilities("capabilities");
    ANativeWindow_setBuffersGeometry(window, 32, 24, 0);
    capabilities("with a buffer geometry of 32 x 24");
    ANativeWindow_setBuffersGeometry(window, 0, 0, 0);
    ANativeWindow_release(window);
    capabilities("after the app released the window");

    formats("formats", surface);
    uint32_t room = 2;
    VkSurfaceFormatKHR two[2];
    const VkResult incomplete = vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &room, two);
    printf("formats with room for 2: %d, %u: %d/%d %d/%d\n", incomplete, room, two[0].format, two[0].colorSpace,
           two[1].format, two[1].colorSpace);
    present_modes("present modes", surface);

    uint32_t rectangles = 0;
    VkRect2D rectangle;
    check("vkGetPhysicalDevicePresentRectanglesKHR",
          vkGetPhysicalDevicePresentRectanglesKHR(physical, surface, &rectangles, NULL));
    check("vkGetPhysicalDevicePresentRectanglesKHR",
          vkGetPhysicalDevicePresentRectanglesKHR(physical, surface, &rectangles, &rectangle));
    printf("rectangles: %u: %d,%d %u x %u\n", rectangles, rectangle.offset.x, rectangle.offset.y,
           rectangle.extent.width, rectangle.extent.height);

    VkDeviceGroupPresentCapabilitiesKHR group = {.sType = VK_STRUCTURE_TYPE_DEVICE_GROUP_PRESENT_CAPABILITIES_KHR};
    memset(group.presentMask, 0xff, sizeof group.presentMask);
    VkDeviceGroupPresentModeFlagsKHR modes = 0;
    check("vkGetDeviceGroupPresentCapabilitiesKHR", vkGetDeviceGroupPresentCapabilitiesKHR(device, &group));
    check("vkGetDeviceGroupSurfacePresentModesKHR", vkGetDeviceGroupSurfacePresentModesKHR(device, surface, &modes));
    printf("device group: present mask 0x%x 0x%x, modes 0x%x; the surface's modes 0x%x\n", group.presentMask[0],
           group.presentMask[1], group.modes, modes);

    PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR capabilities2 = (PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR)
        vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceSurfaceCapabilities2KHR");
    PFN_vkGetPhysicalDeviceSurfaceFormats2KHR formats2 =
        (PFN_vkGetPhysicalDeviceSurfaceFormats2KHR)vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceSurfaceFormats2KHR");
    const VkPhysicalDeviceSurfaceInfo2KHR which = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR,
                                                   .surface = surface};
    VkSurfaceProtectedCapabilitiesKHR protected = {.sType = VK_STRUCTURE_TYPE_SURFACE_PROTECTED_CAPABILITIES_KHR,
                                                   .supportsProtected = 7};
    VkSurfaceCapabilities2KHR got = {.sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR, .pNext = &protected};
    check("vkGetPhysicalDeviceSurfaceCapabilities2KHR", capabilities2(physical, &which, &got));
    printf("capabilities 2: current %u x %u, %u to %u images, protected %u\n", got.surfaceCapabilities.currentExtent.width,
           got.surfaceCapabilities.currentExtent.height, got.surfaceCapabilities.minImageCount,
           got.surfaceCapabilities.maxImageCount, protected.supportsProtected);
    uint32_t count = 8;
    VkSurfaceFormat2KHR list[8];
    for (uint32_t index = 0; index < 8; index++) {
        list[index] = (VkSurfaceFormat2KHR){.sType = VK_STRUCTURE_TYPE_SURFACE_FORMAT_2_KHR};
    }
    check("vkGetPhysicalDeviceSurfaceFormats2KHR", formats2(physical, &which, &count, list));
    printf("formats 2: %u:", count);
    for (uint32_t index = 0; index < count; index++) {
        printf(" %d/%d", list[index].surfaceFormat.format, list[index].surfaceFormat.colorSpace);
    }
    printf("\n");
    count = 2;
    list[0].surfaceFormat.format = list[1].surfaceFormat.format = VK_FORMAT_UNDEFINED;
    const VkResult incomplete2 = formats2(physical, &which, &count, list);
    printf("formats 2 with room for 2: %d, %u: %d %d\n", incomplete2, count, list[0].surfaceFormat.format,
           list[1].surfaceFormat.format);

    formats("formats without a surface", VK_NULL_HANDLE);
    present_modes("present modes without a surface", VK_NULL_HANDLE);
}

static VkSemaphore semaphore(void) {
    const VkSemaphoreCreateInfo info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore made;
    check("vkCreateSemaphore", vkCreateSemaphore(device, &info, NULL, &made));
    return made;
}

static VkSwapchainKHR swapchain(VkFormat format, uint32_t images, uint32_t width, uint32_t height,
                                VkImageUsageFlags usage, VkSwapchainKHR old) {
    const VkSwapchainCreateInfoKHR info = {.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = surface,
                                           .minImageCount = images, .imageFormat = format,
                                           .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
                                           .imageExtent = {width, height}, .imageArrayLayers = 1, .imageUsage = usage,
                                           .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
                                           .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
                                           .compositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
                                           .presentMode = VK_PRESENT_MODE_FIFO_KHR, .clipped = VK_TRUE,
                                           .oldSwapchain = old};
    VkSwapchainKHR made;
    check("vkCreateSwapchainKHR", vkCreateSwapchainKHR(device, &info, NULL, &made));
    return made;
}

static uint32_t image_count(VkSwapchainKHR which) {
    uint32_t count = 0;
    check("vkGetSwapchainImagesKHR", vkGetSwapchainImagesKHR(device, which, &count, NULL));
    return count;
}

static VkResult acquire(VkSwapchainKHR which, VkSemaphore acquired, uint32_t *index) {
    return vkAcquireNextImageKHR(device, which, UINT64_MAX, acquired, VK_NULL_HANDLE, index);
}

/* A present of one image after `waited`, with its result in *result. */
static VkResult present(VkSwapchainKHR which, uint32_t index, VkSemaphore waited, VkResult *result) {
    const VkPresentInfoKHR info = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                                   .waitSemaphoreCount = waited != VK_NULL_HANDLE, .pWaitSemaphores = &waited,
                                   .swapchainCount = 1, .pSwapchains = &which, .pImageIndices = &index,
                                   .pResults = result};
    return vkQueuePresentKHR(queue, &info);
}

/* A submit of nothing but a signal of `signaled`, after `waited`. */
static VkResult signal_after(VkSemaphore waited, VkSemaphore signaled) {
    const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    const VkSubmitInfo batch = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = waited != VK_NULL_HANDLE,
                                .pWaitSemaphores = &waited, .pWaitDstStageMask = &stage,
                                .signalSemaphoreCount = signaled != VK_NULL_HANDLE, .pSignalSemaphores = &signaled};
    return vkQueueSubmit(queue, 1, &batch, VK_NULL_HANDLE);
}

static void swapchain_part(void) {
    const VkSemaphore acquired = semaphore(), rendered = semaphore();
    uint32_t index = 9;
    VkResult result = 9;

    /* The direct way, then replaced by the copy way. */
    const VkSwapchainKHR direct = swapchain(VK_FORMAT_B8G8R8A8_UNORM, 3, 64, 48, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                                            VK_NULL_HANDLE);
    check("vkAcquireNextImageKHR", acquire(direct, acquired, &index));
    printf("direct: %u images, acquired %u\n", image_count(direct), index);
    check("vkQueueSubmit", signal_after(acquired, rendered));
    const VkSwapchainKHR copied = swapchain(VK_FORMAT_R8G8B8A8_UNORM, 4, 64, 48, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                                            direct);
    const VkResult replaced_present = present(direct, index, rendered, &result);
    const VkResult replaced_acquire = acquire(direct, acquired, &index);
    printf("the replaced swapchain: present %d, its result %d, acquire %d\n", replaced_present, result,
           replaced_acquire);

    /* The copy way: the app holds two images at once and gets the least recently presented one back. */
    uint32_t first, second, third, fourth, fifth;
    check("vkAcquireNextImageKHR", acquire(copied, acquired, &first));
    check("vkQueueSubmit", signal_after(acquired, rendered));
    check("vkAcquireNextImageKHR", acquire(copied, acquired, &second));
    check("vkQueuePresentKHR", present(copied, second, rendered, &result));
    check("vkQueueSubmit", signal_after(acquired, rendered));
    check("vkQueuePresentKHR", present(copied, first, rendered, NULL));
    check("vkAcquireNextImageKHR", acquire(copied, acquired, &third));
    check("vkQueueSubmit", signal_after(acquired, rendered));
    check("vkAcquireNextImageKHR", acquire(copied, acquired, &fourth));
    check("vkQueuePresentKHR", present(copied, fourth, rendered, NULL));
    check("vkQueueSubmit", signal_after(acquired, rendered));
    check("vkQueuePresentKHR", present(copied, third, rendered, NULL));
    /* This acquire waits for its image's copy, and the process goes into the background meanwhile: MoltenVK's
       acquire waits for the foreground. */
    marker(BACKGROUND_IN_WAIT);
    check("vkAcquireNextImageKHR", acquire(copied, acquired, &fifth));
    printf("copy: %u images; acquired %u %u, presented %u %u (result %d), acquired %u %u, presented %u %u, "
           "acquired %u\n", image_count(copied), first, second, second, first, result, third, fourth, fourth, third,
           fifth);
    check("vkQueueSubmit", signal_after(acquired, rendered));
    check("vkQueuePresentKHR", present(copied, fifth, rendered, NULL));
    /* A present on a queue of another family: the copy runs there, from a command pool of that family. */
    check("vkAcquireNextImageKHR", acquire(copied, acquired, &index));
    check("vkQueueSubmit", signal_after(acquired, rendered));
    const VkPresentInfoKHR elsewhere = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1,
                                        .pWaitSemaphores = &rendered, .swapchainCount = 1, .pSwapchains = &copied,
                                        .pImageIndices = &index};
    printf("copy: presented %u on another family's queue: %d\n", index, vkQueuePresentKHR(other_queue, &elsewhere));

    /* VK_SUBOPTIMAL_KHR for another size than the window's becomes VK_SUCCESS. */
    const VkSwapchainKHR small = swapchain(VK_FORMAT_B8G8R8A8_UNORM, 3, 32, 24, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                                           copied);
    const VkResult small_acquire = acquire(small, acquired, &index);
    check("vkQueueSubmit", signal_after(acquired, rendered));
    const VkResult small_present = present(small, index, rendered, &result);
    printf("32 x 24: acquire %d, present %d, its result %d\n", small_acquire, small_present, result);

    /* The usage and the number of images decide the way too: an input attachment is not MoltenVK's, nor
       are 4 images; the copy keeps a format of MoltenVK's. */
    const VkSwapchainKHR input = swapchain(VK_FORMAT_B8G8R8A8_UNORM, 3, 64, 48,
                                           VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT,
                                           small);
    uint32_t held[3];
    check("vkAcquireNextImageKHR", acquire(input, acquired, &held[0]));
    check("vkAcquireNextImageKHR", vkAcquireNextImageKHR(device, input, 0, acquired, VK_NULL_HANDLE, &held[1]));
    check("vkAcquireNextImageKHR", vkAcquireNextImageKHR(device, input, 0, acquired, VK_NULL_HANDLE, &held[2]));
    const VkResult none_ready = vkAcquireNextImageKHR(device, input, 0, acquired, VK_NULL_HANDLE, &index);
    const VkResult none_in_time = vkAcquireNextImageKHR(device, input, 1000, acquired, VK_NULL_HANDLE, &index);
    printf("input attachment: %u images; the app holds %u %u %u, then acquire %d, with a timeout %d\n",
           image_count(input), held[0], held[1], held[2], none_ready, none_in_time);
    const VkSwapchainKHR four = swapchain(VK_FORMAT_R16G16B16A16_SFLOAT, 4, 64, 48, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                                          input);
    printf("4 images of R16G16B16A16_SFLOAT: %u images\n", image_count(four));
    const VkSwapchainKHR srgb = swapchain(VK_FORMAT_B8G8R8A8_SRGB, 3, 64, 48, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, four);
    printf("B8G8R8A8_SRGB: %u images\n", image_count(srgb));
    vkDestroySwapchainKHR(device, direct, NULL);
    vkDestroySwapchainKHR(device, copied, NULL);
    vkDestroySwapchainKHR(device, small, NULL);
    vkDestroySwapchainKHR(device, input, NULL);
    vkDestroySwapchainKHR(device, four, NULL);
    vkDestroySwapchainKHR(device, srgb, NULL);
    vkDestroySemaphore(device, acquired, NULL);
    vkDestroySemaphore(device, rendered, NULL);
}

/* A thread that sends the program's thread SIGUSR1 after 50 ms. */
static void *nudging(void *unused) {
    (void)unused;
    usleep(50000);
    pthread_kill(program, SIGUSR1);
    return NULL;
}

static void gate_part(void) {
    const VkSemaphore acquired = semaphore(), rendered = semaphore();
    const VkSwapchainKHR direct = swapchain(VK_FORMAT_B8G8R8A8_UNORM, 3, 64, 48, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                                            VK_NULL_HANDLE);
    uint32_t index;
    VkResult result;

    /* The closed gate: the present waits until it opens, and a signal meanwhile does not end the wait. */
    marker(CLOSE_THEN_OPEN);
    check("vkAcquireNextImageKHR", acquire(direct, acquired, &index));
    check("vkQueueSubmit", signal_after(acquired, rendered));
    pthread_t nudger;
    handled_at = 0;
    uint64_t start = now();
    pthread_create(&nudger, NULL, nudging, NULL);
    result = present(direct, index, rendered, NULL);
    uint64_t end = now();
    pthread_join(nudger, NULL);
    printf("the closed gate: present %d, waited at least 100 ms: %s, the signal handled meanwhile: %s\n", result,
           yes(end - start >= 100 * MS), yes(handled_at > start && handled_at + 40 * MS < end));

    /* The background: each call that commits waits until the foreground. */
    const VkSubmitInfo batch = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO};
    const VkSubmitInfo2 batch2 = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    const VkBindSparseInfo bind = {.sType = VK_STRUCTURE_TYPE_BIND_SPARSE_INFO};
    PFN_vkQueueSubmit2KHR submit2 = (PFN_vkQueueSubmit2KHR)vkGetDeviceProcAddr(device, "vkQueueSubmit2KHR");
    PFN_vkAcquireNextImage2KHR acquire2 = (PFN_vkAcquireNextImage2KHR)vkGetDeviceProcAddr(device, "vkAcquireNextImage2KHR");
    const VkAcquireNextImageInfoKHR next = {.sType = VK_STRUCTURE_TYPE_ACQUIRE_NEXT_IMAGE_INFO_KHR, .swapchain = direct,
                                            .timeout = UINT64_MAX, .semaphore = acquired, .deviceMask = 1};
    for (int call = 0; call < 5; call++) {
        const char *names[] = {"vkQueueSubmit", "vkQueueSubmit2KHR", "vkQueueBindSparse", "vkAcquireNextImage2KHR",
                               "vkQueuePresentKHR"};
        if (call == 4) {
            check("vkQueueSubmit", signal_after(acquired, rendered));
        }
        marker(BACKGROUND);
        start = now();
        switch (call) {
        case 0: result = vkQueueSubmit(queue, 1, &batch, VK_NULL_HANDLE); break;
        case 1: result = submit2 != NULL ? submit2(queue, 1, &batch2, VK_NULL_HANDLE) : VK_ERROR_UNKNOWN; break;
        case 2: result = vkQueueBindSparse(queue, 1, &bind, VK_NULL_HANDLE); break;
        case 3: result = acquire2 != NULL ? acquire2(device, &next, &index) : VK_ERROR_UNKNOWN; break;
        default: result = present(direct, index, rendered, NULL); break;
        }
        end = now();
        printf("in the background: %s %d, waited at least 100 ms: %s\n", names[call], result, yes(end - start >= 100 * MS));
    }

    /* The window goes while a present waits at the background gate: it ends then, before the foreground. */
    check("vkAcquireNextImageKHR", acquire(direct, acquired, &index));
    check("vkQueueSubmit", signal_after(acquired, rendered));
    marker(BACKGROUND_THEN_GONE);
    start = now();
    result = present(direct, index, rendered, NULL);
    end = now();
    printf("the window gone in the background: present %d, after 50 ms: %s, before the foreground: %s\n", result,
           yes(end - start >= 50 * MS), yes(end - start < 180 * MS));
    /* A submit waits for the foreground, so the fake's script has ended. */
    check("vkQueueSubmit", vkQueueSubmit(queue, 1, &batch, VK_NULL_HANDLE));

    /* Gone for good, in the foreground: at once. */
    start = now();
    const VkResult gone_acquire = acquire(direct, acquired, &index);
    const VkResult gone_present = present(direct, 0, VK_NULL_HANDLE, &result);
    const VkSwapchainCreateInfoKHR again = {.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = surface,
                                            .minImageCount = 3, .imageFormat = VK_FORMAT_B8G8R8A8_UNORM,
                                            .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
                                            .imageExtent = {64, 48}, .imageArrayLayers = 1,
                                            .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                                            .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
                                            .compositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
                                            .presentMode = VK_PRESENT_MODE_FIFO_KHR, .oldSwapchain = direct};
    VkSwapchainKHR recreated = VK_NULL_HANDLE;
    const VkResult gone_create = vkCreateSwapchainKHR(device, &again, NULL, &recreated);
    VkSurfaceKHR other = VK_NULL_HANDLE;
    struct ANativeWindow *window = window_of(1);
    const VkAndroidSurfaceCreateInfoKHR info = {.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR,
                                                .window = window};
    const VkResult gone_surface = vkCreateAndroidSurfaceKHR(instance, &info, NULL, &other);
    const VkResult retired = acquire(direct, acquired, &index);
    end = now();
    ANativeWindow_release(window);
    printf("gone: acquire %d, present %d (its result %d), a swapchain %d, a surface %d, within 50 ms: %s\n",
           gone_acquire, gone_present, result, gone_create, gone_surface, yes(end - start < 50 * MS));
    printf("the old swapchain, retired although its replacement failed: acquire %d\n", retired);
    vkDestroySwapchainKHR(device, direct, NULL);
    vkDestroySemaphore(device, acquired, NULL);
    vkDestroySemaphore(device, rendered, NULL);
}

int main(void) {
    program = pthread_self();
    struct sigaction action = {.sa_handler = handled};
    sigaction(SIGUSR1, &action, NULL);
    setup();
    fprintf(stderr, "vulkan-window-test: the surface\n");
    surface_part();
    fprintf(stderr, "vulkan-window-test: swapchains\n");
    swapchain_part();
    fprintf(stderr, "vulkan-window-test: the gates\n");
    gate_part();
    fprintf(stderr, "vulkan-window-test: the end\n");
    vkDestroySurfaceKHR(instance, surface, NULL);
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);
    printf("done\n");
    return 0;
}
