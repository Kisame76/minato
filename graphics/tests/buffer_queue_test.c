/* ADR-0058 c): buffer-queue-test, libnativewindow's buffer queue (APKRuntime/buffer_queue.h) through its private
   functions, as libandroid_runtime's SurfaceTexture and libEGL reach them, on graphics/host's hardware buffers:
   - a queue window's size and lock before and after setDefaultSize, a post, the available callback, an acquire;
   - a newer frame replacing one still queued, with one callback for the two; a second queue of the same slot, which
     only a dequeued slot takes; a cancel;
   - a dequeue that waits for a free slot and wakes with -ENODEV when the queue is abandoned;
   - the context's destroy, which comes once both the window and the consumer let the queue go, and not before;
     also for a window released while it is locked;
   - an ImageReader's consumer (G3a), and an ImageWriter as the producer (G3b): one connection, every frame kept with
     its timestamp, data space and crop, the release callback (not for a cancel), the slot count's wait, and a
     reader's acquired buffer detached and attached to another queue, leaving it again at the release.
   No Android reference: the functions are Minato's own. The output is derived by hand. */
#include <android/hardware_buffer.h>
#include <android/native_window.h>
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#include "APKRuntime/buffer_queue.h"

static __typeof__(&minato_BufferQueue_create) create;
static __typeof__(&minato_BufferQueue_setDefaultSize) set_default_size;
static __typeof__(&minato_BufferQueue_acquire) acquire;
static __typeof__(&minato_BufferQueue_abandon) abandon;
static __typeof__(&minato_BufferQueue_release) release;
static __typeof__(&minato_ANativeWindow_dequeue) dequeue;
static __typeof__(&minato_ANativeWindow_queue) queue;
static __typeof__(&minato_ANativeWindow_cancel) cancel;
static __typeof__(&minato_BufferQueue_setConsumer) set_consumer;
static __typeof__(&minato_BufferQueue_setDefaultFormat) set_default_format;
static __typeof__(&minato_BufferQueue_releaseSlot) release_slot;
static __typeof__(&minato_BufferQueue_discardFreeBuffers) discard_free_buffers;
static __typeof__(&minato_BufferQueue_detach) detach;
static __typeof__(&minato_ANativeWindow_connect) connect_producer;
static __typeof__(&minato_ANativeWindow_disconnect) disconnect_producer;
static __typeof__(&minato_ANativeWindow_queueWith) queue_with;
static __typeof__(&minato_ANativeWindow_attachAndQueue) attach_and_queue;

static atomic_int available_calls;
static atomic_int destroyed;
static atomic_int released_calls;

static void released(void *context) {
    (void)context;
    atomic_fetch_add(&released_calls, 1);
}

static void available(void *context) {
    (void)context;
    atomic_fetch_add(&available_calls, 1);
}

static void destroy(void *context) {
    atomic_fetch_add((atomic_int *)context, 1);
}

static int found(void) {
    void *library = dlopen("libnativewindow.so", RTLD_NOW);
    if (library == NULL) {
        return 0;
    }
#define FIND(variable, name) variable = (__typeof__(variable))dlsym(library, #name)
    FIND(create, minato_BufferQueue_create);
    FIND(set_default_size, minato_BufferQueue_setDefaultSize);
    FIND(acquire, minato_BufferQueue_acquire);
    FIND(abandon, minato_BufferQueue_abandon);
    FIND(release, minato_BufferQueue_release);
    FIND(dequeue, minato_ANativeWindow_dequeue);
    FIND(queue, minato_ANativeWindow_queue);
    FIND(cancel, minato_ANativeWindow_cancel);
    FIND(set_consumer, minato_BufferQueue_setConsumer);
    FIND(set_default_format, minato_BufferQueue_setDefaultFormat);
    FIND(release_slot, minato_BufferQueue_releaseSlot);
    FIND(discard_free_buffers, minato_BufferQueue_discardFreeBuffers);
    FIND(detach, minato_BufferQueue_detach);
    FIND(connect_producer, minato_ANativeWindow_connect);
    FIND(disconnect_producer, minato_ANativeWindow_disconnect);
    FIND(queue_with, minato_ANativeWindow_queueWith);
    FIND(attach_and_queue, minato_ANativeWindow_attachAndQueue);
#undef FIND
    return create != NULL && set_default_size != NULL && acquire != NULL && abandon != NULL && release != NULL &&
           dequeue != NULL && queue != NULL && cancel != NULL;
}

