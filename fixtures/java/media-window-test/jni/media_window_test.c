// ADR-0060 e2c3's test (media-window-test): the JNI library WindowActivity loads. A Vulkan swapchain on a window a
// codec draws into, and the window through the NDK's AMediaCodec: AMediaCodec_setOnFrameRenderedCallback's frames
// (whether some came, each for a released frame's time, in order), what a CPU lock of the window and a second codec's
// configure answer while the codec draws, AMediaCodec_setOutputSurface to an AImageReader's window and back, and a
// codec's configure on the window while a Vulkan surface made before it is on it.
// A line that starts with "device " goes to the device tag.
#define VK_USE_PLATFORM_ANDROID_KHR
#include <android/native_window.h>
#include <dlfcn.h>
#include <android/native_window_jni.h>
#include <jni.h>
#include <media/NdkImageReader.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <vulkan/vulkan.h>

#define NAME(name) Java_io_github_kisame76_minato_test_mediawindow_WindowActivity_##name

enum { kFrames = 64 };

static void append(char *log, size_t size, const char *line) {
    const size_t used = strlen(log);
    if (used + strlen(line) + 2 < size) {
        strcat(log, line);
        strcat(log, "\n");
    }
}

static int64_t now_ns(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000000000 + now.tv_nsec;
}

static void pause_ms(long ms) {
    struct timespec pause = {0, ms * 1000000};
    nanosleep(&pause, NULL);
}

// What vkCreateAndroidSurfaceKHR and vkCreateSwapchainKHR answer on the window; the swapchain's answer is
// VK_ERROR_UNKNOWN (-13) when the surface was refused and no swapchain was tried.
static void swapchain(ANativeWindow *window, char *log, size_t size) {
    const char *const extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
    const VkApplicationInfo application = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                           .apiVersion = VK_API_VERSION_1_1};
    const VkInstanceCreateInfo instance_info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                                .pApplicationInfo = &application,
                                                .enabledExtensionCount = 2,
                                                .ppEnabledExtensionNames = extensions};
    VkInstance instance = VK_NULL_HANDLE;
    char line[200];
    if (vkCreateInstance(&instance_info, NULL, &instance) != VK_SUCCESS) {
        append(log, size, "a Vulkan swapchain: no instance");
        return;
    }
    uint32_t count = 1;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    vkEnumeratePhysicalDevices(instance, &count, &physical);
    const VkAndroidSurfaceCreateInfoKHR surface_info = {.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR,
                                                        .window = window};
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    const VkResult made = vkCreateAndroidSurfaceKHR(instance, &surface_info, NULL, &surface);
    VkResult chain = VK_ERROR_UNKNOWN;
    VkDevice device = VK_NULL_HANDLE;
    if (made == VK_SUCCESS && count > 0) {
        const float priority = 1.0f;
        const VkDeviceQueueCreateInfo queue = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                               .queueFamilyIndex = 0,
                                               .queueCount = 1,
                                               .pQueuePriorities = &priority};
        const char *const device_extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        const VkDeviceCreateInfo device_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                                .queueCreateInfoCount = 1,
                                                .pQueueCreateInfos = &queue,
                                                .enabledExtensionCount = 1,
                                                .ppEnabledExtensionNames = device_extensions};
        if (vkCreateDevice(physical, &device_info, NULL, &device) == VK_SUCCESS) {
            VkSurfaceCapabilitiesKHR capabilities;
            vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &capabilities);
            const VkSwapchainCreateInfoKHR info = {
                .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
                .surface = surface,
                .minImageCount = capabilities.minImageCount,
                .imageFormat = VK_FORMAT_R8G8B8A8_UNORM,
                .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
                .imageExtent = capabilities.currentExtent.width != UINT32_MAX ? capabilities.currentExtent
                                                                              : (VkExtent2D){320, 180},
                .imageArrayLayers = 1,
                .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                .preTransform = capabilities.currentTransform,
                .compositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
                .presentMode = VK_PRESENT_MODE_FIFO_KHR,
                .clipped = VK_TRUE};
            VkSwapchainKHR made_chain = VK_NULL_HANDLE;
            chain = vkCreateSwapchainKHR(device, &info, NULL, &made_chain);
            if (chain == VK_SUCCESS) vkDestroySwapchainKHR(device, made_chain, NULL);
            vkDestroyDevice(device, NULL);
        }
    }
    snprintf(line, sizeof line, "a Vulkan swapchain: surface %d, swapchain %d", made, chain);
    append(log, size, line);
    if (surface != VK_NULL_HANDLE) vkDestroySurfaceKHR(instance, surface, NULL);
    vkDestroyInstance(instance, NULL);
}

