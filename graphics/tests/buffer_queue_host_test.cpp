// The graphics breadth's G3b: buffer-queue-host-test, libnativewindow's buffer queue
// (runtime/android/native_window.cpp) built for the Mac with a counted fake AHardwareBuffer, so that the macos preset's
// AddressSanitizer and UndefinedBehaviorSanitizer see the queue's own memory, which buffer-queue-test runs as guest
// code they do not reach:
// - ownership: frames through an ImageReader's consumer and an ImageWriter's producer, a reader's acquired frame
//   detached and attached to another queue, every queue let go; then no buffer is left and none was released twice;
// - a SurfaceTexture's queue with a writer of three images: slots past its three, all below
//   MINATO_BUFFER_QUEUE_MAX_SLOTS, and its three again after the disconnect; too many slots refused;
// - a writer's disconnect racing the consumer's releases, whose callback must never run after the disconnect has
//   returned (its context is freed right after);
// - the queue abandoned while a dequeue and while an attach wait for a free slot: both wake with -ENODEV, the
//   attach's buffer reference given back;
// - detach and attach racing the consumers' releases of two queues on three threads;
// - libmediandk's AImageReader (runtime/android/image_reader.cpp, G3c) on the same queue: a delete with images still
//   held, which then answer AMEDIA_ERROR_INVALID_OBJECT until their own delete; a delete racing a listener call that
//   runs, whose context is freed right after; a delete from inside the listener; a producer racing the delete;
//   listeners set from two threads at once; frames before a listener not called back; a failed acquire's NULL; a
//   PRIVATE image without planes; an image of the producer's crop.
// Each failure prints its check and exits 1; the last line is "done".
#include <android/hardware_buffer.h>
#include <android/native_window.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <atomic>

#include <media/NdkImage.h>
#include <media/NdkImageReader.h>