/* A CPU frame of one color, posted. */
static int post(struct ANativeWindow *window, uint32_t color) {
    ANativeWindow_Buffer buffer;
    const int locked = ANativeWindow_lock(window, &buffer, NULL);
    if (locked != 0) {
        return locked;
    }
    for (int32_t y = 0; y < buffer.height; y++) {
        for (int32_t x = 0; x < buffer.width; x++) {
            ((uint32_t *)buffer.bits)[y * buffer.stride + x] = color;
        }
    }
    return ANativeWindow_unlockAndPost(window);
}

static uint32_t first_pixel(struct AHardwareBuffer *buffer) {
    void *address = NULL;
    uint32_t pixel = 0;
    if (AHardwareBuffer_lock(buffer, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, NULL, &address) == 0) {
        pixel = *(uint32_t *)address;
        AHardwareBuffer_unlock(buffer, NULL);
    }
    return pixel;
}

/* A frame of one color from an ImageWriter's dequeue, queued with `settings`. */
static int write_frame(struct ANativeWindow *window, uint32_t color,
                       const struct minato_BufferQueue_settings *settings) {
    struct AHardwareBuffer *buffer = NULL;
    int32_t slot = -1;
    const int dequeued = dequeue(window, &buffer, &slot);
    if (dequeued != 0) {
        return dequeued;
    }
    void *address = NULL;
    if (AHardwareBuffer_lock(buffer, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, NULL, &address) == 0) {
        *(uint32_t *)address = color;
        AHardwareBuffer_unlock(buffer, NULL);
    }
    return queue_with(window, slot, settings);
}

static atomic_int dequeue_done;

static void *dequeue_flagged(void *window) {
    struct AHardwareBuffer *buffer = NULL;
    int32_t slot = -1;
    const int result = dequeue(window, &buffer, &slot);
    atomic_store(&dequeue_done, 1);
    return (void *)(intptr_t)result;
}

