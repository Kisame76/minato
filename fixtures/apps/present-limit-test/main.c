// present-limit-test (ADR-0078 decision 4): a NativeActivity on native_app_glue that presents without pacing of its
// own, as a game's busy loop does, so that graphics/host's hold is all that keeps it to the frame rate limit. It is
// built once and started as two apps by the function their manifests name: present-limit-gles swaps with EGL at a
// swap interval of 0 (ANativeActivity_onCreateGLES), present-limit-vulkan presents a FIFO swapchain in a loop
// (ANativeActivity_onCreateVulkan). Once the window can be drawn to, it draws for 1 s to settle, then counts its
// presents for 3 s by the monotonic clock, each frame cleared to a color of its own. Its tag ("present-limit-gles" or
// "present-limit-vulkan") logs what every device answers alike, the tag with "-device" the presents a second, which
// depend on the limit and the screen. Then it calls ANativeActivity_finish.
#define VK_USE_PLATFORM_ANDROID_KHR
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <android/log.h>
#include <android/looper.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <vulkan/vulkan.h>

#include "android_native_app_glue.h"

// The log's tags: each app has its own.
static const char *tag = "present-limit-gles", *device_tag = "present-limit-gles-device";

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, tag, __VA_ARGS__)
#define DEVICE(...) __android_log_print(ANDROID_LOG_INFO, device_tag, __VA_ARGS__)

enum { kImages = 8 };
static const int64_t kSettle = 1000000000, kCount = 3000000000;

static struct {
    bool vulkan;  // present-limit-vulkan, else present-limit-gles
    bool ready;   // the window can be drawn to
    bool done;    // the presents are counted
    int frame;
    int64_t started;  // when the window could be drawn to, 0 before
    int64_t counting;  // when the count began, 0 before
    int presents;      // since the count began
} test;

static struct {
    EGLDisplay display;
    EGLSurface surface;
    EGLContext context;
} gl = {EGL_NO_DISPLAY, EGL_NO_SURFACE, EGL_NO_CONTEXT};

static struct {
    VkInstance instance;
    VkPhysicalDevice physical;
    VkDevice device;
    uint32_t family;
    VkQueue queue;
    VkSurfaceKHR surface;
    VkSwapchainKHR swapchain;
    uint32_t count;
    VkImage images[kImages];
    VkSemaphore rendered[kImages];
    VkSemaphore acquired;
    VkCommandPool pool;
    VkCommandBuffer commands;
    VkFence fence;
} vk;

static int64_t now_ns(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000000000 + now.tv_nsec;
}

static bool ok(VkResult result, const char *what) {
    if (result == VK_SUCCESS) return true;
    LOG("%s: %d", what, result);
    return false;
}

// ---- EGL

static bool start_gles(ANativeWindow *window) {
    gl.display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!eglInitialize(gl.display, NULL, NULL)) {
        LOG("eglInitialize failed: 0x%04x", eglGetError());
        return false;
    }
    static const EGLint attributes[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
                                        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                                        EGL_NONE};
    EGLConfig config;
    EGLint configs = 0;
    if (!eglChooseConfig(gl.display, attributes, &config, 1, &configs) || configs == 0) {
        LOG("no RGBA8888 config");
        return false;
    }
    static const EGLint version[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    gl.surface = eglCreateWindowSurface(gl.display, config, window, NULL);
    gl.context = eglCreateContext(gl.display, config, EGL_NO_CONTEXT, version);
    if (gl.surface == EGL_NO_SURFACE || gl.context == EGL_NO_CONTEXT ||
        !eglMakeCurrent(gl.display, gl.surface, gl.surface, gl.context)) {
        LOG("no window surface or context: 0x%04x", eglGetError());
        return false;
    }
    // Unpaced: the swap does not wait for the display.
    LOG("swap interval 0: %s", eglSwapInterval(gl.display, 0) ? "set" : "refused");
    return true;
}

