// libnativewindow.so's window (ADR-0028, decision 7): the ANativeWindow that activity-host hands the app
// at NATIVE_WINDOW_CREATED and whose host window the front libs draw into. It holds the host window's
// ID, the window's format, and the buffer geometry of ANativeWindow_setBuffersGeometry, which the front
// libs read with minato_ANativeWindow_getInfo. Its size is the host window's as of now, which a turn
// changes (window_size, ADR-0038 c), and the last one it read once the host's window is gone. As on
// Android 15 (checked in the emulator on an AImageReader's window), getWidth and getHeight answer the
// window's size whatever the geometry, and getFormat the requested format once one is set. It lives until
// its last reference, also after the host's window is gone. Drawing by the CPU goes to the host's buffers
// (ADR-0043, decision 3): lock takes one of the geometry's size and format, and unlockAndPost posts it after
// the window's present gate. A window released while it is locked leaves its buffer with the host. Hardware
// buffers are generated stubs (stubgen.py).
#include <android/native_window.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "APKRuntime/guest_interface.h"

namespace {

constexpr uint32_t kCheck = 0x4d4e5749;  // "MNWI", while the window lives
constexpr int32_t kTransforms = ANATIVEWINDOW_TRANSFORM_MIRROR_HORIZONTAL | ANATIVEWINDOW_TRANSFORM_MIRROR_VERTICAL |
                                ANATIVEWINDOW_TRANSFORM_ROTATE_90;
// Android's INVALID_OPERATION (utils/Errors.h), libgui's Surface's answer to a second lock and to a post without one.
constexpr int32_t kInvalidOperation = -ENOSYS;

}  // namespace

struct ANativeWindow {
    uint32_t check;
    uint32_t references;  // atomic
    uint64_t id;
    int32_t format;
    pthread_mutex_t lock;  // for what the app or a turn sets while a render thread reads it
    int32_t width, height;  // the last size the host window had (size())
    int32_t buffer_width, buffer_height, buffer_format;
    int32_t data_space;
    bool locked;  // from ANativeWindow_lock to the end of ANativeWindow_unlockAndPost
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

struct Size {
    int32_t width, height;
};

/// The host window's size as of now, kept; the last one kept once the host's window is gone.
Size size(ANativeWindow* window) {
    int32_t now[2];
    const bool known = syscall(APKRT_NR_WINDOW_SIZE, window->id, now) == 0;
    pthread_mutex_lock(&window->lock);
    if (known) {
        window->width = now[0];
        window->height = now[1];
    }
    const Size kept{window->width, window->height};
    pthread_mutex_unlock(&window->lock);
    return kept;
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
    window->format = WINDOW_FORMAT_RGBX_8888;  // NativeActivity's window in Android 15 (activity-test)
    pthread_mutex_init(&window->lock, nullptr);
    __atomic_store_n(&window->check, kCheck, __ATOMIC_RELEASE);
    return window;
}

int minato_ANativeWindow_getInfo(ANativeWindow* window, minato_window_info* info) {
    if (!is_window(window) || info == nullptr) return EINVAL;
    const Size now = size(window);
    const Geometry geometry = buffer_geometry(window);
    *info = {window->id, now.width, now.height, geometry.width, geometry.height, geometry.format};
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
    return is_window(window) ? size(window).width : -EINVAL;
}

int32_t ANativeWindow_getHeight(ANativeWindow* window) {
    return is_window(window) ? size(window).height : -EINVAL;
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

// Accepted when Android accepts them. The rate goes to the host (ADR-0036, decision 2), whose display link
// prefers it; the compatibility and the strategy have nothing to change there.
int32_t ANativeWindow_setFrameRateWithChangeStrategy(ANativeWindow* window, float frameRate, int8_t compatibility,
                                                      int8_t changeFrameRateStrategy) {
    const bool valid = is_window(window) && frameRate >= 0 && frameRate != __builtin_inff() &&
                       (compatibility == ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_DEFAULT ||
                        compatibility == ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_FIXED_SOURCE) &&
                       (changeFrameRateStrategy == ANATIVEWINDOW_CHANGE_FRAME_RATE_ONLY_IF_SEAMLESS ||
                        changeFrameRateStrategy == ANATIVEWINDOW_CHANGE_FRAME_RATE_ALWAYS);
    if (!valid) return -EINVAL;
    uint32_t bits;
    memcpy(&bits, &frameRate, sizeof bits);
    syscall(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_FRAME_RATE, window->id, bits);
    return 0;
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

// A buffer of setBuffersGeometry's size and format, else the window's, from the host's buffers. Without a dirty
// rectangle the whole buffer is dirty, as libgui's Surface::lock has it, and the host's answer replaces the app's.
int32_t ANativeWindow_lock(ANativeWindow* window, ANativeWindow_Buffer* outBuffer, ARect* inOutDirtyBounds) {
    if (!is_window(window) || outBuffer == nullptr) return -EINVAL;
    const Size now = size(window);
    apkrt_window_buffer buffer{};
    pthread_mutex_lock(&window->lock);
    if (window->locked) {
        pthread_mutex_unlock(&window->lock);
        return kInvalidOperation;
    }
    window->locked = true;
    const bool geometry = window->buffer_width != 0;
    buffer.width = geometry ? window->buffer_width : now.width;
    buffer.height = geometry ? window->buffer_height : now.height;
    buffer.format = window->buffer_format != 0 ? window->buffer_format : window->format;
    pthread_mutex_unlock(&window->lock);
    if (inOutDirtyBounds != nullptr) {
        buffer.dirty[0] = inOutDirtyBounds->left;
        buffer.dirty[1] = inOutDirtyBounds->top;
        buffer.dirty[2] = inOutDirtyBounds->right;
        buffer.dirty[3] = inOutDirtyBounds->bottom;
    } else {
        buffer.dirty[2] = buffer.width;
        buffer.dirty[3] = buffer.height;
    }
    if (syscall(APKRT_NR_WINDOW_LOCK, window->id, &buffer) != 0) {
        const int32_t error = -errno;
        pthread_mutex_lock(&window->lock);
        window->locked = false;
        pthread_mutex_unlock(&window->lock);
        return error;
    }
    outBuffer->width = buffer.width;
    outBuffer->height = buffer.height;
    outBuffer->stride = buffer.stride;
    outBuffer->format = buffer.format;
    outBuffer->bits = buffer.bits;
    if (inOutDirtyBounds != nullptr) {
        *inOutDirtyBounds = {buffer.dirty[0], buffer.dirty[1], buffer.dirty[2], buffer.dirty[3]};
    }
    return 0;
}

// After the window's present gate, as libEGL's swap waits there (ADR-0032, decision 6), the host's buffers present
// the locked buffer. Once the window is gone the gate answers 1, and the host only takes the buffer back.
int32_t ANativeWindow_unlockAndPost(ANativeWindow* window) {
    if (!is_window(window)) return -EINVAL;
    pthread_mutex_lock(&window->lock);
    const bool locked = window->locked;
    pthread_mutex_unlock(&window->lock);
    if (!locked) return kInvalidOperation;
    long gate;
    do {
        gate = syscall(APKRT_NR_WINDOW_GATE, window->id, APKRT_GATE_PRESENT);
    } while (gate == -1 && errno == EINTR);
    const int32_t result = syscall(APKRT_NR_WINDOW_POST, window->id) == 0 ? 0 : -errno;
    pthread_mutex_lock(&window->lock);
    window->locked = false;
    pthread_mutex_unlock(&window->lock);
    return result;
}

}  // extern "C"
