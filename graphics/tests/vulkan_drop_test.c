/* ADR-0041 decision 11 c) on the Mac (graphics.vulkan_drop): what graphics/host drops at a drawable's present, with
   MoltenVK, graphics-run's window of 64 x 48 pixels (window_host.m, a MinatoMetalLayer), its test control and the
   Metal hook (count mode), which sees the drawables Core Animation gets. On a refresh grid of 10 ms from 1 s, each time
   below goes to Core Animation less half a period (decision 11).
   - Six presents with VK_GOOGLE_display_timing, IDs 1 to 6 at 2 to 2.05 s, 10 ms apart, each 50 ms after the one
     before, so Core Animation has shown it, on the swapchain's image MoltenVK acquired the longest ago. The one asked
     for at 2.02 s is not shown (the test control's MINATO_TEST_DROP, in the place of the rule): Core Animation gets
     1.995, 2.005, 2.025, 2.035 and 2.045 s.
   - MoltenVK's presented handler still runs for the drawable not shown, once MoltenVK lets it go as it acquires that
     image again for the sixth: MoltenVK keeps 6 past timings, ID 3's after ID 5's, while
     vkGetPastPresentationTimingGOOGLE gives the app 5, IDs 1, 2, 4, 5 and 6 with the times they asked for, as Android
     leaves a dropped frame out.
   - Three more presents, IDs 7 to 9 at 2.06 to 2.08 s, are not shown either: their acquires let the drawables of IDs 4
     to 6 go, so those presented handlers run even when Core Animation never reports their drawables shown, which a
     windowless layer's drawables in a starved process may never be (with taskpolicy -c maintenance about half the
     runs, under a loaded Mac's whole ctest now and then); MoltenVK keeps the last three drawables, and with them their
     timings, until the swapchain goes. */
#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>

#include <android/native_window.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "test_control.h"

#define WIDTH 64
#define HEIGHT 48

static VkInstance instance;
static VkPhysicalDevice physical;
static VkDevice device;
static VkQueue queue;
static VkSurfaceKHR surface;
static VkSwapchainKHR swapchain;
static VkCommandPool pool;
static VkCommandBuffer commands;
static VkSemaphore acquired, rendered;

static void check(const char *what, VkResult result) {
    if (result != VK_SUCCESS) {
        printf("%s: %d\n", what, result);
        exit(1);
    }
}

static void setup(void) {
    const char *extensions[] = {"VK_KHR_surface", "VK_KHR_android_surface"};
    const VkApplicationInfo application = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "drop",
                                           .apiVersion = VK_API_VERSION_1_1};
    const VkInstanceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                       .pApplicationInfo = &application, .enabledExtensionCount = 2,
                                       .ppEnabledExtensionNames = extensions};
    check("vkCreateInstance", vkCreateInstance(&info, NULL, &instance));
    uint32_t count = 1;
    check("vkEnumeratePhysicalDevices", vkEnumeratePhysicalDevices(instance, &count, &physical));
    const float priority = 1;
    const VkDeviceQueueCreateInfo family = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1,
                                            .pQueuePriorities = &priority};
    const char *device_extensions[] = {"VK_KHR_swapchain", "VK_GOOGLE_display_timing"};
    const VkDeviceCreateInfo created = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
                                        .pQueueCreateInfos = &family, .enabledExtensionCount = 2,
                                        .ppEnabledExtensionNames = device_extensions};
    check("vkCreateDevice", vkCreateDevice(physical, &created, NULL, &device));
    vkGetDeviceQueue(device, 0, 0, &queue);
    const VkCommandPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                               .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
    check("vkCreateCommandPool", vkCreateCommandPool(device, &pool_info, NULL, &pool));
    const VkCommandBufferAllocateInfo allocate = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                                  .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                                  .commandBufferCount = 1};
    check("vkAllocateCommandBuffers", vkAllocateCommandBuffers(device, &allocate, &commands));
    const VkSemaphoreCreateInfo semaphore = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    check("vkCreateSemaphore", vkCreateSemaphore(device, &semaphore, NULL, &acquired));
    check("vkCreateSemaphore", vkCreateSemaphore(device, &semaphore, NULL, &rendered));

    struct ANativeWindow *(*create)(uint64_t, int32_t, int32_t) =
        (struct ANativeWindow * (*)(uint64_t, int32_t, int32_t))
            dlsym(dlopen("libnativewindow.so", RTLD_NOW), "minato_ANativeWindow_create");
    const uint64_t id = minato_test(MINATO_TEST_WINDOW, 0, 0, 0);
    if (create == NULL || id == 0) {
        printf("no window\n");
        exit(1);
    }
    struct ANativeWindow *window = create(id, WIDTH, HEIGHT);
    const VkAndroidSurfaceCreateInfoKHR surface_info = {.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR,
                                                        .window = window};
    check("vkCreateAndroidSurfaceKHR", vkCreateAndroidSurfaceKHR(instance, &surface_info, NULL, &surface));
    ANativeWindow_release(window);
    const VkSwapchainCreateInfoKHR swapchain_info = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = surface, .minImageCount = 3,
        .imageFormat = VK_FORMAT_B8G8R8A8_UNORM, .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
        .imageExtent = {WIDTH, HEIGHT}, .imageArrayLayers = 1, .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE, .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
        .compositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR, .presentMode = VK_PRESENT_MODE_FIFO_KHR,
        .clipped = VK_TRUE};
    check("vkCreateSwapchainKHR", vkCreateSwapchainKHR(device, &swapchain_info, NULL, &swapchain));
}