static int feed(AMediaCodec *codec, const jbyte *bytes, const jlong *units, jsize count, jsize *next) {
    const ssize_t in = AMediaCodec_dequeueInputBuffer(codec, 10000);
    if (in < 0) return 0;
    size_t capacity = 0;
    uint8_t *input = AMediaCodec_getInputBuffer(codec, (size_t)in, &capacity);
    if (*next < count && input != NULL && (size_t)units[*next * 4 + 1] <= capacity) {
        memcpy(input, bytes + units[*next * 4], (size_t)units[*next * 4 + 1]);
        AMediaCodec_queueInputBuffer(codec, (size_t)in, 0, (size_t)units[*next * 4 + 1],
                                     (uint64_t)units[*next * 4 + 2], 0);
        (*next)++;
        return 0;
    }
    AMediaCodec_queueInputBuffer(codec, (size_t)in, 0, 0, 0, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
    return 1;
}

// The frames AMediaCodec_setOnFrameRenderedCallback reported: each frame's time and the time it was rendered.
struct rendered {
    pthread_mutex_t lock;
    int count;
    int64_t pts[kFrames], at[kFrames];
};

static void frame_rendered(AMediaCodec *codec, void *data, int64_t pts, int64_t at) {
    (void)codec;
    struct rendered *rendered = data;
    pthread_mutex_lock(&rendered->lock);
    if (rendered->count < kFrames) {
        rendered->pts[rendered->count] = pts;
        rendered->at[rendered->count] = at;
        rendered->count++;
    }
    pthread_mutex_unlock(&rendered->lock);
}

// AMediaCodec_setOnFrameRenderedCallback, API 33's, above the library's API 29: looked up, NULL without it.
typedef media_status_t (*set_rendered_callback)(AMediaCodec *, void (*)(AMediaCodec *, void *, int64_t, int64_t),
                                                void *);

static AMediaCodec *configured(const char *decoder, ANativeWindow *window, media_status_t *configure) {
    AMediaCodec *codec = AMediaCodec_createCodecByName(decoder);
    if (codec == NULL) return NULL;
    AMediaFormat *format = AMediaFormat_new();
    AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, "video/avc");
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, 320);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, 180);
    *configure = AMediaCodec_configure(codec, format, window, NULL, 0);
    AMediaFormat_delete(format);
    return codec;
}