static void stop_gles(void) {
    if (gl.display == EGL_NO_DISPLAY) return;
    eglMakeCurrent(gl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (gl.context != EGL_NO_CONTEXT) eglDestroyContext(gl.display, gl.context);
    if (gl.surface != EGL_NO_SURFACE) eglDestroySurface(gl.display, gl.surface);
    eglTerminate(gl.display);
    gl.display = EGL_NO_DISPLAY;
    gl.surface = EGL_NO_SURFACE;
    gl.context = EGL_NO_CONTEXT;
}

static bool frame_gles(int frame) {
    glClearColor((float)(frame & 1), (float)((frame >> 1) & 1), (float)((frame >> 2) & 1), 1);
    glClear(GL_COLOR_BUFFER_BIT);
    return eglSwapBuffers(gl.display, gl.surface) == EGL_TRUE;
}

// ---- Vulkan

static bool start_vulkan(ANativeWindow *window) {
    const char *const instance_names[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
    const VkApplicationInfo application = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                           .pApplicationName = "present-limit-test", .apiVersion = VK_API_VERSION_1_1};
    const VkInstanceCreateInfo instance = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                           .pApplicationInfo = &application, .enabledExtensionCount = 2,
                                           .ppEnabledExtensionNames = instance_names};
    if (!ok(vkCreateInstance(&instance, NULL, &vk.instance), "vkCreateInstance")) return false;
    uint32_t devices = 1;
    const VkResult enumerated = vkEnumeratePhysicalDevices(vk.instance, &devices, &vk.physical);
    if ((enumerated != VK_SUCCESS && enumerated != VK_INCOMPLETE) || devices == 0) {
        LOG("no physical device: %d", enumerated);
        return false;
    }
    const VkAndroidSurfaceCreateInfoKHR surface = {.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR,
                                                   .window = window};
    if (!ok(vkCreateAndroidSurfaceKHR(vk.instance, &surface, NULL, &vk.surface), "vkCreateAndroidSurfaceKHR")) {
        return false;
    }
    uint32_t families = 16;
    VkQueueFamilyProperties properties[16];
    vkGetPhysicalDeviceQueueFamilyProperties(vk.physical, &families, properties);
    vk.family = UINT32_MAX;
    for (uint32_t family = 0; family < families && vk.family == UINT32_MAX; ++family) {
        VkBool32 presents = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(vk.physical, family, vk.surface, &presents);
        if ((properties[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) && presents) vk.family = family;
    }
    if (vk.family == UINT32_MAX) {
        LOG("no queue family with graphics and presents");
        return false;
    }
    const float priority = 1;
    const VkDeviceQueueCreateInfo queue = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                           .queueFamilyIndex = vk.family, .queueCount = 1,
                                           .pQueuePriorities = &priority};
    const char *const device_names[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    const VkDeviceCreateInfo device = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
                                       .pQueueCreateInfos = &queue, .enabledExtensionCount = 1,
                                       .ppEnabledExtensionNames = device_names};
    if (!ok(vkCreateDevice(vk.physical, &device, NULL, &vk.device), "vkCreateDevice")) return false;
    vkGetDeviceQueue(vk.device, vk.family, 0, &vk.queue);

    // An R8G8B8A8_UNORM swapchain at the surface's size with FIFO, the one mode Android's surfaces offer every app,
    // cleared by transfers.
    VkSurfaceCapabilitiesKHR capabilities;
    if (!ok(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk.physical, vk.surface, &capabilities),
            "vkGetPhysicalDeviceSurfaceCapabilitiesKHR")) {
        return false;
    }
    uint32_t images = capabilities.minImageCount > 3 ? capabilities.minImageCount : 3;
    if (capabilities.maxImageCount != 0 && images > capabilities.maxImageCount) images = capabilities.maxImageCount;
    VkExtent2D extent = capabilities.currentExtent;
    if (extent.width == UINT32_MAX) {
        extent = (VkExtent2D){(uint32_t)ANativeWindow_getWidth(window), (uint32_t)ANativeWindow_getHeight(window)};
    }
    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    const VkCompositeAlphaFlagBitsKHR alphas[4] = {VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR, VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
                                                   VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
                                                   VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR};
    for (int index = 3; index >= 0; --index) {
        if (capabilities.supportedCompositeAlpha & alphas[index]) alpha = alphas[index];
    }
    const VkSwapchainCreateInfoKHR swapchain = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = vk.surface, .minImageCount = images,
        .imageFormat = VK_FORMAT_R8G8B8A8_UNORM, .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
        .imageExtent = extent, .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE, .preTransform = capabilities.currentTransform,
        .compositeAlpha = alpha, .presentMode = VK_PRESENT_MODE_FIFO_KHR, .clipped = VK_TRUE};
    if (!ok(vkCreateSwapchainKHR(vk.device, &swapchain, NULL, &vk.swapchain), "vkCreateSwapchainKHR")) return false;
    vk.count = kImages;
    if (!ok(vkGetSwapchainImagesKHR(vk.device, vk.swapchain, &vk.count, vk.images), "vkGetSwapchainImagesKHR")) {
        return false;
    }
    const VkSemaphoreCreateInfo semaphore = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (uint32_t index = 0; index < vk.count; ++index) {
        if (!ok(vkCreateSemaphore(vk.device, &semaphore, NULL, &vk.rendered[index]), "vkCreateSemaphore")) return false;
    }
    const VkCommandPoolCreateInfo pool = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                          .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                          .queueFamilyIndex = vk.family};
    const VkFenceCreateInfo fence = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                                     .flags = VK_FENCE_CREATE_SIGNALED_BIT};
    if (!ok(vkCreateSemaphore(vk.device, &semaphore, NULL, &vk.acquired), "vkCreateSemaphore") ||
        !ok(vkCreateCommandPool(vk.device, &pool, NULL, &vk.pool), "vkCreateCommandPool") ||
        !ok(vkCreateFence(vk.device, &fence, NULL, &vk.fence), "vkCreateFence")) {
        return false;
    }
    const VkCommandBufferAllocateInfo allocate = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                                  .commandPool = vk.pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                                  .commandBufferCount = 1};
    if (!ok(vkAllocateCommandBuffers(vk.device, &allocate, &vk.commands), "vkAllocateCommandBuffers")) return false;
    LOG("FIFO swapchain: made");
    return true;
}

