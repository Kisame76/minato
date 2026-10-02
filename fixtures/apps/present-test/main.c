// present-test (ADR-0041 decision 11, test 3; step d), the Grafik chat's: a NativeActivity on native_app_glue that
// asks when its frames are to be presented, as Swappy does. It is built once and started as two apps by the
// function their manifests name: present-gles with EGL_ANDROID_presentation_time (ANativeActivity_onCreateGLES)
// and present-vulkan with VK_GOOGLE_display_timing (ANativeActivity_onCreateVulkan).
// It draws on AChoreographer's vsync callbacks (AChoreographer_postVsyncCallback, by dlsym above the minSdk; its frame
// callbacks where those are missing), at 60 and then at 120 frames a second (ANativeWindow_setFrameRate, by dlsym
// above the minSdk). Per rate: 30 callbacks to settle, the period as the median of the last 16 intervals,
// then 600 frames in three blocks of 200, a frame on every first, second and third callback, each presented 1, 2
// and 3 periods after its callback's frame time, so the times asked for rise by 1, 2 and 3 periods. Each frame
// clears the window to a color of its own.
// The app's tag ("present-gles" or "present-vulkan") logs what every Android 15 device answers alike: whether the
// extension is there, and per rate the frames, the times the API took and the presents that succeeded. Its tag with
// "-device" logs the periods, Vulkan's refresh duration and how many past timings came. present.csv in
// internalDataPath holds a row per frame in the order of the presents, so that a recorder's drawable k is frame k:
// the rate, the interval, the callback's frame time, the time asked for, the expected presentation time of the
// callback's frame timeline 0 (ADR-0041 decision 11; 0 without the vsync callbacks), and for Vulkan the past timing of
// its present ID (zeros where none came). Then it calls ANativeActivity_finish.
#define VK_USE_PLATFORM_ANDROID_KHR
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <android/choreographer.h>
#include <android/log.h>
#include <android/looper.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#include "android_native_app_glue.h"

// The log's tags: each app has its own.
static const char *tag = "present-gles", *device_tag = "present-gles-device";

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, tag, __VA_ARGS__)
#define DEVICE(...) __android_log_print(ANDROID_LOG_INFO, device_tag, __VA_ARGS__)

enum { kSettle = 30, kIntervals = 16, kBlock = 200, kFrames = 3 * kBlock, kRates = 2, kRows = kRates * kFrames };
enum { kImages = 8, kTimings = 16 };

static const int rates[kRates] = {60, 120};

// A frame's row of present.csv.
struct row {
    int rate, interval;
    int64_t frame_time, asked, timeline0;
    VkPastPresentationTimingGOOGLE past;  // presentID 0 until its timing came
};

static struct {
    bool vulkan;  // present-vulkan, else present-gles
    struct android_app *app;
    AChoreographer *choreographer;
    bool ready;      // the window can be drawn to
    bool posting;    // a frame callback is posted
    bool done;       // every frame is drawn
    bool extension;  // EGL_ANDROID_presentation_time or VK_GOOGLE_display_timing
    int rate;        // the index into rates
    int callbacks;   // of the rate's settling, then of the block
    int64_t previous;                // the frame time of the callback before, 0 for none
    int64_t intervals[kIntervals];   // the last ones while settling, ns
    int64_t period;                  // the rate's, 0 while settling
    int interval;                    // a frame on every interval-th callback, presented interval periods on
    int frames;                      // of all rates so far
    int block_frames;                // of the block so far
    int times[kRates], presents[kRates];
    struct row rows[kRows];
} test;

static struct {
    EGLDisplay display;
    EGLSurface surface;
    EGLContext context;
    PFNEGLPRESENTATIONTIMEANDROIDPROC presentation_time;
} gl = {EGL_NO_DISPLAY, EGL_NO_SURFACE, EGL_NO_CONTEXT, NULL};

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
    PFN_vkGetRefreshCycleDurationGOOGLE refresh_cycle;
    PFN_vkGetPastPresentationTimingGOOGLE past_timing;
} vk;

static bool ok(VkResult result, const char *what) {
    if (result == VK_SUCCESS) return true;
    LOG("%s: %d", what, result);
    return false;
}

static bool has(const char *list, const char *name) {
    const size_t length = strlen(name);
    for (const char *at = list; at != NULL && (at = strstr(at, name)) != NULL; at += length) {
        if ((at == list || at[-1] == ' ') && (at[length] == ' ' || at[length] == '\0')) return true;
    }
    return false;
}

// ---- EGL

