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
// buffers are hardware_buffer.cpp's (ADR-0058 a)).
//
// A queue window (ADR-0058 c), APKRuntime/buffer_queue.h) is the producer of a buffer queue, a SurfaceTexture's, and
// has no host window: its size is the queue's default size, a lock dequeues a slot's AHardwareBuffer and locks it for
// the CPU, and unlockAndPost unlocks and queues it. Abandoned, its lock answers -ENODEV, libgui's NO_INIT.
// An ImageWriter connects to it as its producer (the graphics breadth's G3b): then the queue drops no frame, has the
// writer's slots next to the consumer's, and calls the writer back at each release of one of its frames.
#include <android/hardware_buffer.h>
#include <android/native_window.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "APKRuntime/buffer_queue.h"
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
    bool claimed;  // a host window's decoder (minato_ANativeWindow_claim, ADR-0060 e2c3); a queue's is its queue's
    minato_BufferQueue* queue;  // a queue window's, else nullptr
    int32_t locked_slot;        // a queue window's slot from ANativeWindow_lock to its post
    int32_t transform;          // a queue window's ANativeWindow_setBuffersTransform, for its frames
};

namespace {

constexpr int kSlots = 3;      // a SurfaceTexture's queue
constexpr int kMaxSlots = MINATO_BUFFER_QUEUE_MAX_SLOTS;  // for an ImageReader's and an ImageWriter's slots
constexpr int32_t kImplementationDefined = 0x22;  // HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED
constexpr uint64_t kSlotUsage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER |
                                AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN;

enum class SlotState { kFree, kDequeued, kQueued, kAcquired };

struct Slot {
    AHardwareBuffer* buffer;
    int32_t width, height, format;
    SlotState state;
    int64_t timestamp;
    int32_t transform;
    int32_t data_space;
    int32_t crop[4];  // left, top, right, bottom; all 0 for none
    uint64_t order;   // of its queue, for first in, first out
    bool attached;    // a buffer the producer attached, which leaves the slot at its release
};

}  // namespace

