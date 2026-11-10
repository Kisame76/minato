/* ADR-0078 decision 4 on the Mac (graphics.present_limit_vulkan): graphics/host holds an app's Vulkan presents to the
   frame rate limit, with MoltenVK and graphics-run's window of 64 x 48 pixels (its test control sets the limit as App
   does at a session's start). An app that presents in a loop (FIFO, the one mode Android's and Minato's surfaces
   offer, without pacing of its own) gets more than 70 presents a second without a limit (100-120 on a loaded Mac, the
   better of two seconds), at most 30 a second (one more where a late present catches up on the limit's grid; at least
   20, with room for a loaded Mac) with a limit of 30, and more than 70 again without. Each case counts the presents
   of one second by the monotonic clock. */
#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>

#include <android/native_window.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

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
    const VkApplicationInfo application = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "limit",
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
    const char *device_extensions[] = {"VK_KHR_swapchain"};
    const VkDeviceCreateInfo created = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
                                        .pQueueCreateInfos = &family, .enabledExtensionCount = 1,
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

/* One frame: an image acquired, made presentable and presented. */
static VkResult frame(void) {
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
    const VkPresentInfoKHR info = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                                   .waitSemaphoreCount = 1, .pWaitSemaphores = &rendered, .swapchainCount = 1,
                                   .pSwapchains = &swapchain, .pImageIndices = &index};
    return vkQueuePresentKHR(queue, &info);
}

static uint64_t now_ns(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000u + (uint64_t)now.tv_nsec;
}

/* The presents of one second, each frame presented as soon as the one before. */
static int presents_in_a_second(void) {
    const uint64_t end = now_ns() + 1000000000u;
    int presents = 0;
    while (now_ns() < end) {
        presents += frame() == VK_SUCCESS;
    }
    return presents;
}

/* Without a limit, the better of two seconds: a loaded Mac starves a process for a moment now and then. */
static int unheld(void) {
    const int first = presents_in_a_second(), second = presents_in_a_second();
    return first > second ? first : second;
}

static const char *yes(int condition) {
    return condition ? "yes" : "no";
}

static void limit(int rate) {
    printf("the limit %d: %s\n", rate, minato_test(MINATO_TEST_FRAME_RATE_LIMIT, (uint64_t)rate, 0, 0) == 1
                                           ? "set" : "refused");
}

int main(void) {
    setup();
    presents_in_a_second();  // warm up
    printf("in a loop without a limit: more than 70 a second: %s\n", yes(unheld() > 70));
    limit(30);
    const int held = presents_in_a_second();
    printf("in a loop at the limit 30: 20 to 30 a second: %s\n", yes(held >= 20 && held <= 32));
    limit(0);
    printf("in a loop without a limit again: more than 70 a second: %s\n", yes(unheld() > 70));
    check("vkQueueWaitIdle", vkQueueWaitIdle(queue));
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