static bool start_gles(ANativeWindow *window) {
    gl.display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!eglInitialize(gl.display, NULL, NULL)) {
        LOG("eglInitialize failed: 0x%04x", eglGetError());
        return false;
    }
    static const EGLint attributes[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
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
    test.extension = has(eglQueryString(gl.display, EGL_EXTENSIONS), "EGL_ANDROID_presentation_time");
    LOG("EGL_ANDROID_presentation_time: %s", test.extension ? "there" : "not there");
    if (test.extension) {
        gl.presentation_time = (PFNEGLPRESENTATIONTIMEANDROIDPROC)eglGetProcAddress("eglPresentationTimeANDROID");
        test.extension = gl.presentation_time != NULL;
    }
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

// A clear and the swap, presented at `asked` when the extension is there. False when the swap failed.
static bool frame_gles(int frame, int64_t asked) {
    glClearColor((float)(frame & 1), (float)((frame >> 1) & 1), (float)((frame >> 2) & 1), 1);
    glClear(GL_COLOR_BUFFER_BIT);
    if (test.extension && gl.presentation_time(gl.display, gl.surface, asked)) ++test.times[test.rate];
    return eglSwapBuffers(gl.display, gl.surface) == EGL_TRUE;
}

// ---- Vulkan

static bool device_extension(const VkExtensionProperties *list, uint32_t count, const char *name) {
    for (uint32_t index = 0; index < count; ++index) {
        if (strcmp(list[index].extensionName, name) == 0) return true;
    }
    return false;
}

static bool start_vulkan(ANativeWindow *window) {
    const char *const instance_names[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
    const VkApplicationInfo application = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                           .pApplicationName = "present-test", .apiVersion = VK_API_VERSION_1_1};
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
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(vk.physical, NULL, &count, NULL);
    VkExtensionProperties *extensions = calloc(count, sizeof *extensions);
    vkEnumerateDeviceExtensionProperties(vk.physical, NULL, &count, extensions);
    test.extension = device_extension(extensions, count, VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME);
    free(extensions);
    LOG("VK_GOOGLE_display_timing: %s", test.extension ? "there" : "not there");
    const float priority = 1;
    const VkDeviceQueueCreateInfo queue = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                           .queueFamilyIndex = vk.family, .queueCount = 1,
                                           .pQueuePriorities = &priority};
    const char *const device_names[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME};
    const VkDeviceCreateInfo device = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
                                       .pQueueCreateInfos = &queue, .enabledExtensionCount = test.extension ? 2 : 1,
                                       .ppEnabledExtensionNames = device_names};
    if (!ok(vkCreateDevice(vk.physical, &device, NULL, &vk.device), "vkCreateDevice")) return false;
    vkGetDeviceQueue(vk.device, vk.family, 0, &vk.queue);
    if (test.extension) {
        vk.refresh_cycle =
            (PFN_vkGetRefreshCycleDurationGOOGLE)vkGetDeviceProcAddr(vk.device, "vkGetRefreshCycleDurationGOOGLE");
        vk.past_timing = (PFN_vkGetPastPresentationTimingGOOGLE)vkGetDeviceProcAddr(vk.device,
                                                                                    "vkGetPastPresentationTimingGOOGLE");
        test.extension = vk.refresh_cycle != NULL && vk.past_timing != NULL;
    }

    // An R8G8B8A8_UNORM swapchain, Android's first format, at the surface's size with FIFO, cleared by transfers.
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
    DEVICE("swapchain: %u x %u, %u images", extent.width, extent.height, vk.count);
    if (test.extension) {
        VkRefreshCycleDurationGOOGLE refresh = {0};
        const VkResult result = vk.refresh_cycle(vk.device, vk.swapchain, &refresh);
        DEVICE("vkGetRefreshCycleDurationGOOGLE: %d, %.3f ms", result, (double)refresh.refreshDuration / 1e6);
    }
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

// The past timings that came so far, each into its frame's row (present ID k is frame k - 1).
static void collect_timings(void) {
    if (!test.extension) return;
    for (;;) {
        VkPastPresentationTimingGOOGLE timings[kTimings];
        uint32_t count = kTimings;
        const VkResult result = vk.past_timing(vk.device, vk.swapchain, &count, timings);
        for (uint32_t index = 0; index < count; ++index) {
            const uint32_t id = timings[index].presentID;
            if (id >= 1 && id <= kRows) test.rows[id - 1].past = timings[index];
        }
        if (result != VK_INCOMPLETE) return;
    }
}

