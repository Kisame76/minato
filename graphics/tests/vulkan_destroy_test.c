/* ADR-0030's crash in MoltenVK's present (graphics.vulkan_destroy), with MoltenVK, graphics-run's window of 64 x 48
   pixels (window_host.m, a MinatoMetalLayer), its test control and the Metal hook's late-scheduled mode, which runs
   every command buffer's scheduled handlers 100 ms late, as Metal runs them on a loaded machine. MoltenVK presents a
   drawable from its present command buffer's scheduled handler, which first counts the present on the image's
   swapchain, with no check that the swapchain is still there [verified: MVKImage.mm at v1.4.2]. As an app replaces its
   swapchain on a resize, this test presents, then makes a new swapchain with the old one as oldSwapchain and destroys
   the old one at once, five times: each destroy must wait until its present's drawable went its way, or the late
   handler finds the destroyed swapchain. */
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
#define ROUNDS 5

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

static int64_t now_ns(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000000000 + now.tv_nsec;
}

/* A swapchain of 3 FIFO images on the surface, replacing `old` when it is not VK_NULL_HANDLE. */
static VkSwapchainKHR make_swapchain(VkSwapchainKHR old) {
    const VkSwapchainCreateInfoKHR info = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = surface, .minImageCount = 3,
        .imageFormat = VK_FORMAT_B8G8R8A8_UNORM, .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
        .imageExtent = {WIDTH, HEIGHT}, .imageArrayLayers = 1, .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE, .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
        .compositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR, .presentMode = VK_PRESENT_MODE_FIFO_KHR,
        .clipped = VK_TRUE, .oldSwapchain = old};
    VkSwapchainKHR made;
    check("vkCreateSwapchainKHR", vkCreateSwapchainKHR(device, &info, NULL, &made));
    return made;
}

static void setup(void) {
    const char *extensions[] = {"VK_KHR_surface", "VK_KHR_android_surface"};
    const VkApplicationInfo application = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                           .pApplicationName = "destroy", .apiVersion = VK_API_VERSION_1_1};
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
    swapchain = make_swapchain(VK_NULL_HANDLE);
}

/* One frame on the swapchain: an image acquired, made presentable and presented. */
static void frame(void) {
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
    const VkPresentInfoKHR info = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1,
                                   .pWaitSemaphores = &rendered, .swapchainCount = 1, .pSwapchains = &swapchain,
                                   .pImageIndices = &index};
    check("vkQueuePresentKHR", vkQueuePresentKHR(queue, &info));
}

int main(void) {
    setup();
    int waited = 0;
    for (int round = 0; round < ROUNDS; round++) {
        frame();
        const VkSwapchainKHR old = swapchain;
        swapchain = make_swapchain(old);
        const int64_t start = now_ns();
        vkDestroySwapchainKHR(device, old, NULL);
        /* The present's scheduled handler runs 100 ms late: a destroy that waited for its drawable took most of it. */
        waited += now_ns() - start >= 50000000;
    }
    printf("%d swapchains destroyed right after their presents, %d of them after their drawables went\n", ROUNDS,
           waited);
    /* vkDeviceWaitIdle does not wait for the scheduled handlers [measured]: the 5 late ones run within this second. */
    usleep(1000000);
    check("vkDeviceWaitIdle", vkDeviceWaitIdle(device));
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