struct minato_BufferQueue {
    pthread_mutex_t lock;
    pthread_cond_t freed;  // a slot became free, or the queue was abandoned
    uint32_t references;   // the window's and the consumer's, under the lock
    bool abandoned;
    int32_t default_width, default_height;
    Slot slots[kMaxSlots];
    int32_t slot_count;  // kSlots, or an ImageReader's maxAcquired + 2
    uint64_t next_order;
    int32_t acquired;      // a SurfaceTexture's slot, or -1
    int32_t max_acquired;  // an ImageReader's maxImages (setConsumer), else 0
    int32_t acquired_count;
    uint64_t usage;          // of the buffers
    int32_t default_format;  // setDefaultFormat's, else 0
    int32_t default_data_space;  // setDefaultDataSpace's, else 0
    bool connected;          // an ImageWriter's producer (minato_ANativeWindow_connect)
    bool claimed;            // a decoder's (minato_ANativeWindow_claim)
    int32_t unconnected_slot_count;  // the slot count before the connect, again after the disconnect
    pthread_mutex_t callback_lock;  // held around each call of `released`, so that a disconnect waits for it
    minato_BufferQueue_released released;
    void* released_context;
    minato_BufferQueue_available available;
    minato_BufferQueue_destroy destroy;
    void* context;
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

/// The host window's size as of now, kept; the last one kept once the host's window is gone. A queue window's is its
/// queue's default size.
Size size(ANativeWindow* window) {
    if (window->queue != nullptr) {
        pthread_mutex_lock(&window->queue->lock);
        const Size kept{window->queue->default_width, window->queue->default_height};
        pthread_mutex_unlock(&window->queue->lock);
        return kept;
    }
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

/// Drops a reference of the queue; the last frees its buffers, calls destroy and frees the queue. A producer in a call
/// holds the window's reference, so no call of `available` runs after destroy.
void unref(minato_BufferQueue* queue) {
    pthread_mutex_lock(&queue->lock);
    const bool last = --queue->references == 0;
    pthread_mutex_unlock(&queue->lock);
    if (!last) return;
    for (Slot& slot : queue->slots) {
        if (slot.buffer != nullptr) AHardwareBuffer_release(slot.buffer);
    }
    if (queue->destroy != nullptr) queue->destroy(queue->context);
    pthread_mutex_destroy(&queue->callback_lock);
    pthread_cond_destroy(&queue->freed);
    pthread_mutex_destroy(&queue->lock);
    free(queue);
}

int64_t monotonic_now() {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec * INT64_C(1000000000) + now.tv_nsec;
}

/// A free slot with a buffer of `width` × `height` in `format`, dequeued: waits for one, takes one whose buffer fits
/// first, and makes the buffer outside the lock. 0, -ENODEV once abandoned, -ENOMEM.
int dequeue(minato_BufferQueue* queue, int32_t width, int32_t height, int32_t format, int32_t* slot_index) {
    pthread_mutex_lock(&queue->lock);
    int found = -1;
    while (!queue->abandoned) {
        for (int index = 0; index < queue->slot_count; index++) {
            const Slot& slot = queue->slots[index];
            if (slot.state != SlotState::kFree) continue;
            const bool fits = slot.buffer != nullptr && slot.width == width && slot.height == height &&
                              slot.format == format;
            if (found < 0 || fits) found = index;
            if (fits) break;
        }
        if (found >= 0) break;
        pthread_cond_wait(&queue->freed, &queue->lock);
    }
    if (queue->abandoned) {
        pthread_mutex_unlock(&queue->lock);
        return -ENODEV;
    }
    Slot& slot = queue->slots[found];
    slot.state = SlotState::kDequeued;
    AHardwareBuffer* stale = nullptr;
    if (slot.buffer != nullptr && (slot.width != width || slot.height != height || slot.format != format)) {
        stale = slot.buffer;
        slot.buffer = nullptr;
        slot.attached = false;
    }
    const bool make = slot.buffer == nullptr;
    pthread_mutex_unlock(&queue->lock);
    if (stale != nullptr) AHardwareBuffer_release(stale);
    if (make) {
        AHardwareBuffer_Desc description{};
        description.width = static_cast<uint32_t>(width);
        description.height = static_cast<uint32_t>(height);
        description.layers = 1;
        // IMPLEMENTATION_DEFINED, a PRIVATE ImageReader's format, is RGBA_8888 in memory.
        const int32_t memory = format == kImplementationDefined ? AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM : format;
        description.format = static_cast<uint32_t>(memory);
        description.usage = queue->usage;
        AHardwareBuffer* made = nullptr;
        const int result = AHardwareBuffer_allocate(&description, &made);
        pthread_mutex_lock(&queue->lock);
        if (result != 0) {
            slot.state = SlotState::kFree;
            pthread_cond_signal(&queue->freed);
            pthread_mutex_unlock(&queue->lock);
            return -ENOMEM;
        }
        slot.buffer = made;
        slot.width = width;
        slot.height = height;
        slot.format = format;
        pthread_mutex_unlock(&queue->lock);
    }
    *slot_index = found;
    return 0;
}

/// Queues a dequeued slot and tells the consumer; a slot of an abandoned queue only becomes free. 0, -ENODEV, or
/// -EINVAL for a slot that is not dequeued (libgui's BAD_VALUE for a slot the producer does not own).
int queue_slot(minato_BufferQueue* queue, int32_t index, const minato_BufferQueue_settings& settings) {
    pthread_mutex_lock(&queue->lock);
    Slot& slot = queue->slots[index];
    if (slot.state != SlotState::kDequeued) {
        pthread_mutex_unlock(&queue->lock);
        return -EINVAL;
    }
    if (queue->abandoned) {
        slot.state = SlotState::kFree;
        slot.attached = false;
        pthread_mutex_unlock(&queue->lock);
        return -ENODEV;
    }
    // A frame still queued is replaced and dropped, as an app's SurfaceTexture fed by an app's producer has it on
    // Android 15 (mQueueBufferCanDrop, surface-texture-test): only the first frame of a run calls `available`. An
    // ImageWriter's frames are all kept, as Android 15's (image-writer-test).
    bool replaced = false;
    for (int other = 0; other < queue->slot_count && !queue->connected; other++) {
        Slot& queued = queue->slots[other];
        if (queued.state == SlotState::kQueued) {
            queued.state = SlotState::kFree;
            queued.attached = false;  // a writer's, queued before its disconnect: the buffer stays the slot's
            replaced = true;
            pthread_cond_signal(&queue->freed);
        }
    }
    slot.state = SlotState::kQueued;
    slot.timestamp = settings.timestamp != 0 || settings.exact_time != 0 ? settings.timestamp : monotonic_now();
    slot.transform = settings.transform;
    slot.data_space = settings.data_space;
    slot.crop[0] = settings.crop_left;
    slot.crop[1] = settings.crop_top;
    slot.crop[2] = settings.crop_right;
    slot.crop[3] = settings.crop_bottom;
    slot.order = queue->next_order++;
    const minato_BufferQueue_available available = replaced ? nullptr : queue->available;
    void* const context = queue->context;
    pthread_mutex_unlock(&queue->lock);
    if (available != nullptr) available(context);
    return 0;
}

/// `count` slots, or more while a slot beyond them is still in use, so that it stays visible; under the lock.
int32_t slots_in_use_from(const minato_BufferQueue* queue, int32_t count) {
    for (int index = count; index < kMaxSlots; index++) {
        if (queue->slots[index].state != SlotState::kFree) count = index + 1;
    }
    return count;
}

/// Calls the connected producer's `released`, outside the queue's lock and under its callback lock.
void notify_released(minato_BufferQueue* queue) {
    pthread_mutex_lock(&queue->callback_lock);
    pthread_mutex_lock(&queue->lock);
    const minato_BufferQueue_released released = queue->released;
    void* const context = queue->released_context;
    pthread_mutex_unlock(&queue->lock);
    if (released != nullptr) released(context);
    pthread_mutex_unlock(&queue->callback_lock);
}

/// A slot back to free from the consumer, under the lock: an attached buffer leaves it, returned to be released
/// outside the lock. Whether a producer is to hear of it.
bool free_released(minato_BufferQueue* queue, Slot& slot, AHardwareBuffer** detached) {
    slot.state = SlotState::kFree;
    if (slot.attached) {
        *detached = slot.buffer;
        slot.buffer = nullptr;
        slot.attached = false;
    }
    pthread_cond_signal(&queue->freed);
    return queue->connected;
}

/// The window's format without setBuffersGeometry's: its queue's default format, else its own.
int32_t own_format(ANativeWindow* window) {
    if (window->queue != nullptr) {
        pthread_mutex_lock(&window->queue->lock);
        const int32_t format = window->queue->default_format;
        pthread_mutex_unlock(&window->queue->lock);
        if (format != 0) return format;
    }
    return window->format;
}

/// The window's buffer size and format for a dequeue: its geometry's, else its queue's default size and its format.
Geometry dequeue_geometry(ANativeWindow* window) {
    const Size now = size(window);
    const Geometry geometry = buffer_geometry(window);
    return {geometry.width != 0 ? geometry.width : now.width, geometry.width != 0 ? geometry.height : now.height,
            geometry.format != 0 ? geometry.format : own_format(window)};
}

}  // namespace

extern "C" {

int minato_BufferQueue_create(minato_BufferQueue_available available, minato_BufferQueue_destroy destroy,
                              void* context, ANativeWindow** producer, minato_BufferQueue** consumer) {
    if (producer == nullptr || consumer == nullptr) return -EINVAL;
    auto* queue = static_cast<minato_BufferQueue*>(calloc(1, sizeof(minato_BufferQueue)));
    auto* window = static_cast<ANativeWindow*>(calloc(1, sizeof(ANativeWindow)));
    if (queue == nullptr || window == nullptr) {
        free(queue);
        free(window);
        return -ENOMEM;
    }
    pthread_mutex_init(&queue->lock, nullptr);
    pthread_cond_init(&queue->freed, nullptr);
    pthread_mutex_init(&queue->callback_lock, nullptr);
    queue->references = 2;
    queue->default_width = 1;  // BufferQueue's default buffer size
    queue->default_height = 1;
    queue->acquired = -1;
    queue->slot_count = kSlots;
    queue->usage = kSlotUsage;
    queue->available = available;
    queue->destroy = destroy;
    queue->context = context;
    window->references = 1;
    window->format = WINDOW_FORMAT_RGBA_8888;  // a SurfaceTexture's default, PIXEL_FORMAT_RGBA_8888
    window->queue = queue;
    window->locked_slot = -1;
    pthread_mutex_init(&window->lock, nullptr);
    __atomic_store_n(&window->check, kCheck, __ATOMIC_RELEASE);
    *producer = window;
    *consumer = queue;
    return 0;
}

void minato_BufferQueue_setDefaultSize(minato_BufferQueue* queue, int32_t width, int32_t height) {
    if (queue == nullptr || width <= 0 || height <= 0) return;
    pthread_mutex_lock(&queue->lock);
    queue->default_width = width;
    queue->default_height = height;
    pthread_mutex_unlock(&queue->lock);
}

int minato_BufferQueue_acquire(minato_BufferQueue* queue, minato_BufferQueue_frame* frame) {
    if (queue == nullptr || frame == nullptr) return -EINVAL;
    pthread_mutex_lock(&queue->lock);
    if (queue->abandoned) {
        pthread_mutex_unlock(&queue->lock);
        return -ENODEV;
    }
    if (queue->max_acquired > 0 && queue->acquired_count >= queue->max_acquired) {
        pthread_mutex_unlock(&queue->lock);
        return -EBUSY;
    }
    int oldest = -1;
    for (int index = 0; index < queue->slot_count; index++) {
        const Slot& slot = queue->slots[index];
        if (slot.state == SlotState::kQueued && (oldest < 0 || slot.order < queue->slots[oldest].order)) {
            oldest = index;
        }
    }
    if (oldest < 0) {
        pthread_mutex_unlock(&queue->lock);
        return -EAGAIN;
    }
    bool notify = false;
    AHardwareBuffer* detached = nullptr;
    if (queue->max_acquired > 0) {
        queue->acquired_count++;
    } else {
        if (queue->acquired >= 0) notify = free_released(queue, queue->slots[queue->acquired], &detached);
        queue->acquired = oldest;
    }
    Slot& slot = queue->slots[oldest];
    slot.state = SlotState::kAcquired;
    *frame = {oldest, slot.buffer, slot.timestamp, slot.transform, slot.data_space,
              slot.crop[0], slot.crop[1], slot.crop[2], slot.crop[3]};
    pthread_mutex_unlock(&queue->lock);
    if (detached != nullptr) AHardwareBuffer_release(detached);
    if (notify) notify_released(queue);
    return 0;
}

void minato_BufferQueue_releaseSlot(minato_BufferQueue* queue, int32_t slot) {
    if (queue == nullptr || slot < 0 || slot >= kMaxSlots) return;
    bool notify = false;
    AHardwareBuffer* detached = nullptr;
    pthread_mutex_lock(&queue->lock);
    if (queue->slots[slot].state == SlotState::kAcquired) {
        notify = free_released(queue, queue->slots[slot], &detached);
        if (queue->max_acquired > 0) {
            queue->acquired_count--;
        } else {
            queue->acquired = -1;
        }
    }
    pthread_mutex_unlock(&queue->lock);
    if (detached != nullptr) AHardwareBuffer_release(detached);
    if (notify) notify_released(queue);
}

int minato_BufferQueue_detach(minato_BufferQueue* queue, int32_t slot) {
    if (queue == nullptr || slot < 0 || slot >= kMaxSlots) return -EINVAL;
    pthread_mutex_lock(&queue->lock);
    Slot& detached = queue->slots[slot];
    if (detached.state != SlotState::kAcquired || queue->max_acquired == 0) {
        pthread_mutex_unlock(&queue->lock);
        return -EINVAL;
    }
    AHardwareBuffer* buffer = detached.buffer;
    detached.buffer = nullptr;
    detached.attached = false;
    detached.state = SlotState::kFree;
    queue->acquired_count--;
    pthread_cond_signal(&queue->freed);
    pthread_mutex_unlock(&queue->lock);
    if (buffer != nullptr) AHardwareBuffer_release(buffer);
    return 0;
}

int minato_BufferQueue_setConsumer(minato_BufferQueue* queue, int32_t maxAcquired, uint64_t usage) {
    if (queue == nullptr || maxAcquired < 1 || maxAcquired > kMaxSlots - 2) return -EINVAL;
    pthread_mutex_lock(&queue->lock);
    queue->max_acquired = maxAcquired;
    queue->slot_count = maxAcquired + 2;
    queue->usage = usage | AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN;
    pthread_mutex_unlock(&queue->lock);
    return 0;
}

void minato_BufferQueue_setDefaultFormat(minato_BufferQueue* queue, int32_t format) {
    if (queue == nullptr) return;
    pthread_mutex_lock(&queue->lock);
    queue->default_format = format;
    pthread_mutex_unlock(&queue->lock);
}

void minato_BufferQueue_setDefaultDataSpace(minato_BufferQueue* queue, int32_t dataSpace) {
    if (queue == nullptr) return;
    pthread_mutex_lock(&queue->lock);
    queue->default_data_space = dataSpace;
    pthread_mutex_unlock(&queue->lock);
}

void minato_BufferQueue_discardFreeBuffers(minato_BufferQueue* queue) {
    if (queue == nullptr) return;
    AHardwareBuffer* discarded[kMaxSlots];
    int count = 0;
    pthread_mutex_lock(&queue->lock);
    // Every slot: a disconnect leaves free buffers beyond the slot count of the time.
    for (int index = 0; index < kMaxSlots; index++) {
        Slot& slot = queue->slots[index];
        if (slot.state == SlotState::kFree && slot.buffer != nullptr) {
            discarded[count++] = slot.buffer;
            slot.buffer = nullptr;
            slot.attached = false;
        }
    }
    pthread_mutex_unlock(&queue->lock);
    for (int index = 0; index < count; index++) AHardwareBuffer_release(discarded[index]);
}

void minato_BufferQueue_abandon(minato_BufferQueue* queue) {
    if (queue == nullptr) return;
    pthread_mutex_lock(&queue->lock);
    queue->abandoned = true;
    queue->available = nullptr;
    pthread_cond_broadcast(&queue->freed);
    pthread_mutex_unlock(&queue->lock);
}

int minato_BufferQueue_isAbandoned(minato_BufferQueue* queue) {
    if (queue == nullptr) return 1;
    pthread_mutex_lock(&queue->lock);
    const bool abandoned = queue->abandoned;
    pthread_mutex_unlock(&queue->lock);
    return abandoned ? 1 : 0;
}

void minato_BufferQueue_release(minato_BufferQueue* queue) {
    if (queue == nullptr) return;
    minato_BufferQueue_abandon(queue);
    unref(queue);
}

int minato_ANativeWindow_isQueue(ANativeWindow* window) {
    return is_window(window) && window->queue != nullptr ? 1 : 0;
}

int minato_ANativeWindow_dequeue(ANativeWindow* window, AHardwareBuffer** buffer, int32_t* slot) {
    if (!is_window(window) || window->queue == nullptr || buffer == nullptr || slot == nullptr) return -EINVAL;
    const Geometry geometry = dequeue_geometry(window);
    const int result = dequeue(window->queue, geometry.width, geometry.height, geometry.format, slot);
    if (result == 0) {
        pthread_mutex_lock(&window->queue->lock);
        *buffer = window->queue->slots[*slot].buffer;
        pthread_mutex_unlock(&window->queue->lock);
    }
    return result;
}

int minato_ANativeWindow_cancel(ANativeWindow* window, int32_t slot) {
    if (!is_window(window) || window->queue == nullptr || slot < 0 || slot >= kMaxSlots) return -EINVAL;
    minato_BufferQueue* queue = window->queue;
    pthread_mutex_lock(&queue->lock);
    const bool dequeued = queue->slots[slot].state == SlotState::kDequeued;
    if (dequeued) {
        queue->slots[slot].state = SlotState::kFree;
        pthread_cond_signal(&queue->freed);
    }
    pthread_mutex_unlock(&queue->lock);
    return dequeued ? 0 : -EINVAL;
}

int minato_ANativeWindow_queue(ANativeWindow* window, int32_t slot, int64_t timestamp) {
    if (!is_window(window) || window->queue == nullptr || slot < 0 || slot >= kMaxSlots) return -EINVAL;
    minato_BufferQueue_settings settings{};
    settings.timestamp = timestamp;
    pthread_mutex_lock(&window->lock);
    settings.transform = window->transform;
    settings.data_space = window->data_space;
    pthread_mutex_unlock(&window->lock);
    return queue_slot(window->queue, slot, settings);
}

int minato_ANativeWindow_queueWith(ANativeWindow* window, int32_t slot, const minato_BufferQueue_settings* settings) {
    if (!is_window(window) || window->queue == nullptr || settings == nullptr || slot < 0 || slot >= kMaxSlots) {
        return -EINVAL;
    }
    return queue_slot(window->queue, slot, *settings);
}

int minato_ANativeWindow_attachAndQueue(ANativeWindow* window, AHardwareBuffer* buffer,
                                        const minato_BufferQueue_settings* settings) {
    if (!is_window(window) || window->queue == nullptr || buffer == nullptr || settings == nullptr) return -EINVAL;
    minato_BufferQueue* queue = window->queue;
    AHardwareBuffer_Desc description{};
    AHardwareBuffer_describe(buffer, &description);
    AHardwareBuffer_acquire(buffer);
    // As libgui's attachBuffer: a free slot, waiting for one; a slot without a buffer first.
    pthread_mutex_lock(&queue->lock);
    int found = -1;
    while (!queue->abandoned) {
        for (int index = 0; index < queue->slot_count; index++) {
            const Slot& slot = queue->slots[index];
            if (slot.state != SlotState::kFree) continue;
            if (found < 0 || slot.buffer == nullptr) found = index;
            if (slot.buffer == nullptr) break;
        }
        if (found >= 0) break;
        pthread_cond_wait(&queue->freed, &queue->lock);
    }
    if (queue->abandoned) {
        pthread_mutex_unlock(&queue->lock);
        AHardwareBuffer_release(buffer);
        return -ENODEV;
    }
    Slot& slot = queue->slots[found];
    AHardwareBuffer* stale = slot.buffer;
    slot.buffer = buffer;
    slot.width = static_cast<int32_t>(description.width);
    slot.height = static_cast<int32_t>(description.height);
    slot.format = static_cast<int32_t>(description.format);
    slot.attached = true;
    slot.state = SlotState::kDequeued;
    pthread_mutex_unlock(&queue->lock);
    if (stale != nullptr) AHardwareBuffer_release(stale);
    return queue_slot(queue, found, *settings);
}

int minato_ANativeWindow_connect(ANativeWindow* window, int32_t maxDequeued, minato_BufferQueue_released released,
                                 void* context) {
    if (!is_window(window) || window->queue == nullptr || maxDequeued < 1) return -EINVAL;
    minato_BufferQueue* queue = window->queue;
    pthread_mutex_lock(&queue->lock);
    // As the tag's ImageWriter: maxImages + the consumer's undequeued buffers, its maxAcquired (one for a
    // SurfaceTexture), so that a dequeue waits once they are all out; more than BufferQueue's slots fail, as the tag's
    // set_buffer_count.
    const int32_t wanted = maxDequeued + (queue->max_acquired > 0 ? queue->max_acquired : 1);
    int result = 0;
    if (queue->abandoned) {
        result = -ENODEV;
    } else if (queue->connected || queue->claimed) {
        result = -EBUSY;
    } else if (wanted > kMaxSlots) {
        result = -EINVAL;
    } else {
        queue->connected = true;
        queue->released = released;
        queue->released_context = context;
        queue->unconnected_slot_count = queue->slot_count;
        queue->slot_count = slots_in_use_from(queue, wanted);
    }
    pthread_mutex_unlock(&queue->lock);
    return result;
}

void minato_ANativeWindow_disconnect(ANativeWindow* window) {
    if (!is_window(window) || window->queue == nullptr) return;
    minato_BufferQueue* queue = window->queue;
    // Under the callback lock: a call of `released` that has begun ends before its context may go.
    pthread_mutex_lock(&queue->callback_lock);
    pthread_mutex_lock(&queue->lock);
    if (queue->connected) {
        queue->connected = false;
        queue->released = nullptr;
        queue->released_context = nullptr;
        queue->slot_count = slots_in_use_from(queue, queue->unconnected_slot_count);
    }
    pthread_mutex_unlock(&queue->lock);
    pthread_mutex_unlock(&queue->callback_lock);
}

int minato_ANativeWindow_claim(ANativeWindow* window) {
    if (!is_window(window)) return -EINVAL;
    if (window->queue == nullptr) {  // a host window: its own flag, under the lock a CPU lock takes
        pthread_mutex_lock(&window->lock);
        const int result = window->claimed || window->locked ? -EBUSY : 0;
        if (result == 0) window->claimed = true;
        pthread_mutex_unlock(&window->lock);
        return result;
    }
    minato_BufferQueue* queue = window->queue;
    // The window's lock first, as a lock takes them: a lock and a claim never both go ahead.
    pthread_mutex_lock(&window->lock);
    pthread_mutex_lock(&queue->lock);
    int result = 0;
    if (queue->abandoned) {
        result = -ENODEV;
    } else if (queue->claimed || queue->connected || window->locked) {
        result = -EBUSY;
    } else {
        queue->claimed = true;
    }
    pthread_mutex_unlock(&queue->lock);
    pthread_mutex_unlock(&window->lock);
    return result;
}

void minato_ANativeWindow_unclaim(ANativeWindow* window) {
    if (!is_window(window)) return;
    if (window->queue == nullptr) {
        pthread_mutex_lock(&window->lock);
        window->claimed = false;
        pthread_mutex_unlock(&window->lock);
        return;
    }
    pthread_mutex_lock(&window->queue->lock);
    window->queue->claimed = false;
    pthread_mutex_unlock(&window->queue->lock);
}

int minato_ANativeWindow_isClaimed(ANativeWindow* window) {
    if (!is_window(window)) return 0;
    if (window->queue == nullptr) {
        pthread_mutex_lock(&window->lock);
        const bool claimed = window->claimed;
        pthread_mutex_unlock(&window->lock);
        return claimed ? 1 : 0;
    }
    pthread_mutex_lock(&window->queue->lock);
    const bool claimed = window->queue->claimed;
    pthread_mutex_unlock(&window->queue->lock);
    return claimed ? 1 : 0;
}

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
    if (window->queue != nullptr) unref(window->queue);
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
    return geometry.format != 0 ? geometry.format : own_format(window);
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
    // A queue window, a SurfaceTexture's, is no layer of the display, as on Android: nothing to ask the host for.
    if (window->queue != nullptr) return 0;
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
    if (!is_window(window) || (transform & ~kTransforms) != 0) return -EINVAL;
    pthread_mutex_lock(&window->lock);
    window->transform = transform;
    pthread_mutex_unlock(&window->lock);
    return 0;
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

// A queue's default (setDefaultDataSpace), else UNKNOWN.
int32_t ANativeWindow_getBuffersDefaultDataSpace(ANativeWindow* window) {
    if (!is_window(window)) return -EINVAL;
    if (window->queue == nullptr) return ADATASPACE_UNKNOWN;
    pthread_mutex_lock(&window->queue->lock);
    const int32_t data_space = window->queue->default_data_space;
    pthread_mutex_unlock(&window->queue->lock);
    return data_space;
}

void ANativeWindow_tryAllocateBuffers(ANativeWindow*) {}

// A buffer of setBuffersGeometry's size and format, else the window's, from the host's buffers. Without a dirty
// rectangle the whole buffer is dirty, as libgui's Surface::lock has it, and the host's answer replaces the app's.
/// A queue window's lock: a dequeued slot's buffer, locked for the CPU. The whole buffer is dirty, as nothing of the
/// last frame is copied into it.
static int32_t queue_lock(ANativeWindow* window, ANativeWindow_Buffer* outBuffer, ARect* inOutDirtyBounds) {
    // IMPLEMENTATION_DEFINED, a PRIVATE reader's, is no format the CPU writes: -EINVAL, as Android 15's
    // (image-ndk-test).
    if (dequeue_geometry(window).format == kImplementationDefined) return -EINVAL;
    pthread_mutex_lock(&window->lock);
    if (window->locked) {
        pthread_mutex_unlock(&window->lock);
        return kInvalidOperation;
    }
    pthread_mutex_lock(&window->queue->lock);
    const bool claimed = window->queue->claimed;
    pthread_mutex_unlock(&window->queue->lock);
    if (claimed) {
        pthread_mutex_unlock(&window->lock);
        return -EINVAL;  // a decoder's: libgui's connect of the CPU fails, "already connected"
    }
    window->locked = true;
    pthread_mutex_unlock(&window->lock);
    AHardwareBuffer* buffer = nullptr;
    int32_t slot = -1;
    int32_t result = minato_ANativeWindow_dequeue(window, &buffer, &slot);
    void* bits = nullptr;
    if (result == 0 && AHardwareBuffer_lock(buffer, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, nullptr, &bits) != 0) {
        pthread_mutex_lock(&window->queue->lock);
        window->queue->slots[slot].state = SlotState::kFree;
        pthread_cond_signal(&window->queue->freed);
        pthread_mutex_unlock(&window->queue->lock);
        result = -ENOMEM;
    }
    pthread_mutex_lock(&window->lock);
    window->locked = result == 0;
    window->locked_slot = result == 0 ? slot : -1;
    pthread_mutex_unlock(&window->lock);
    if (result != 0) return result;
    AHardwareBuffer_Desc description{};
    AHardwareBuffer_describe(buffer, &description);
    outBuffer->width = static_cast<int32_t>(description.width);
    outBuffer->height = static_cast<int32_t>(description.height);
    outBuffer->stride = static_cast<int32_t>(description.stride);
    outBuffer->format = static_cast<int32_t>(description.format);
    outBuffer->bits = bits;
    if (inOutDirtyBounds != nullptr) {
        *inOutDirtyBounds = {0, 0, outBuffer->width, outBuffer->height};
    }
    return 0;
}

/// A queue window's post: the slot's buffer unlocked and queued. The slot is taken from the window at once, so that a
/// second post meanwhile finds none.
static int32_t queue_post(ANativeWindow* window) {
    pthread_mutex_lock(&window->lock);
    const int32_t slot = window->locked ? window->locked_slot : -1;
    window->locked_slot = -1;
    pthread_mutex_unlock(&window->lock);
    if (slot < 0) return kInvalidOperation;
    pthread_mutex_lock(&window->queue->lock);
    AHardwareBuffer* buffer = window->queue->slots[slot].buffer;
    pthread_mutex_unlock(&window->queue->lock);
    AHardwareBuffer_unlock(buffer, nullptr);
    const int32_t result = minato_ANativeWindow_queue(window, slot, 0);
    pthread_mutex_lock(&window->lock);
    window->locked = false;
    window->locked_slot = -1;
    pthread_mutex_unlock(&window->lock);
    return result;
}

int32_t ANativeWindow_lock(ANativeWindow* window, ANativeWindow_Buffer* outBuffer, ARect* inOutDirtyBounds) {
    if (!is_window(window) || outBuffer == nullptr) return -EINVAL;
    if (window->queue != nullptr) return queue_lock(window, outBuffer, inOutDirtyBounds);
    const Size now = size(window);
    apkrt_window_buffer buffer{};
    pthread_mutex_lock(&window->lock);
    if (window->locked) {
        pthread_mutex_unlock(&window->lock);
        return kInvalidOperation;
    }
    if (window->claimed) {
        pthread_mutex_unlock(&window->lock);
        return -EINVAL;  // a decoder's, as on a queue window [verified: Android 15, media-window-test, -22]
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
    if (window->queue != nullptr) return queue_post(window);
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
