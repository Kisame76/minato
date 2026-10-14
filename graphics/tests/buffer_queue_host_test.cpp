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
// - detach and attach racing the consumers' releases of two queues on three threads.
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

}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    ownership();
    surface_texture_slots();
    disconnect_racing_releases();
    abandon_while_waiting(false);
    abandon_while_waiting(true);
    detach_attach_racing_releases();
    printf("done\n");
    return 0;
}