static void stop_vulkan(void) {
    if (vk.device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(vk.device);
        if (vk.fence != VK_NULL_HANDLE) vkDestroyFence(vk.device, vk.fence, NULL);
        if (vk.pool != VK_NULL_HANDLE) vkDestroyCommandPool(vk.device, vk.pool, NULL);
        if (vk.acquired != VK_NULL_HANDLE) vkDestroySemaphore(vk.device, vk.acquired, NULL);
        for (uint32_t index = 0; index < vk.count; ++index) {
            if (vk.rendered[index] != VK_NULL_HANDLE) vkDestroySemaphore(vk.device, vk.rendered[index], NULL);
        }
        if (vk.swapchain != VK_NULL_HANDLE) vkDestroySwapchainKHR(vk.device, vk.swapchain, NULL);
        vkDestroyDevice(vk.device, NULL);
    }
    if (vk.surface != VK_NULL_HANDLE) vkDestroySurfaceKHR(vk.instance, vk.surface, NULL);
    if (vk.instance != VK_NULL_HANDLE) vkDestroyInstance(vk.instance, NULL);
    memset(&vk, 0, sizeof vk);
}

static bool frame_vulkan(int frame) {
    if (!ok(vkWaitForFences(vk.device, 1, &vk.fence, VK_TRUE, UINT64_MAX), "vkWaitForFences") ||
        !ok(vkResetFences(vk.device, 1, &vk.fence), "vkResetFences")) {
        return false;
    }
    uint32_t index = 0;
    const VkResult acquired =
        vkAcquireNextImageKHR(vk.device, vk.swapchain, UINT64_MAX, vk.acquired, VK_NULL_HANDLE, &index);
    if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) {
        LOG("vkAcquireNextImageKHR: %d", acquired);
        return false;
    }
    const VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    if (!ok(vkResetCommandBuffer(vk.commands, 0), "vkResetCommandBuffer") ||
        !ok(vkBeginCommandBuffer(vk.commands, &begin), "vkBeginCommandBuffer")) {
        return false;
    }
    const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = 0,
                                    .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = vk.images[index],
                                    .subresourceRange = range};
    vkCmdPipelineBarrier(vk.commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0,
                         NULL, 1, &barrier);
    const VkClearColorValue color = {.float32 = {(float)(frame & 1), (float)((frame >> 1) & 1),
                                                 (float)((frame >> 2) & 1), 1}};
    vkCmdClearColorImage(vk.commands, vk.images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = 0;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(vk.commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL,
                         0, NULL, 1, &barrier);
    const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    const VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1,
                                 .pWaitSemaphores = &vk.acquired, .pWaitDstStageMask = &stage,
                                 .commandBufferCount = 1, .pCommandBuffers = &vk.commands, .signalSemaphoreCount = 1,
                                 .pSignalSemaphores = &vk.rendered[index]};
    if (!ok(vkEndCommandBuffer(vk.commands), "vkEndCommandBuffer") ||
        !ok(vkQueueSubmit(vk.queue, 1, &submit, vk.fence), "vkQueueSubmit")) {
        return false;
    }
    const VkPresentInfoKHR present = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1,
                                      .pWaitSemaphores = &vk.rendered[index], .swapchainCount = 1,
                                      .pSwapchains = &vk.swapchain, .pImageIndices = &index};
    const VkResult presented = vkQueuePresentKHR(vk.queue, &present);
    if (presented != VK_SUCCESS && presented != VK_SUBOPTIMAL_KHR) {
        LOG("vkQueuePresentKHR: %d", presented);
        return false;
    }
    return true;
}