static uint64_t now_ns(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
}

/* One frame: an image acquired, made presentable and presented with ID `id` asked for at `time`. */
static VkResult frame(uint32_t id, uint64_t time) {
    uint32_t index = 0, count = 8;
    VkImage images[8];
    check("vkQueueWaitIdle", vkQueueWaitIdle(queue));
    check("vkGetSwapchainImagesKHR", vkGetSwapchainImagesKHR(device, swapchain, &count, images));
    check("vkAcquireNextImageKHR", vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, acquired, VK_NULL_HANDLE,
                                                         &index));
    const VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    check("vkBeginCommandBuffer", vkBeginCommandBuffer(commands, &begin));
    const VkImageMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                          .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                          .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                                          .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                          .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = images[index],
                                          .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL,
                         0, NULL, 1, &barrier);
    check("vkEndCommandBuffer", vkEndCommandBuffer(commands));
    const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    const VkSubmitInfo batch = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1,
                                .pWaitSemaphores = &acquired, .pWaitDstStageMask = &stage, .commandBufferCount = 1,
                                .pCommandBuffers = &commands, .signalSemaphoreCount = 1,
                                .pSignalSemaphores = &rendered};
    check("vkQueueSubmit", vkQueueSubmit(queue, 1, &batch, VK_NULL_HANDLE));
    const VkPresentTimeGOOGLE asked = {.presentID = id, .desiredPresentTime = time};
    const VkPresentTimesInfoGOOGLE times = {.sType = VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE,
                                            .swapchainCount = 1, .pTimes = &asked};
    const VkPresentInfoKHR info = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .pNext = &times,
                                   .waitSemaphoreCount = 1, .pWaitSemaphores = &rendered, .swapchainCount = 1,
                                   .pSwapchains = &swapchain, .pImageIndices = &index};
    return vkQueuePresentKHR(queue, &info);
}

int main(void) {
    setup();
    const PFN_vkGetPastPresentationTimingGOOGLE past =
        (PFN_vkGetPastPresentationTimingGOOGLE)vkGetDeviceProcAddr(device, "vkGetPastPresentationTimingGOOGLE");
    minato_test(MINATO_TEST_VSYNC_GRID, 1000000000, 10000000, 0);
    printf("the present at 2.02 s to drop: %s\n",
           minato_test(MINATO_TEST_DROP, 2020000000, 0, 0) == 1 ? "noted" : "refused");
    int noted = 1;
    for (uint64_t time = 2060000000; time <= 2080000000; time += 10000000) {
        noted &= minato_test(MINATO_TEST_DROP, time, 0, 0) == 1;
    }
    printf("the presents at 2.06 to 2.08 s to drop: %s\n", noted ? "noted" : "refused");
    minato_test(MINATO_TEST_PRESENTED_AT, 1, 0, 0);
    printf("presents:");
    for (uint32_t id = 1; id <= 9; id++) {
        printf(" %d", frame(id, 2000000000 + (id - 1) * 10000000ull));
        usleep(50000);
    }
    printf("\n");
    check("vkQueueWaitIdle", vkQueueWaitIdle(queue));

    /* The five drawables shown and the six presented handlers, by the monotonic clock (sleeps of 1 ms can each take
       much longer on a loaded Mac), and time for a sixth drawable that must not come. */
    const uint64_t until = now_ns() + 10000000000ull;
    uint32_t kept = 0;
    while (now_ns() < until && (minato_test(MINATO_TEST_PRESENTED_NTH, 4, 0, 0) == 0 ||
                                (past(device, swapchain, &kept, NULL), kept < 6))) {
        usleep(1000);
    }
    usleep(100000);
    printf("Core Animation got:\n");
    for (uint64_t index = 0; index < 8; index++) {
        const uint64_t how = minato_test(MINATO_TEST_PRESENTED_NTH, index, 0, 0);
        if (how == 0) {
            break;
        }
        if (how == UINT64_MAX) {
            printf("  without a time\n");
        } else {
            printf("  at %llu ns\n", (unsigned long long)how);
        }
    }
    uint32_t given = 8;
    kept = 0;
    VkPastPresentationTimingGOOGLE timings[8];
    check("vkGetPastPresentationTimingGOOGLE", past(device, swapchain, &kept, NULL));
    const VkResult result = past(device, swapchain, &given, timings);
    printf("past timings: MoltenVK keeps %u, the app gets %u (%d)\n", kept, given, result);
    for (uint32_t index = 0; index < given; index++) {
        printf("  ID %u asked for %llu ns\n", timings[index].presentID,
               (unsigned long long)timings[index].desiredPresentTime);
    }
    vkDestroySwapchainKHR(device, swapchain, NULL);
    vkDestroySurfaceKHR(instance, surface, NULL);
    vkDestroySemaphore(device, acquired, NULL);
    vkDestroySemaphore(device, rendered, NULL);
    vkDestroyCommandPool(device, pool, NULL);
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);
    printf("done\n");
    return 0;
}