#include "APKRuntime/buffer_queue.h"

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            printf("failed: %s (line %d)\n", #condition, __LINE__);          \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

// The fake buffer: its description, memory and references; freed at the last release, so that a use after it is
// AddressSanitizer's heap-use-after-free, and a release past the last aborts.
struct AHardwareBuffer {
    uint32_t magic;
    std::atomic<int32_t> references;
    AHardwareBuffer_Desc description;
    void *memory;
    std::atomic<bool> locked;
};

namespace {

constexpr uint32_t kMagic = 0x46414b45;  // "FAKE"
std::atomic<int32_t> live_buffers{0};

AHardwareBuffer *checked(const AHardwareBuffer *buffer) {
    CHECK(buffer != nullptr && buffer->magic == kMagic);
    return const_cast<AHardwareBuffer *>(buffer);
}

}  // namespace

extern "C" {

int AHardwareBuffer_allocate(const AHardwareBuffer_Desc *description, AHardwareBuffer **out) {
    auto *buffer = new AHardwareBuffer;
    buffer->magic = kMagic;
    buffer->references = 1;
    buffer->description = *description;
    buffer->description.stride = description->width;
    buffer->memory = calloc(static_cast<size_t>(description->width) * description->height, 8);
    buffer->locked = false;
    live_buffers++;
    *out = buffer;
    return 0;
}

void AHardwareBuffer_acquire(AHardwareBuffer *buffer) {
    CHECK(checked(buffer)->references.fetch_add(1) > 0);
}

void AHardwareBuffer_release(AHardwareBuffer *buffer) {
    const int32_t before = checked(buffer)->references.fetch_sub(1);
    CHECK(before > 0);
    if (before != 1) return;
    CHECK(!buffer->locked);
    buffer->magic = 0;
    free(buffer->memory);
    delete buffer;
    live_buffers--;
}

void AHardwareBuffer_describe(const AHardwareBuffer *buffer, AHardwareBuffer_Desc *description) {
    *description = checked(buffer)->description;
}

int AHardwareBuffer_lock(AHardwareBuffer *buffer, uint64_t, int32_t, const ARect *, void **address) {
    CHECK(!checked(buffer)->locked.exchange(true));
    *address = buffer->memory;
    return 0;
}

// The test's readers are RGB: a YUV image's planes (image_reader.cpp's lockPlanes) are image-ndk-test's.
int AHardwareBuffer_lockPlanes(AHardwareBuffer *, uint64_t, int32_t, const ARect *, AHardwareBuffer_Planes *) {
    return -EINVAL;
}

int AHardwareBuffer_unlock(AHardwareBuffer *buffer, int32_t *fence) {
    CHECK(checked(buffer)->locked.exchange(false));
    if (fence != nullptr) *fence = -1;
    return 0;
}

// The host windows' syscalls (the window's size, the frame rate): a queue window makes none.
int minato_host_syscall(int, ...) {
    errno = ENOSYS;
    return -1;
}

}  // extern "C"

namespace {

struct Queue {
    ANativeWindow *window = nullptr;
    minato_BufferQueue *consumer = nullptr;
};

Queue make_queue(int32_t maxAcquired) {
    Queue made;
    CHECK(minato_BufferQueue_create(nullptr, nullptr, nullptr, &made.window, &made.consumer) == 0);
    CHECK(minato_BufferQueue_setConsumer(made.consumer, maxAcquired, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN) == 0);
    minato_BufferQueue_setDefaultSize(made.consumer, 4, 4);
    return made;
}

void let_go(Queue &queue) {
    minato_BufferQueue_release(queue.consumer);
    ANativeWindow_release(queue.window);
}

// One frame from the producer: a dequeue, written and queued with its timestamp; the dequeue's answer otherwise.
int produce(ANativeWindow *window, int64_t timestamp) {
    AHardwareBuffer *buffer = nullptr;
    int32_t slot = -1;
    const int dequeued = minato_ANativeWindow_dequeue(window, &buffer, &slot);
    if (dequeued != 0) return dequeued;
    void *address = nullptr;
    CHECK(AHardwareBuffer_lock(buffer, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, nullptr, &address) == 0);
    memcpy(address, &timestamp, sizeof timestamp);
    AHardwareBuffer_unlock(buffer, nullptr);
    minato_BufferQueue_settings settings{};
    settings.timestamp = timestamp;
    return minato_ANativeWindow_queueWith(window, slot, &settings);
}

// A release callback's context, whose magic goes before it is freed; a slow one sleeps inside the call, which widens
// the window in which a disconnect that does not wait for the call would free it.
struct Listener {
    uint32_t magic = kMagic;
    bool slow = false;
    std::atomic<int32_t> calls{0};
};

void released(void *context) {
    auto *listener = static_cast<Listener *>(context);
    CHECK(listener->magic == kMagic);
    if (listener->slow) usleep(200);
    CHECK(listener->magic == kMagic);
    listener->calls++;
}

void ownership() {
    Queue target = make_queue(2);
    Listener listener;
    CHECK(minato_ANativeWindow_connect(target.window, 2, released, &listener) == 0);
    CHECK(minato_ANativeWindow_connect(target.window, 2, released, &listener) == -EBUSY);
    for (int64_t frame = 1; frame <= 3; frame++) CHECK(produce(target.window, frame) == 0);
    for (int64_t frame = 1; frame <= 3; frame++) {
        minato_BufferQueue_frame got{};
        CHECK(minato_BufferQueue_acquire(target.consumer, &got) == 0);
        CHECK(got.timestamp == frame);
        minato_BufferQueue_releaseSlot(target.consumer, got.slot);
    }
    CHECK(listener.calls == 3);

    Queue source = make_queue(1);
    CHECK(produce(source.window, 7) == 0);
    minato_BufferQueue_frame detached{};
    CHECK(minato_BufferQueue_acquire(source.consumer, &detached) == 0);
    AHardwareBuffer_acquire(detached.buffer);  // the image's own reference
    CHECK(minato_BufferQueue_detach(source.consumer, detached.slot) == 0);
    CHECK(minato_BufferQueue_detach(source.consumer, detached.slot) == -EINVAL);
    minato_BufferQueue_settings settings{};
    settings.timestamp = 9;
    CHECK(minato_ANativeWindow_attachAndQueue(target.window, detached.buffer, &settings) == 0);
    AHardwareBuffer_release(detached.buffer);  // the image's close
    minato_BufferQueue_frame attached{};
    CHECK(minato_BufferQueue_acquire(target.consumer, &attached) == 0);
    CHECK(attached.buffer == detached.buffer && attached.timestamp == 9);
    minato_BufferQueue_releaseSlot(target.consumer, attached.slot);  // the buffer leaves the queue: its last reference
    CHECK(listener.calls == 4);
    minato_ANativeWindow_disconnect(target.window);
    let_go(source);
    let_go(target);
    CHECK(live_buffers == 0);
    printf("ownership: fine\n");
}

void surface_texture_slots() {
    Queue queue;
    CHECK(minato_BufferQueue_create(nullptr, nullptr, nullptr, &queue.window, &queue.consumer) == 0);
    minato_BufferQueue_setDefaultSize(queue.consumer, 4, 4);
    Listener listener;
    CHECK(minato_ANativeWindow_connect(queue.window, MINATO_BUFFER_QUEUE_MAX_SLOTS, released, &listener) == -EINVAL);
    CHECK(minato_ANativeWindow_connect(queue.window, 3, released, &listener) == 0);
    for (int64_t frame = 1; frame <= 4; frame++) CHECK(produce(queue.window, frame) == 0);
    int32_t highest = 0;
    for (int64_t frame = 1; frame <= 4; frame++) {
        minato_BufferQueue_frame got{};
        CHECK(minato_BufferQueue_acquire(queue.consumer, &got) == 0);  // a SurfaceTexture's: lets the last one go
        CHECK(got.timestamp == frame && got.slot >= 0 && got.slot < MINATO_BUFFER_QUEUE_MAX_SLOTS);
        if (got.slot > highest) highest = got.slot;
    }
    CHECK(highest == 3);
    minato_ANativeWindow_disconnect(queue.window);
    for (int64_t frame = 5; frame <= 12; frame++) {
        CHECK(produce(queue.window, frame) == 0);
        minato_BufferQueue_frame got{};
        CHECK(minato_BufferQueue_acquire(queue.consumer, &got) == 0);
        CHECK(got.slot < 3);
    }
    let_go(queue);
    CHECK(live_buffers == 0);
    printf("a SurfaceTexture's slots with a writer: fine\n");
}

struct Cycle {
    Queue queue;
    std::atomic<bool> stop{false};
    std::atomic<int64_t> produced{0};
};

void *producer(void *context) {
    auto *cycle = static_cast<Cycle *>(context);
    while (!cycle->stop) {
        if (produce(cycle->queue.window, cycle->produced + 1) == -ENODEV) break;
        cycle->produced++;
    }
    return nullptr;
}

void *consumer(void *context) {
    auto *cycle = static_cast<Cycle *>(context);
    while (!cycle->stop) {
        minato_BufferQueue_frame got{};
        const int result = minato_BufferQueue_acquire(cycle->queue.consumer, &got);
        if (result == -ENODEV) break;
        if (result == 0) minato_BufferQueue_releaseSlot(cycle->queue.consumer, got.slot);
    }
    return nullptr;
}

void disconnect_racing_releases() {
    Cycle cycle;
    cycle.queue = make_queue(1);
    pthread_t threads[2];
    pthread_create(&threads[0], nullptr, producer, &cycle);
    pthread_create(&threads[1], nullptr, consumer, &cycle);
    for (int round = 0; round < 2000; round++) {
        auto *listener = new Listener;
        listener->slow = true;
        CHECK(minato_ANativeWindow_connect(cycle.queue.window, 1, released, listener) == 0);
        usleep(round % 7 == 0 ? 50 : 0);
        minato_ANativeWindow_disconnect(cycle.queue.window);
        listener->magic = 0;
        delete listener;  // a call after the disconnect is a heap-use-after-free
    }
    cycle.stop = true;
    minato_BufferQueue_abandon(cycle.queue.consumer);  // wakes a waiting dequeue
    pthread_join(threads[0], nullptr);
    pthread_join(threads[1], nullptr);
    let_go(cycle.queue);
    CHECK(live_buffers == 0);
    CHECK(cycle.produced > 0);
    printf("disconnect racing releases: fine\n");
}

struct Waiter {
    Queue *queue;
    AHardwareBuffer *buffer;  // for an attach, else a dequeue
    std::atomic<bool> done{false};
    int result = 0;
};

void *wait_for_slot(void *context) {
    auto *waiter = static_cast<Waiter *>(context);
    if (waiter->buffer != nullptr) {
        minato_BufferQueue_settings settings{};
        waiter->result = minato_ANativeWindow_attachAndQueue(waiter->queue->window, waiter->buffer, &settings);
    } else {
        AHardwareBuffer *buffer = nullptr;
        int32_t slot = -1;
        waiter->result = minato_ANativeWindow_dequeue(waiter->queue->window, &buffer, &slot);
    }
    waiter->done = true;
    return nullptr;
}

void abandon_while_waiting(bool attach) {
    Queue queue = make_queue(1);
    Listener listener;
    CHECK(minato_ANativeWindow_connect(queue.window, 1, released, &listener) == 0);
    // Two slots: one acquired, one queued.
    CHECK(produce(queue.window, 1) == 0);
    minato_BufferQueue_frame held{};
    CHECK(minato_BufferQueue_acquire(queue.consumer, &held) == 0);
    CHECK(produce(queue.window, 2) == 0);
    AHardwareBuffer_Desc description{4, 4, 1, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM, 0, 0, 0, 0};
    AHardwareBuffer *extra = nullptr;
    if (attach) AHardwareBuffer_allocate(&description, &extra);
    Waiter waiter{&queue, extra};
    pthread_t thread;
    pthread_create(&thread, nullptr, wait_for_slot, &waiter);
    usleep(100000);
    CHECK(!waiter.done);
    minato_BufferQueue_abandon(queue.consumer);
    pthread_join(thread, nullptr);
    CHECK(waiter.result == -ENODEV);
    if (extra != nullptr) AHardwareBuffer_release(extra);
    minato_BufferQueue_releaseSlot(queue.consumer, held.slot);
    minato_ANativeWindow_disconnect(queue.window);
    let_go(queue);
    CHECK(live_buffers == 0);
    printf("abandon while %s waits: fine\n", attach ? "an attach" : "a dequeue");
}

struct Mover {
    Queue source;
    Queue target;
    std::atomic<bool> stop{false};
    std::atomic<int32_t> moved{0};
};

void *move_frames(void *context) {
    auto *mover = static_cast<Mover *>(context);
    for (int frame = 0; frame < 3000 && !mover->stop; frame++) {
        if (produce(mover->source.window, frame + 1) != 0) break;
        minato_BufferQueue_frame got{};
        if (minato_BufferQueue_acquire(mover->source.consumer, &got) != 0) continue;
        AHardwareBuffer_acquire(got.buffer);
        CHECK(minato_BufferQueue_detach(mover->source.consumer, got.slot) == 0);
        minato_BufferQueue_settings settings{};
        settings.timestamp = frame + 1;
        const int result = minato_ANativeWindow_attachAndQueue(mover->target.window, got.buffer, &settings);
        AHardwareBuffer_release(got.buffer);
        if (result != 0) break;
        mover->moved++;
    }
    return nullptr;
}

void *drain_target(void *context) {
    auto *mover = static_cast<Mover *>(context);
    while (!mover->stop) {
        minato_BufferQueue_frame got{};
        const int result = minato_BufferQueue_acquire(mover->target.consumer, &got);
        if (result == -ENODEV) break;
        if (result == 0) minato_BufferQueue_releaseSlot(mover->target.consumer, got.slot);
    }
    return nullptr;
}

void detach_attach_racing_releases() {
    Mover mover;
    mover.source = make_queue(1);
    mover.target = make_queue(2);
    Listener listener;
    CHECK(minato_ANativeWindow_connect(mover.target.window, 2, released, &listener) == 0);
    pthread_t threads[3];
    pthread_create(&threads[0], nullptr, move_frames, &mover);
    pthread_create(&threads[1], nullptr, drain_target, &mover);
    pthread_create(&threads[2], nullptr, drain_target, &mover);
    pthread_join(threads[0], nullptr);
    mover.stop = true;
    minato_BufferQueue_abandon(mover.target.consumer);
    pthread_join(threads[1], nullptr);
    pthread_join(threads[2], nullptr);
    minato_ANativeWindow_disconnect(mover.target.window);
    let_go(mover.source);
    let_go(mover.target);
    CHECK(live_buffers == 0);
    CHECK(mover.moved > 0);
    printf("detach and attach racing releases: fine\n");
}

// A CPU frame of one color through a reader's window: ANativeWindow_lock's answer.
int post(ANativeWindow *window, uint32_t color) {
    ANativeWindow_Buffer buffer;
    const int locked = ANativeWindow_lock(window, &buffer, nullptr);
    if (locked != 0) return locked;
    static_cast<uint32_t *>(buffer.bits)[0] = color;
    return ANativeWindow_unlockAndPost(window);
}

AImage *acquire_waiting(AImageReader *reader) {
    AImage *image = nullptr;
    for (int tries = 0; tries < 200 && AImageReader_acquireNextImage(reader, &image) != AMEDIA_OK; tries++) {
        usleep(1000);
    }
    return image;
}

void reader_delete_with_images_held() {
    AImageReader *reader = nullptr;
    CHECK(AImageReader_new(4, 4, AIMAGE_FORMAT_RGBA_8888, 2, &reader) == AMEDIA_OK);
    ANativeWindow *window = nullptr;
    CHECK(AImageReader_getWindow(reader, &window) == AMEDIA_OK);
    CHECK(post(window, 1) == 0);
    AImage *first = acquire_waiting(reader);
    CHECK(post(window, 2) == 0);
    AImage *second = acquire_waiting(reader);
    CHECK(first != nullptr && second != nullptr);
    uint8_t *data = nullptr;
    int length = 0;
    CHECK(AImage_getPlaneData(first, 0, &data, &length) == AMEDIA_OK);  // locked, and to be unlocked by the delete
    AImageReader_delete(reader);
    int32_t width = 0;
    AHardwareBuffer *buffer = nullptr;
    CHECK(AImage_getWidth(first, &width) == AMEDIA_ERROR_INVALID_OBJECT && width == -1);
    CHECK(AImage_getHardwareBuffer(second, &buffer) == AMEDIA_ERROR_INVALID_OBJECT);
    AImage_delete(first);
    AImage_delete(second);
    CHECK(live_buffers == 0);
    printf("reader delete with images held: fine\n");
}

struct Called {
    uint32_t magic = kMagic;
    std::atomic<bool> running{false};
    std::atomic<int32_t> calls{0};
    AImageReader *delete_inside = nullptr;  // a delete from inside the call, once
};

void image_available(void *context, AImageReader *reader) {
    auto *called = static_cast<Called *>(context);
    CHECK(called->magic == kMagic);
    called->running = true;
    if (called->delete_inside == reader) {
        called->delete_inside = nullptr;
        AImageReader_delete(reader);
    } else {
        usleep(300);
    }
    CHECK(called->magic == kMagic);
    called->calls++;
}

void reader_delete_racing_listener() {
    for (int round = 0; round < 100; round++) {
        auto *called = new Called;
        AImageReader *reader = nullptr;
        CHECK(AImageReader_new(4, 4, AIMAGE_FORMAT_RGBA_8888, 1, &reader) == AMEDIA_OK);
        AImageReader_ImageListener listener{called, image_available};
        CHECK(AImageReader_setImageListener(reader, &listener) == AMEDIA_OK);
        ANativeWindow *window = nullptr;
        AImageReader_getWindow(reader, &window);
        CHECK(post(window, 3) == 0);
        for (int tries = 0; tries < 2000 && !called->running; tries++) usleep(10);
        AImageReader_delete(reader);  // waits for the call that runs
        called->magic = 0;
        delete called;  // a call after the delete is a heap-use-after-free
    }
    CHECK(live_buffers == 0);
    printf("reader delete racing a listener call: fine\n");
}

void reader_delete_inside_listener() {
    Called called;
    AImageReader *reader = nullptr;
    CHECK(AImageReader_new(4, 4, AIMAGE_FORMAT_RGBA_8888, 1, &reader) == AMEDIA_OK);
    called.delete_inside = reader;
    AImageReader_ImageListener listener{&called, image_available};
    AImageReader_setImageListener(reader, &listener);
    ANativeWindow *window = nullptr;
    AImageReader_getWindow(reader, &window);
    // Nothing else holds the window: the delete inside the call lets the queue go, and the dispatcher thread, which
    // ends by itself after the call, outlives the queue's context.
    CHECK(post(window, 4) == 0);
    for (int tries = 0; tries < 2000 && called.calls == 0; tries++) usleep(100);
    CHECK(called.calls == 1);
    usleep(10000);
    CHECK(live_buffers == 0);
    printf("reader delete inside the listener: fine\n");
}

struct Race {
    AImageReader *reader;
    ANativeWindow *window;
    std::atomic<bool> stop{false};
};

void *post_frames(void *context) {
    auto *race = static_cast<Race *>(context);
    for (uint32_t color = 0; !race->stop; color++) {
        if (post(race->window, color) == -ENODEV) break;
    }
    return nullptr;
}

void *take_latest(void *context) {
    auto *race = static_cast<Race *>(context);
    while (!race->stop) {
        AImage *image = nullptr;
        if (AImageReader_acquireLatestImage(race->reader, &image) == AMEDIA_OK) {
            uint8_t *data = nullptr;
            int length = 0;
            AImage_getPlaneData(image, 0, &data, &length);
            AImage_delete(image);
        }
    }
    return nullptr;
}

struct Counter {
    std::atomic<int32_t> calls{0};
};

void count_call(void *context, AImageReader *) {
    static_cast<Counter *>(context)->calls++;
}

void *set_listener_repeatedly(void *context) {
    auto *reader = static_cast<AImageReader *>(context);
    static Counter counter;
    AImageReader_ImageListener listener{&counter, count_call};
    for (int round = 0; round < 200; round++) {
        AImageReader_setImageListener(reader, round % 3 == 2 ? nullptr : &listener);
    }
    AImageReader_setImageListener(reader, &listener);
    return nullptr;
}

void reader_listeners_and_answers() {
    // Listeners set from two threads at once: one dispatcher thread, which the delete waits for.
    AImageReader *reader = nullptr;
    CHECK(AImageReader_new(4, 4, AIMAGE_FORMAT_RGBA_8888, 1, &reader) == AMEDIA_OK);
    ANativeWindow *window = nullptr;
    AImageReader_getWindow(reader, &window);
    pthread_t setters[2];
    pthread_create(&setters[0], nullptr, set_listener_repeatedly, reader);
    pthread_create(&setters[1], nullptr, set_listener_repeatedly, reader);
    for (int frame = 0; frame < 20; frame++) {
        post(window, static_cast<uint32_t>(frame));
        AImage *image = nullptr;
        if (AImageReader_acquireNextImage(reader, &image) == AMEDIA_OK) AImage_delete(image);
    }
    pthread_join(setters[0], nullptr);
    pthread_join(setters[1], nullptr);
    AImageReader_delete(reader);

    // Frames that came without a listener are not called back once one is set; a failed acquire leaves NULL.
    CHECK(AImageReader_new(4, 4, AIMAGE_FORMAT_RGBA_8888, 1, &reader) == AMEDIA_OK);
    AImageReader_getWindow(reader, &window);
    for (int frame = 0; frame < 3; frame++) {
        CHECK(post(window, static_cast<uint32_t>(frame)) == 0);
        AImage *image = acquire_waiting(reader);
        CHECK(image != nullptr);
        AImage_delete(image);
    }
    auto *stale = reinterpret_cast<AImage *>(uintptr_t{1});
    CHECK(AImageReader_acquireLatestImage(reader, &stale) == AMEDIA_IMGREADER_NO_BUFFER_AVAILABLE && stale == nullptr);
    Counter counter;
    AImageReader_ImageListener listener{&counter, count_call};
    AImageReader_setImageListener(reader, &listener);
    usleep(20000);
    CHECK(counter.calls == 0);
    AImageReader_delete(reader);

    // A PRIVATE image has no planes; an image is its producer's crop.
    CHECK(AImageReader_new(4, 4, AIMAGE_FORMAT_PRIVATE, 1, &reader) == AMEDIA_OK);
    AImageReader_getWindow(reader, &window);
    CHECK(produce(window, 1) == 0);
    AImage *opaque = acquire_waiting(reader);
    int32_t planes = -1;
    uint8_t *data = nullptr;
    int length = 0;
    AHardwareBuffer *buffer = nullptr;
    CHECK(AImage_getNumberOfPlanes(opaque, &planes) == AMEDIA_OK && planes == 0);
    CHECK(AImage_getPlaneData(opaque, 0, &data, &length) == AMEDIA_ERROR_INVALID_PARAMETER);
    CHECK(AImage_getHardwareBuffer(opaque, &buffer) == AMEDIA_OK && buffer != nullptr);
    AImage_delete(opaque);
    AImageReader_delete(reader);
    CHECK(AImageReader_new(4, 4, AIMAGE_FORMAT_RGBA_8888, 1, &reader) == AMEDIA_OK);
    AImageReader_getWindow(reader, &window);
    AHardwareBuffer *dequeued = nullptr;
    int32_t slot = -1;
    CHECK(minato_ANativeWindow_dequeue(window, &dequeued, &slot) == 0);
    minato_BufferQueue_settings settings{};
    settings.crop_right = 2;
    settings.crop_bottom = 3;
    CHECK(minato_ANativeWindow_queueWith(window, slot, &settings) == 0);
    AImage *cropped = acquire_waiting(reader);
    int32_t width = 0, height = 0;
    AImageCropRect crop{};
    AImage_getWidth(cropped, &width);
    AImage_getHeight(cropped, &height);
    AImage_getCropRect(cropped, &crop);
    CHECK(width == 2 && height == 3 && crop.left == 0 && crop.top == 0 && crop.right == 2 && crop.bottom == 3);
    AImage_delete(cropped);
    AImageReader_delete(reader);
    CHECK(live_buffers == 0);
    printf("reader listeners and answers: fine\n");
}

void reader_delete_racing_producer() {
    Race race{};
    CHECK(AImageReader_new(4, 4, AIMAGE_FORMAT_RGBA_8888, 2, &race.reader) == AMEDIA_OK);
    AImageReader_getWindow(race.reader, &race.window);
    ANativeWindow_acquire(race.window);
    pthread_t threads[2];
    pthread_create(&threads[0], nullptr, post_frames, &race);
    pthread_create(&threads[1], nullptr, take_latest, &race);
    usleep(50000);
    race.stop = true;
    // The acquiring thread ends first (an acquire after the delete would use a freed reader, as on Android); the
    // producer races the delete and wakes with -ENODEV.
    pthread_join(threads[1], nullptr);
    AImageReader_delete(race.reader);
    pthread_join(threads[0], nullptr);
    ANativeWindow_release(race.window);
    CHECK(live_buffers == 0);
    printf("reader delete racing a producer: fine\n");
}

}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    ownership();
    surface_texture_slots();
    disconnect_racing_releases();
    abandon_while_waiting(false);
    abandon_while_waiting(true);
    detach_attach_racing_releases();
    reader_delete_with_images_held();
    reader_delete_racing_listener();
    reader_delete_inside_listener();
    reader_listeners_and_answers();
    reader_delete_racing_producer();
    printf("done\n");
    return 0;
}