static void *dequeue_one(void *window) {
    struct AHardwareBuffer *buffer = NULL;
    int32_t slot = -1;
    return (void *)(intptr_t)dequeue(window, &buffer, &slot);
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (!found()) {
        printf("no private functions\n");
        return 1;
    }
    struct ANativeWindow *window = NULL;
    struct minato_BufferQueue *consumer = NULL;
    printf("create: %d\n", create(available, destroy, (void *)&destroyed, &window, &consumer));
    printf("size %d x %d, format %d\n", ANativeWindow_getWidth(window), ANativeWindow_getHeight(window),
           ANativeWindow_getFormat(window));
    set_default_size(consumer, 4, 2);
    ANativeWindow_Buffer buffer;
    printf("lock: %d, %d x %d\n", ANativeWindow_lock(window, &buffer, NULL), buffer.width, buffer.height);
    printf("unlockAndPost: %d, available %d\n", ANativeWindow_unlockAndPost(window), atomic_load(&available_calls));
    struct minato_BufferQueue_frame frame;
    printf("acquire: %d, timestamp positive %d\n", acquire(consumer, &frame), frame.timestamp > 0);
    printf("acquire with none queued: %d\n", acquire(consumer, &frame) == -EAGAIN);

    // A newer frame replaces one still queued.
    const int calls = atomic_load(&available_calls);
    post(window, 0xff0000ff);
    post(window, 0xff00ff00);
    printf("two frames: available calls %d\n", atomic_load(&available_calls) - calls);
    acquire(consumer, &frame);
    printf("acquired: %08x\n", first_pixel(frame.buffer));
    printf("acquire again: %d\n", acquire(consumer, &frame) == -EAGAIN);

    // A slot is queued once.
    struct AHardwareBuffer *dequeued = NULL;
    int32_t slot = -1;
    printf("dequeue: %d\n", dequeue(window, &dequeued, &slot));
    printf("queue: %d\n", queue(window, slot, 0));
    printf("queue again: %d\n", queue(window, slot, 0));
    printf("queue of the consumer's slot: %d\n", queue(window, frame.slot, 0));
    printf("dequeue for a cancel: %d\n", dequeue(window, &dequeued, &slot));
    printf("cancel: %d\n", cancel(window, slot));
    printf("cancel again: %d\n", cancel(window, slot));

    // A dequeue waits while every slot is busy: one acquired, one queued, one dequeued.
    printf("dequeue held: %d\n", dequeue(window, &dequeued, &slot));
    pthread_t waiter;
    pthread_create(&waiter, NULL, dequeue_one, window);
    usleep(100 * 1000);
    abandon(consumer);
    void *woken = NULL;
    pthread_join(waiter, &woken);
    printf("a waiting dequeue after abandon: %d\n", (int)(intptr_t)woken);
    printf("lock after abandon: %d\n", ANativeWindow_lock(window, &buffer, NULL));

    // The destroy: after both releases, not before.
    release(consumer);
    printf("destroyed after the consumer's release: %d\n", atomic_load(&destroyed));
    ANativeWindow_release(window);
    printf("destroyed after the window's: %d\n", atomic_load(&destroyed));

    // A window released while it is locked, the consumer first.
    atomic_store(&destroyed, 0);
    create(NULL, destroy, (void *)&destroyed, &window, &consumer);
    set_default_size(consumer, 2, 2);
    printf("lock: %d\n", ANativeWindow_lock(window, &buffer, NULL));
    printf("setFrameRate: %d\n",
           ANativeWindow_setFrameRate(window, 30.0f, ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_DEFAULT));
    release(consumer);
    ANativeWindow_release(window);
    printf("destroyed after releases while locked: %d\n", atomic_load(&destroyed));

    // An ImageReader's consumer (G3a): up to two frames held at once, each until its release, and its buffers' usage.
    if (set_consumer == NULL || set_default_format == NULL || release_slot == NULL || discard_free_buffers == NULL) {
        printf("no consumer functions\n");
        return 1;
    }
    create(NULL, NULL, NULL, &window, &consumer);
    printf("setConsumer of 0: %d, of 2: %d\n", set_consumer(consumer, 0, 0),
           set_consumer(consumer, 2, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN));
    set_default_format(consumer, AHARDWAREBUFFER_FORMAT_R5G6B5_UNORM);
    printf("default format: %d\n", ANativeWindow_getFormat(window));
    set_default_format(consumer, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM);
    set_default_size(consumer, 2, 2);
    struct minato_BufferQueue_frame first, second, third;
    post(window, 0xff0000ff);
    const int got_first = acquire(consumer, &first);
    post(window, 0xff00ff00);
    const int got_second = acquire(consumer, &second);
    printf("two held: %d %d, slots differ %d, first %08x, second %08x\n", got_first, got_second,
           first.slot != second.slot, first_pixel(first.buffer), first_pixel(second.buffer));
    post(window, 0xffff0000);
    printf("a third while two are held: %d\n", acquire(consumer, &third) == -EBUSY);
    release_slot(consumer, first.slot);
    printf("after a release: %d, %08x\n", acquire(consumer, &third), first_pixel(third.buffer));
    AHardwareBuffer_Desc description;
    AHardwareBuffer_describe(third.buffer, &description);
    printf("usage: CPU read %d, GPU sampled %d\n", (description.usage & AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN) != 0,
           (description.usage & AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE) != 0);
    release_slot(consumer, second.slot);
    release_slot(consumer, third.slot);
    discard_free_buffers(consumer);
    post(window, 0xff123456);
    printf("after discarding: %d, %08x\n", acquire(consumer, &first), first_pixel(first.buffer));
    release(consumer);
    ANativeWindow_release(window);

    // An ImageWriter as the producer (G3b) of a reader holding one frame: one connection, frames kept, releases.
    if (detach == NULL || connect_producer == NULL || disconnect_producer == NULL || queue_with == NULL ||
        attach_and_queue == NULL) {
        printf("no producer functions\n");
        return 1;
    }
    create(NULL, NULL, NULL, &window, &consumer);
    set_consumer(consumer, 1, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN);
    set_default_size(consumer, 2, 2);
    printf("connect: %d, again: %d\n", connect_producer(window, 1, released, NULL),
           connect_producer(window, 1, released, NULL));
    struct minato_BufferQueue_settings settings = {5, 0, 7, 0, 0, 1, 2};
    write_frame(window, 0xff0000ff, &settings);
    settings.timestamp = 6;
    write_frame(window, 0xff00ff00, &settings);
    printf("two queued: first %d, %08x, timestamp %lld, data space %d, crop %d %d %d %d\n", acquire(consumer, &first),
           first_pixel(first.buffer), (long long)first.timestamp, first.data_space, first.crop_left, first.crop_top,
           first.crop_right, first.crop_bottom);
    printf("released before a release: %d\n", atomic_load(&released_calls));
    release_slot(consumer, first.slot);
    printf("after a release: %d, then the second %d, %08x\n", atomic_load(&released_calls),
           acquire(consumer, &second), first_pixel(second.buffer));
    // Two slots, the writer's one and the reader's: one more frame queued, then a dequeue waits for a release.
    write_frame(window, 0xffff0000, &settings);
    pthread_t writer;
    pthread_create(&writer, NULL, dequeue_flagged, window);
    usleep(200000);
    printf("a dequeue with every slot out waits: %d\n", !atomic_load(&dequeue_done));
    release_slot(consumer, second.slot);
    void *waited = NULL;
    pthread_join(writer, &waited);
    printf("after the reader's release: %d\n", (int)(intptr_t)waited);
    // That dequeued slot, cancelled: no release callback.
    const int before_cancel = atomic_load(&released_calls);
    for (int32_t slot = 0; slot < 64; slot++) {
        if (cancel(window, slot) == 0) {
            break;
        }
    }
    printf("released after a cancel: %d\n", atomic_load(&released_calls) - before_cancel);
    acquire(consumer, &third);
    release_slot(consumer, third.slot);

    // A reader's acquired frame detached from its queue and attached to the writer's, which it leaves at the release.
    struct ANativeWindow *source_window = NULL;
    struct minato_BufferQueue *source = NULL;
    create(NULL, NULL, NULL, &source_window, &source);
    set_consumer(source, 1, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN);
    set_default_size(source, 2, 2);
    post(source_window, 0xffffff00);
    struct minato_BufferQueue_frame detached;
    acquire(source, &detached);
    AHardwareBuffer_acquire(detached.buffer);  // the image's own reference
    printf("detach: %d, again: %d, a slot never acquired: %d\n", detach(source, detached.slot),
           detach(source, detached.slot), detach(source, 63));
    post(source_window, 0xff00ffff);
    struct minato_BufferQueue_frame next;
    printf("the source after the detach: %d, %08x\n", acquire(source, &next), first_pixel(next.buffer));
    settings.timestamp = 9;
    const int before_attach = atomic_load(&released_calls);
    printf("attachAndQueue: %d\n", attach_and_queue(window, detached.buffer, &settings));
    struct minato_BufferQueue_frame attached;
    printf("attached: %d, the same buffer %d, %08x, timestamp %lld\n", acquire(consumer, &attached),
           attached.buffer == detached.buffer, first_pixel(attached.buffer), (long long)attached.timestamp);
    release_slot(consumer, attached.slot);
    printf("released after its release: %d\n", atomic_load(&released_calls) - before_attach);
    AHardwareBuffer_release(detached.buffer);
    release_slot(source, next.slot);
    release(source);
    ANativeWindow_release(source_window);

    // After the disconnect: no callback, and frames drop again; another writer may connect.
    disconnect_producer(window);
    const int before_disconnect = atomic_load(&released_calls);
    write_frame(window, 0xff000001, &settings);
    acquire(consumer, &first);
    release_slot(consumer, first.slot);
    printf("released after the disconnect: %d, connect again: %d\n",
           atomic_load(&released_calls) - before_disconnect, connect_producer(window, 1, released, NULL));
    disconnect_producer(window);
    release(consumer);
    ANativeWindow_release(window);
    printf("done\n");
    return 0;
}