// A clear of the next image and its present, at `asked` when the extension is there. False when it failed.
static bool frame_vulkan(int frame, int64_t asked) {
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
    const VkPresentTimeGOOGLE time = {.presentID = (uint32_t)frame + 1, .desiredPresentTime = (uint64_t)asked};
    const VkPresentTimesInfoGOOGLE times = {.sType = VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE, .swapchainCount = 1,
                                            .pTimes = &time};
    const VkPresentInfoKHR present = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                                      .pNext = test.extension ? &times : NULL, .waitSemaphoreCount = 1,
                                      .pWaitSemaphores = &vk.rendered[index], .swapchainCount = 1,
                                      .pSwapchains = &vk.swapchain, .pImageIndices = &index};
    const VkResult presented = vkQueuePresentKHR(vk.queue, &present);
    if (presented != VK_SUCCESS && presented != VK_SUBOPTIMAL_KHR) {
        LOG("vkQueuePresentKHR: %d", presented);
        return false;
    }
    if (test.extension) ++test.times[test.rate];
    collect_timings();
    return true;
}

// ---- The frames

typedef int32_t (*set_frame_rate_function)(ANativeWindow *window, float rate, int8_t compatibility);

// ANativeWindow_setFrameRate came with API 30, above the minSdk: from libnativewindow by dlsym.
static void ask_rate(int rate) {
    static set_frame_rate_function set_frame_rate;
    static bool looked;
    if (!looked) {
        looked = true;
        void *library = dlopen("libnativewindow.so", RTLD_NOW);
        if (library != NULL) set_frame_rate = (set_frame_rate_function)dlsym(library, "ANativeWindow_setFrameRate");
    }
    if (set_frame_rate == NULL) {
        DEVICE("ANativeWindow_setFrameRate: not there");
        return;
    }
    DEVICE("ANativeWindow_setFrameRate(%d): %d", rate,
           set_frame_rate(test.app->window, (float)rate, ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_DEFAULT));
}

// AChoreographer_postVsyncCallback and its frame data came with API 33, above the minSdk: from libandroid by dlsym.
typedef void (*post_vsync_function)(AChoreographer *, AChoreographer_vsyncCallback, void *);
typedef int64_t (*frame_time_function)(const AChoreographerFrameCallbackData *);
typedef int64_t (*presentation_time_function)(const AChoreographerFrameCallbackData *, size_t);
static struct {
    post_vsync_function post;  // NULL where the three are not all there
    frame_time_function frame_time;
    presentation_time_function presentation_time;
} vsync;

static void find_vsync(void) {
    void *library = dlopen("libandroid.so", RTLD_NOW);
    if (library != NULL) {
        vsync.post = (post_vsync_function)dlsym(library, "AChoreographer_postVsyncCallback");
        vsync.frame_time = (frame_time_function)dlsym(library, "AChoreographerFrameCallbackData_getFrameTimeNanos");
        vsync.presentation_time = (presentation_time_function)dlsym(
            library, "AChoreographerFrameCallbackData_getFrameTimelineExpectedPresentationTimeNanos");
    }
    if (vsync.post == NULL || vsync.frame_time == NULL || vsync.presentation_time == NULL) {
        vsync.post = NULL;
        DEVICE("AChoreographer_postVsyncCallback: not there");
    }
}

static int compare(const void *a, const void *b) {
    const int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return x < y ? -1 : x > y;
}

// present.csv, the per-rate lines and the finish.
static void finish(void) {
    test.done = true;
    int timed = 0;
    if (test.vulkan && test.extension) {
        vkDeviceWaitIdle(vk.device);
        collect_timings();
        for (int frame = 0; frame < kRows; ++frame) timed += test.rows[frame].past.presentID != 0;
        DEVICE("past timings: %d of %d frames", timed, kRows);
    }
    for (int rate = 0; rate < kRates; ++rate) {
        LOG("rate %d: %d frames presented 1, 2 and 3 periods on, %d times taken, %d presents", rates[rate], kFrames,
            test.times[rate], test.presents[rate]);
    }
    char path[512];
    snprintf(path, sizeof path, "%s/present.csv", test.app->activity->internalDataPath);
    FILE *file = fopen(path, "w");
    if (file != NULL) {
        fprintf(file, "api,rate,interval,frame,frame_time_ns,asked_ns,timeline0_ns,present_id,desired_ns,actual_ns,"
                      "earliest_ns,margin_ns\n");
        for (int frame = 0; frame < kRows; ++frame) {
            const struct row *row = &test.rows[frame];
            fprintf(file, "%s,%d,%d,%d,%lld,%lld,%lld,%u,%llu,%llu,%llu,%llu\n", test.vulkan ? "vulkan" : "gles",
                    row->rate, row->interval, frame, (long long)row->frame_time, (long long)row->asked,
                    (long long)row->timeline0, row->past.presentID,
                    (unsigned long long)row->past.desiredPresentTime, (unsigned long long)row->past.actualPresentTime,
                    (unsigned long long)row->past.earliestPresentTime, (unsigned long long)row->past.presentMargin);
        }
        fclose(file);
    }
    LOG("present.csv: %s", file != NULL ? "written" : "not written");
    LOG("ANativeActivity_finish");
    ANativeActivity_finish(test.app->activity);
}