// The window through AMediaCodec: every frame rendered, one every 34 ms, a CPU lock and a second codec after the
// first frame, the switch to a reader's window for frames 10 to 19 and back.
static void through_ndk(const char *decoder, const jbyte *bytes, const jlong *units, jsize count,
                        ANativeWindow *window, char *log, size_t size) {
    char line[300];
    media_status_t configure = AMEDIA_ERROR_UNKNOWN;
    AMediaCodec *codec = configured(decoder, window, &configure);
    if (codec == NULL) {
        append(log, size, "ndk: no codec");
        return;
    }
    struct rendered rendered = {.lock = PTHREAD_MUTEX_INITIALIZER};
    const set_rendered_callback set_callback =
        (set_rendered_callback)dlsym(RTLD_DEFAULT, "AMediaCodec_setOnFrameRenderedCallback");
    const media_status_t callback =
        set_callback != NULL ? set_callback(codec, frame_rendered, &rendered) : AMEDIA_ERROR_UNSUPPORTED;
    const media_status_t start = configure == AMEDIA_OK ? AMediaCodec_start(codec) : AMEDIA_ERROR_INVALID_OPERATION;
    snprintf(line, sizeof line, "ndk: configure %d, frame rendered callback %d, start %d", configure, callback, start);
    append(log, size, line);
    AImageReader *reader = NULL;
    ANativeWindow *reader_window = NULL;
    AImageReader_new(320, 180, AIMAGE_FORMAT_YUV_420_888, 3, &reader);
    if (reader != NULL) AImageReader_getWindow(reader, &reader_window);
    int64_t released_pts[kFrames];
    int released = 0, frames = 0, ended = 0, images = 0;
    jsize next = 0;
    const int64_t deadline = now_ns() + 15000000000LL;
    while (!ended && now_ns() < deadline) {
        if (!ended) feed(codec, bytes, units, count, &next);
        AMediaCodecBufferInfo info;
        const ssize_t out = AMediaCodec_dequeueOutputBuffer(codec, &info, 10000);
        if (out < 0) continue;
        if ((info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0 && info.size == 0) {
            AMediaCodec_releaseOutputBuffer(codec, (size_t)out, false);
            ended = 1;
            break;
        }
        if (frames == 10 || frames == 20) {
            const media_status_t switched =
                AMediaCodec_setOutputSurface(codec, frames == 10 ? reader_window : window);
            snprintf(line, sizeof line, "ndk: setOutputSurface to the %s %d", frames == 10 ? "reader" : "window",
                     switched);
            append(log, size, line);
        }
        const int to_reader = frames >= 10 && frames < 20;
        if (released < kFrames) {
            released_pts[released] = info.presentationTimeUs;
            released++;
        }
        AMediaCodec_releaseOutputBuffer(codec, (size_t)out, true);
        if (to_reader) {
            AImage *image = NULL;
            for (int64_t wait = now_ns() + 2000000000LL; image == NULL && now_ns() < wait;) {
                if (AImageReader_acquireNextImage(reader, &image) != AMEDIA_OK) pause_ms(2);
            }
            int64_t time = -1;
            if (image != NULL) {
                AImage_getTimestamp(image, &time);
                AImage_delete(image);
            }
            if (image != NULL) images++;
            snprintf(line, sizeof line, "device ndk: the reader's image %s, pts %lld",
                     image != NULL ? "came" : "none", (long long)(time / 1000));
            append(log, size, line);
        } else {
            pause_ms(34);
        }
        if (frames == 0) {
            ANativeWindow_Buffer buffer;
            const int32_t locked = ANativeWindow_lock(window, &buffer, NULL);
            if (locked == 0) ANativeWindow_unlockAndPost(window);
            media_status_t second_configure = AMEDIA_ERROR_UNKNOWN;
            AMediaCodec *second = configured(decoder, window, &second_configure);
            snprintf(line, sizeof line, "ndk: while the codec draws, a CPU lock %d, a second codec's configure %d",
                     locked, second_configure);
            append(log, size, line);
            if (second != NULL) AMediaCodec_delete(second);
        }
        frames++;
    }
    pause_ms(500);
    pthread_mutex_lock(&rendered.lock);
    int known = 1, ordered = 1, stamped = 1;
    int64_t last = -1;
    for (int index = 0; index < rendered.count; index++) {
        int found = 0;
        for (int r = 0; r < released; r++) {
            found |= released_pts[r] == rendered.pts[index];
        }
        known &= found;
        ordered &= rendered.pts[index] > last;
        stamped &= rendered.at[index] == rendered.pts[index] * 1000;
        last = rendered.pts[index];
    }
    snprintf(line, sizeof line,
             "ndk: %d frames rendered, the reader's images some %d, callbacks: some %d, each a rendered frame's %d, "
             "in order %d, its time the frame's %d",
             frames, images > 0, rendered.count > 0, known, ordered, stamped);
    append(log, size, line);
    snprintf(line, sizeof line, "device ndk: %d callbacks", rendered.count);
    append(log, size, line);
    pthread_mutex_unlock(&rendered.lock);
    AMediaCodec_stop(codec);
    AMediaCodec_delete(codec);
    if (reader != NULL) AImageReader_delete(reader);
}

JNIEXPORT jstring JNICALL NAME(swapchainNative)(JNIEnv *env, jclass type, jobject surface) {
    (void)type;
    char log[1024] = {0};
    ANativeWindow *window = ANativeWindow_fromSurface(env, surface);
    swapchain(window, log, sizeof log);
    ANativeWindow_release(window);
    log[strcspn(log, "\n")] = '\0';
    return (*env)->NewStringUTF(env, log);
}

// What a new codec's configure on the window answers (a media_status_t), the codec deleted after it.
static int configure_answer(const char *decoder, ANativeWindow *window) {
    media_status_t status = AMEDIA_ERROR_UNKNOWN;
    AMediaCodec *codec = configured(decoder, window, &status);
    if (codec == NULL) return -1;
    AMediaCodec_delete(codec);
    return status;
}

JNIEXPORT jstring JNICALL NAME(beforeNative)(JNIEnv *env, jclass type, jstring decoder, jobject surface) {
    (void)type;
    char log[512] = {0};
    const char *chosen = decoder != NULL ? (*env)->GetStringUTFChars(env, decoder, NULL) : NULL;
    ANativeWindow *window = ANativeWindow_fromSurface(env, surface);
    const char *const extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
    const VkApplicationInfo application = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                           .apiVersion = VK_API_VERSION_1_1};
    const VkInstanceCreateInfo instance_info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                                .pApplicationInfo = &application,
                                                .enabledExtensionCount = 2,
                                                .ppEnabledExtensionNames = extensions};
    VkInstance instance = VK_NULL_HANDLE;
    if (chosen == NULL || window == NULL || vkCreateInstance(&instance_info, NULL, &instance) != VK_SUCCESS) {
        append(log, sizeof log, "before: no decoder, window or Vulkan instance");
    } else {
        const VkAndroidSurfaceCreateInfoKHR surface_info = {
            .sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR, .window = window};
        VkSurfaceKHR made = VK_NULL_HANDLE;
        const VkResult result = vkCreateAndroidSurfaceKHR(instance, &surface_info, NULL, &made);
        const int with = configure_answer(chosen, window);
        if (made != VK_NULL_HANDLE) vkDestroySurfaceKHR(instance, made, NULL);
        const int after = configure_answer(chosen, window);
        char line[200];
        snprintf(line, sizeof line, "before: a Vulkan surface %d, a codec's configure with it %d, after its destroy %d",
                 result, with, after);
        append(log, sizeof log, line);
        vkDestroyInstance(instance, NULL);
    }
    if (window != NULL) ANativeWindow_release(window);
    if (chosen != NULL) (*env)->ReleaseStringUTFChars(env, decoder, chosen);
    return (*env)->NewStringUTF(env, log);
}

JNIEXPORT jstring JNICALL NAME(ndkNative)(JNIEnv *env, jclass type, jstring decoder, jbyteArray stream,
                                          jlongArray index, jobject surface) {
    (void)type;
    char log[8192] = {0};
    const char *chosen = decoder != NULL ? (*env)->GetStringUTFChars(env, decoder, NULL) : NULL;
    jbyte *bytes = (*env)->GetByteArrayElements(env, stream, NULL);
    jlong *units = (*env)->GetLongArrayElements(env, index, NULL);
    ANativeWindow *window = ANativeWindow_fromSurface(env, surface);
    if (chosen != NULL && window != NULL) {
        through_ndk(chosen, bytes, units, (*env)->GetArrayLength(env, index) / 4, window, log, sizeof log);
    } else {
        append(log, sizeof log, "ndk: no decoder or window");
    }
    if (window != NULL) ANativeWindow_release(window);
    (*env)->ReleaseLongArrayElements(env, index, units, JNI_ABORT);
    (*env)->ReleaseByteArrayElements(env, stream, bytes, JNI_ABORT);
    if (chosen != NULL) (*env)->ReleaseStringUTFChars(env, decoder, chosen);
    return (*env)->NewStringUTF(env, log);
}