// ---- The frames

// One frame of the busy loop; the count's end logs the presents a second and finishes the activity.
static void draw(struct android_app *app) {
    if (!(test.vulkan ? frame_vulkan(test.frame) : frame_gles(test.frame))) {
        LOG("the present of frame %d failed", test.frame);
        test.done = true;
        ANativeActivity_finish(app->activity);
        return;
    }
    ++test.frame;
    const int64_t now = now_ns();
    if (test.counting == 0) {
        if (now - test.started >= kSettle) test.counting = now;
        return;
    }
    ++test.presents;
    if (now - test.counting < kCount) return;
    const int64_t a_second = (int64_t)test.presents * 1000000000 / (now - test.counting);
    DEVICE("presents a second: %lld", (long long)a_second);
    LOG("presents counted");
    test.done = true;
    ANativeActivity_finish(app->activity);
}

static void on_command(struct android_app *app, int32_t command) {
    switch (command) {
        case APP_CMD_INIT_WINDOW:
            if (test.done || test.ready) break;
            if (test.vulkan ? start_vulkan(app->window) : start_gles(app->window)) {
                test.ready = true;
                if (test.started == 0) test.started = now_ns();
            } else {
                LOG("the window cannot be drawn to");
            }
            break;
        case APP_CMD_TERM_WINDOW:
            if (!test.done && test.ready) LOG("the window went at frame %d", test.frame);
            test.ready = false;
            if (test.vulkan) {
                stop_vulkan();
            } else {
                stop_gles();
            }
            break;
        default:
            break;
    }
}

static void start(ANativeActivity *activity, void *saved_state, size_t saved_state_size, bool vulkan) {
    test.vulkan = vulkan;
    if (vulkan) {
        tag = "present-limit-vulkan";
        device_tag = "present-limit-vulkan-device";
    }
    ANativeActivity_onCreate(activity, saved_state, saved_state_size);
}

// present-limit-gles's entry, which its manifest names (android.app.func_name).
__attribute__((visibility("default"))) void ANativeActivity_onCreateGLES(ANativeActivity *activity, void *saved_state,
                                                                         size_t saved_state_size) {
    start(activity, saved_state, saved_state_size, false);
}

// present-limit-vulkan's entry.
__attribute__((visibility("default"))) void ANativeActivity_onCreateVulkan(ANativeActivity *activity, void *saved_state,
                                                                           size_t saved_state_size) {
    start(activity, saved_state, saved_state_size, true);
}

// A game's loop: the events without waiting while the window can be drawn to, a frame after each round.
void android_main(struct android_app *app) {
    app->onAppCmd = on_command;
    while (!app->destroyRequested) {
        struct android_poll_source *source = NULL;
        const bool drawing = test.ready && !test.done;
        const int result = ALooper_pollOnce(drawing ? 0 : -1, NULL, NULL, (void **)&source);
        if ((result == LOOPER_ID_MAIN || result == LOOPER_ID_INPUT) && source != NULL) source->process(app, source);
        if (test.ready && !test.done && !app->destroyRequested) draw(app);
    }
    if (test.vulkan) {
        stop_vulkan();
    } else {
        stop_gles();
    }
    LOG("done");
}
