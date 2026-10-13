/* ADR-0058 c): buffer-queue-test, libnativewindow's buffer queue (APKRuntime/buffer_queue.h) through its private
   functions, as libandroid_runtime's SurfaceTexture and libEGL reach them, on graphics/host's hardware buffers:
   - a queue window's size and lock before and after setDefaultSize, a post, the available callback, an acquire;
   - first in, first out over two frames; a second queue of the same slot, which only a dequeued slot takes;
   - a dequeue that waits for a free slot and wakes with -ENODEV when the queue is abandoned;
   - the context's destroy, which comes once both the window and the consumer let the queue go, and not before;
     also for a window released while it is locked.
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

static atomic_int available_calls;
static atomic_int destroyed;

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
#undef FIND
    return create != NULL && set_default_size != NULL && acquire != NULL && abandon != NULL && release != NULL &&
           dequeue != NULL && queue != NULL;
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

    // First in, first out.
    post(window, 0xff0000ff);
    post(window, 0xff00ff00);
    acquire(consumer, &frame);
    printf("first: %08x\n", first_pixel(frame.buffer));
    acquire(consumer, &frame);
    printf("second: %08x\n", first_pixel(frame.buffer));

    // A slot is queued once.
    struct AHardwareBuffer *dequeued = NULL;
    int32_t slot = -1;
    printf("dequeue: %d\n", dequeue(window, &dequeued, &slot));
    printf("queue: %d\n", queue(window, slot, 0));
    printf("queue again: %d\n", queue(window, slot, 0));
    printf("queue of the consumer's slot: %d\n", queue(window, frame.slot, 0));

    // A dequeue waits for a free slot: one acquired, two queued.
    post(window, 0xffff0000);
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
    printf("done\n");
    return 0;
}