// One frame of the block, presented `interval` periods after the callback's frame time.
static void draw(int64_t frame_time, int64_t timeline0) {
    const int64_t asked = frame_time + test.interval * test.period;
    const int frame = test.frames++;
    test.rows[frame] = (struct row){rates[test.rate], test.interval, frame_time, asked, timeline0, {0}};
    if (test.vulkan ? frame_vulkan(frame, asked) : frame_gles(frame, asked)) ++test.presents[test.rate];
    if (++test.block_frames < kBlock) return;
    test.block_frames = 0;
    test.callbacks = 0;
    if (++test.interval <= 3) return;
    // The rate is done: the next one settles first.
    test.period = 0;
    test.previous = 0;
    if (++test.rate == kRates) {
        finish();
        return;
    }
    ask_rate(rates[test.rate]);
}

static void on_frame(int64_t frame_time, void *data);
static void on_vsync(const AChoreographerFrameCallbackData *data, void *user);

// The next callback: a vsync callback where they are there.
static void post_next(void) {
    if (vsync.post != NULL) {
        vsync.post(test.choreographer, on_vsync, NULL);
    } else {
        AChoreographer_postFrameCallback64(test.choreographer, on_frame, NULL);
    }
}

static void tick(int64_t frame_time, int64_t timeline0) {
    if (test.done || test.app->destroyRequested) {
        test.posting = false;
        return;
    }
    post_next();
    if (!test.ready) {
        test.previous = 0;
        return;
    }
    if (test.period == 0) {
        // Settling: the period is the median of the last intervals.
        if (test.previous != 0) test.intervals[test.callbacks % kIntervals] = frame_time - test.previous;
        test.previous = frame_time;
        if (++test.callbacks < kSettle) return;
        int64_t sorted[kIntervals];
        memcpy(sorted, test.intervals, sizeof sorted);
        qsort(sorted, kIntervals, sizeof sorted[0], compare);
        test.period = sorted[kIntervals / 2];
        test.interval = 1;
        test.callbacks = 0;
        DEVICE("rate %d: period %.3f ms", rates[test.rate], (double)test.period / 1e6);
        return;
    }
    if (test.callbacks++ % test.interval == 0) draw(frame_time, timeline0);
}

static void on_frame(int64_t frame_time, void *data) {
    (void)data;
    tick(frame_time, 0);
}

static void on_vsync(const AChoreographerFrameCallbackData *data, void *user) {
    (void)user;
    tick(vsync.frame_time(data), vsync.presentation_time(data, 0));
}

static void on_command(struct android_app *app, int32_t command) {
    switch (command) {
        case APP_CMD_INIT_WINDOW:
            if (test.done || test.ready) break;
            if (test.vulkan ? start_vulkan(app->window) : start_gles(app->window)) {
                test.ready = true;
                ask_rate(rates[test.rate]);
                if (!test.posting && test.choreographer != NULL) {
                    test.posting = true;
                    post_next();
                }
            } else {
                LOG("the window cannot be drawn to");
            }
            break;
        case APP_CMD_TERM_WINDOW:
            if (!test.done && test.ready) LOG("the window went at frame %d", test.frames);
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
        tag = "present-vulkan";
        device_tag = "present-vulkan-device";
    }
    ANativeActivity_onCreate(activity, saved_state, saved_state_size);
}

// present-gles's entry, which its manifest names (android.app.func_name).
__attribute__((visibility("default"))) void ANativeActivity_onCreateGLES(ANativeActivity *activity, void *saved_state,
                                                                         size_t saved_state_size) {
    start(activity, saved_state, saved_state_size, false);
}

// present-vulkan's entry.
__attribute__((visibility("default"))) void ANativeActivity_onCreateVulkan(ANativeActivity *activity, void *saved_state,
                                                                           size_t saved_state_size) {
    start(activity, saved_state, saved_state_size, true);
}

// The frames come from the choreographer's callbacks inside ALooper_pollOnce.
void android_main(struct android_app *app) {
    test.app = app;
    app->onAppCmd = on_command;
    test.choreographer = AChoreographer_getInstance();
    if (test.choreographer == NULL) LOG("no AChoreographer");
    find_vsync();
    while (!app->destroyRequested) {
        struct android_poll_source *source = NULL;
        const int result = ALooper_pollOnce(-1, NULL, NULL, (void **)&source);
        if ((result == LOOPER_ID_MAIN || result == LOOPER_ID_INPUT) && source != NULL) source->process(app, source);
    }
    if (test.vulkan) {
        stop_vulkan();
    } else {
        stop_gles();
    }
    LOG("android_main returns");
}
