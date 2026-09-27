// libnativewindow.so's window (ADR-0028, decision 7): the ANativeWindow that activity-host hands the app
// at NATIVE_WINDOW_CREATED and whose host window the front libs draw into. It holds the host window's
// ID, the window's size and format, and the buffer geometry of ANativeWindow_setBuffersGeometry, which
// the front libs read with minato_ANativeWindow_getInfo. As on Android 15 (checked in the emulator on
// an AImageReader's window), getWidth and getHeight answer the window's size whatever the geometry, and
// getFormat the requested format once one is set. It lives until its last reference, also after the
// host's window is gone. Drawing by the CPU and hardware buffers are generated stubs (stubgen.py).
#include <android/native_window.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>

#include "APKRuntime/guest_interface.h"

namespace {

constexpr uint32_t kCheck = 0x4d4e5749;  // "MNWI", while the window lives
constexpr int32_t kTransforms = ANATIVEWINDOW_TRANSFORM_MIRROR_HORIZONTAL | ANATIVEWINDOW_TRANSFORM_MIRROR_VERTICAL |
                                ANATIVEWINDOW_TRANSFORM_ROTATE_90;

}  // namespace

struct ANativeWindow {
    uint32_t check;
    uint32_t references;  // atomic
    uint64_t id;
    int32_t width, height, format;
    pthread_mutex_t lock;  // for what the app sets while a render thread reads it
    int32_t buffer_width, buffer_height, buffer_format;
    int32_t data_space;
};

namespace {

bool is_window(const ANativeWindow* window) {
    return window != nullptr && __atomic_load_n(&window->check, __ATOMIC_RELAXED) == kCheck;
}

struct Geometry {
    int32_t width, height, format;
};

Geometry buffer_geometry(ANativeWindow* window) {
    pthread_mutex_lock(&window->lock);
    const Geometry geometry{window->buffer_width, window->buffer_height, window->buffer_format};
    pthread_mutex_unlock(&window->lock);
    return geometry;
}

}  // namespace

extern "C" {

ANativeWindow* minato_ANativeWindow_create(uint64_t id, int32_t width, int32_t height) {
    auto* window = static_cast<ANativeWindow*>(calloc(1, sizeof(ANativeWindow)));
    if (window == nullptr) return nullptr;
    window->references = 1;
    window->id = id;
    window->width = width;
    window->height = height;
    window->format = WINDOW_FORMAT_RGB_565;  // NativeActivity's window
    pthread_mutex_init(&window->lock, nullptr);
    __atomic_store_n(&window->check, kCheck, __ATOMIC_RELEASE);
    return window;
}

int minato_ANativeWindow_getInfo(ANativeWindow* window, minato_window_info* info) {
    if (!is_window(window) || info == nullptr) return EINVAL;
    const Geometry geometry = buffer_geometry(window);
    *info = {window->id, window->width, window->height, geometry.width, geometry.height, geometry.format};
    return 0;
}

void ANativeWindow_acquire(ANativeWindow* window) {
    if (is_window(window)) __atomic_add_fetch(&window->references, 1, __ATOMIC_RELAXED);
}

void ANativeWindow_release(ANativeWindow* window) {
    if (!is_window(window) || __atomic_sub_fetch(&window->references, 1, __ATOMIC_ACQ_REL) != 0) return;
    __atomic_store_n(&window->check, 0, __ATOMIC_RELAXED);
    pthread_mutex_destroy(&window->lock);
    free(window);
}

int32_t ANativeWindow_getWidth(ANativeWindow* window) {
    return is_window(window) ? window->width : -EINVAL;
}

int32_t ANativeWindow_getHeight(ANativeWindow* window) {
    return is_window(window) ? window->height : -EINVAL;
}

int32_t ANativeWindow_getFormat(ANativeWindow* window) {
    if (!is_window(window)) return -EINVAL;
    const Geometry geometry = buffer_geometry(window);
    return geometry.format != 0 ? geometry.format : window->format;
}

// Format 0 is the window's own. Width and height are both 0, the window's own size, or both set; as on
// Android, the format is taken even when the size is refused, and any other size is taken as it is.
int32_t ANativeWindow_setBuffersGeometry(ANativeWindow* window, int32_t width, int32_t height, int32_t format) {
    if (!is_window(window)) return -EINVAL;
    pthread_mutex_lock(&window->lock);
    window->buffer_format = format;
    const bool size_taken = (width == 0) == (height == 0);
    if (size_taken) {
        window->buffer_width = width;
        window->buffer_height = height;
    }
    pthread_mutex_unlock(&window->lock);
    return size_taken ? 0 : -EINVAL;
}

// Accepted when Android accepts them, and they change nothing: the frame rate is the host's
// (CADisplayLink), the transform and the data space are the front libs' business.
int32_t ANativeWindow_setFrameRateWithChangeStrategy(ANativeWindow* window, float frameRate, int8_t compatibility,
                                                      int8_t changeFrameRateStrategy) {
    const bool valid = is_window(window) && frameRate >= 0 && frameRate != __builtin_inff() &&
                       (compatibility == ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_DEFAULT ||
                        compatibility == ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_FIXED_SOURCE) &&
                       (changeFrameRateStrategy == ANATIVEWINDOW_CHANGE_FRAME_RATE_ONLY_IF_SEAMLESS ||
                        changeFrameRateStrategy == ANATIVEWINDOW_CHANGE_FRAME_RATE_ALWAYS);
    return valid ? 0 : -EINVAL;
}

int32_t ANativeWindow_setFrameRate(ANativeWindow* window, float frameRate, int8_t compatibility) {
    return ANativeWindow_setFrameRateWithChangeStrategy(window, frameRate, compatibility,
                                                        ANATIVEWINDOW_CHANGE_FRAME_RATE_ONLY_IF_SEAMLESS);
}

int32_t ANativeWindow_setBuffersTransform(ANativeWindow* window, int32_t transform) {
    return is_window(window) && (transform & ~kTransforms) == 0 ? 0 : -EINVAL;
}

// Kept only to be answered.
int32_t ANativeWindow_setBuffersDataSpace(ANativeWindow* window, int32_t dataSpace) {
    if (!is_window(window)) return -EINVAL;
    pthread_mutex_lock(&window->lock);
    window->data_space = dataSpace;
    pthread_mutex_unlock(&window->lock);
    return 0;
}

int32_t ANativeWindow_getBuffersDataSpace(ANativeWindow* window) {
    if (!is_window(window)) return -EINVAL;
    pthread_mutex_lock(&window->lock);
    const int32_t data_space = window->data_space;
    pthread_mutex_unlock(&window->lock);
    return data_space;
}

int32_t ANativeWindow_getBuffersDefaultDataSpace(ANativeWindow* window) {
    return is_window(window) ? ADATASPACE_UNKNOWN : -EINVAL;
}

void ANativeWindow_tryAllocateBuffers(ANativeWindow*) {}

}  // extern "C"
