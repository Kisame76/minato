// AInputQueue of libandroid.so (ADR-0036, decision 3), as InputQueue.cpp keeps NativeActivity's: the queue
// activity-host hands the app at INPUT_QUEUE_CREATED, on the input channel whose ID that event carries
// (minato_AInputQueue_create). The channel's descriptor is readable while it holds an event, as InputQueue.cpp's
// wake byte: attachLooper has each looper watch it, getEvent takes the oldest event (input_read), and taking the
// last ends the readiness (pop and drain). A key (ADR-0068 step 2e) comes twice, as NativeActivity's queue gives it
// on Android 15 (native-ime-test on the AVD): first with AKEY_EVENT_FLAG_PREDISPATCH, ViewRootImpl's stage before the
// input method's, where preDispatchEvent finishes it unhandled and answers 1; a predispatched key finished unhandled
// comes again without the flag, and one finished as handled does not, as handled ends ViewRootImpl's stages. The
// second delivery waits in the queue ahead of the channel's next record, and an eventfd that each attached looper
// watches beside the channel stays readable while one waits, as InputQueue.cpp's wake byte is written for it.
// Texts go nowhere, since a NativeActivity's view has no editor and Android 15 serves none; devices go nowhere
// either. finishEvent frees the event. A queue of channel 0, or of a channel it cannot connect to, has no events.
// A local queue (ADR-0069 step 3), NativeActivity's on the Java path, has no channel: libandroid_runtime's InputQueue
// natives post its keys and motion events in the process (minato_AInputQueue_postKey and _postMotion), with
// ViewRootImpl's predispatch on a key it sends before the input method's stage, and each finish goes back to them.
#include <android/input.h>
#include <android/log.h>
#include <android/looper.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "APKRuntime/guest_interface.h"
#include "internal.h"

struct AInputQueue {
    int fd;              // the channel's descriptor, -1 for a queue without events or a local one
    int wake;            // readable while a key waits for its second delivery or a local event waits, -1 without
    minato_input_finished finished;  // a local queue's (ADR-0069 step 3), NULL for a channel's
    void *context;
    ALooper **loopers;   // those attached, each once
    size_t looper_count;
    pthread_mutex_t lock;     // for the waiting keys, which any thread may finish
    AInputEvent *waiting;     // the keys after their predispatch, oldest first
    AInputEvent **waiting_end;
};

namespace {

/// The oldest key that waits for its second delivery, or NULL; the wake ends with the last.
AInputEvent *take_waiting(AInputQueue *queue) {
    pthread_mutex_lock(&queue->lock);
    AInputEvent *event = queue->waiting;
    if (event != nullptr) {
        queue->waiting = event->next;
        if (queue->waiting == nullptr) {
            queue->waiting_end = &queue->waiting;
            uint64_t count;
            (void)read(queue->wake, &count, sizeof count);
        }
        event->next = nullptr;
    }
    pthread_mutex_unlock(&queue->lock);
    return event;
}

}  // namespace

extern "C" {

AInputQueue *minato_AInputQueue_create(uint64_t channel) {
    auto *queue = static_cast<AInputQueue *>(calloc(1, sizeof(AInputQueue)));
    if (queue == nullptr) return nullptr;
    queue->fd = -1;
    queue->wake = -1;
    pthread_mutex_init(&queue->lock, nullptr);
    queue->waiting_end = &queue->waiting;
    if (channel == 0) return queue;
    const long fd = syscall(APKRT_NR_INPUT_CONNECT, channel);
    const int wake = fd >= 0 ? eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK) : -1;
    if (fd >= 0 && wake >= 0) {
        queue->fd = static_cast<int>(fd);
        queue->wake = wake;
    } else if (fd >= 0) {
        __android_log_print(ANDROID_LOG_WARN, "libandroid", "input channel %llu: eventfd: %s; the queue has no events",
                            static_cast<unsigned long long>(channel), strerror(errno));
        close(static_cast<int>(fd));
    } else {
        __android_log_print(ANDROID_LOG_WARN, "libandroid", "input channel %llu: %s; the queue has no events",
                            static_cast<unsigned long long>(channel), strerror(errno));
    }
    return queue;
}

/// A local queue (ADR-0069 step 3): no channel, its events posted in the process, each finish told to `finished`
/// with the event's ID, as Android's InputQueue.cpp tells NativeActivity's Java InputQueue. The predispatch is the
/// poster's (AKEY_EVENT_FLAG_PREDISPATCH on the key it posts), so a key comes once per post.
AInputQueue *minato_AInputQueue_createLocal(minato_input_finished finished, void *context) {
    auto *queue = static_cast<AInputQueue *>(calloc(1, sizeof(AInputQueue)));
    if (queue == nullptr) return nullptr;
    queue->fd = -1;
    queue->wake = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    queue->finished = finished;
    queue->context = context;
    pthread_mutex_init(&queue->lock, nullptr);
    queue->waiting_end = &queue->waiting;
    if (queue->wake < 0) {
        __android_log_print(ANDROID_LOG_WARN, "libandroid", "local input queue: eventfd: %s; it has no events",
                            strerror(errno));
    }
    return queue;
}

/// A key or a motion event at the end of a local queue, with the poster's ID; false without memory or a wake.
static bool post_local(AInputQueue *queue, const void *record, size_t size, uint64_t id) {
    if (queue->wake < 0) return false;
    auto *event = static_cast<AInputEvent *>(calloc(1, sizeof(AInputEvent)));
    if (event == nullptr) return false;
    memcpy(&event->motion, record, size);
    event->id = id;
    pthread_mutex_lock(&queue->lock);
    *queue->waiting_end = event;
    queue->waiting_end = &event->next;
    const uint64_t one = 1;
    (void)write(queue->wake, &one, sizeof one);
    pthread_mutex_unlock(&queue->lock);
    return true;
}

bool minato_AInputQueue_postKey(AInputQueue *queue, const struct apkrt_key_event *key, uint64_t id) {
    return post_local(queue, key, sizeof *key, id);
}

bool minato_AInputQueue_postMotion(AInputQueue *queue, const struct apkrt_motion_event *motion, uint64_t id) {
    return post_local(queue, motion, sizeof *motion, id);
}

/// Once onInputQueueDestroyed has returned, which detaches the loopers, as Android's queue expects: closing the
/// descriptor ends the channel.
void minato_AInputQueue_delete(AInputQueue *queue) {
    if (queue == nullptr) return;
    // The waiting keys first: taking the last reads the wake, which must still be open.
    while (AInputEvent *event = take_waiting(queue)) free(event);
    if (queue->fd >= 0) close(queue->fd);
    if (queue->wake >= 0) close(queue->wake);
    pthread_mutex_destroy(&queue->lock);
    free(queue->loopers);
    free(queue);
}

void AInputQueue_attachLooper(AInputQueue *queue, ALooper *looper, int ident, ALooper_callbackFunc callback,
                              void *data) {
    for (size_t index = 0; index < queue->looper_count; ++index) {
        if (queue->loopers[index] == looper) return;
    }
    auto *grown = static_cast<ALooper **>(realloc(queue->loopers, (queue->looper_count + 1) * sizeof(ALooper *)));
    if (grown == nullptr) __android_log_assert(nullptr, "InputQueue", "Out of memory");
    queue->loopers = grown;
    queue->loopers[queue->looper_count++] = looper;
    if (queue->fd >= 0) ALooper_addFd(looper, queue->fd, ident, ALOOPER_EVENT_INPUT, callback, data);
    if (queue->wake >= 0) ALooper_addFd(looper, queue->wake, ident, ALOOPER_EVENT_INPUT, callback, data);
}

void AInputQueue_detachLooper(AInputQueue *queue) {
    for (size_t index = 0; index < queue->looper_count; ++index) {
        if (queue->fd >= 0) ALooper_removeFd(queue->loopers[index], queue->fd);
        if (queue->wake >= 0) ALooper_removeFd(queue->loopers[index], queue->wake);
    }
    queue->looper_count = 0;
}

/// 1 or 0, and a negative errno when the channel cannot be asked, as input.h allows.
int32_t AInputQueue_hasEvents(AInputQueue *queue) {
    pthread_mutex_lock(&queue->lock);
    const bool waiting = queue->waiting != nullptr;
    pthread_mutex_unlock(&queue->lock);
    if (waiting) return 1;
    if (queue->fd < 0) return 0;
    struct pollfd entry = {queue->fd, POLLIN, 0};
    const int ready = poll(&entry, 1, 0);
    if (ready < 0) return -errno;
    return ready == 1 && (entry.revents & POLLIN) != 0 ? 1 : 0;
}

/// 0 and the oldest event, or WOULD_BLOCK (-EWOULDBLOCK) and NULL once the queue is empty.
int32_t AInputQueue_getEvent(AInputQueue *queue, AInputEvent **outEvent) {
    *outEvent = nullptr;
    if (queue->fd < 0 && queue->finished == nullptr) return -EWOULDBLOCK;
    if ((*outEvent = take_waiting(queue)) != nullptr) return 0;
    if (queue->fd < 0) return -EWOULDBLOCK;
    auto *event = static_cast<AInputEvent *>(malloc(sizeof(AInputEvent)));
    if (event == nullptr) return -ENOMEM;  // NO_MEMORY
    // Texts and devices (ADR-0068) are taken and dropped.
    do {
        if (syscall(APKRT_NR_INPUT_READ, queue->fd, &event->motion) != 0) {
            free(event);
            return -EWOULDBLOCK;
        }
    } while (event->motion.kind != APKRT_INPUT_MOTION && event->motion.kind != APKRT_INPUT_KEY);
    event->next = nullptr;
    if (event->motion.kind == APKRT_INPUT_KEY) event->key.flags |= MINATO_KEY_FLAG_PREDISPATCH;
    *outEvent = event;
    return 0;
}

int32_t AInputQueue_preDispatchEvent(AInputQueue *queue, AInputEvent *event) {
    if (event->motion.kind != APKRT_INPUT_KEY || (event->key.flags & MINATO_KEY_FLAG_PREDISPATCH) == 0) return 0;
    AInputQueue_finishEvent(queue, event, 0);
    return 1;
}

void AInputQueue_finishEvent(AInputQueue *queue, AInputEvent *event, int handled) {
    if (queue->finished != nullptr) {
        queue->finished(queue->context, event->id, handled);
        free(event);
        return;
    }
    if (event->motion.kind == APKRT_INPUT_KEY && (event->key.flags & MINATO_KEY_FLAG_PREDISPATCH) != 0 && !handled) {
        event->key.flags &= ~MINATO_KEY_FLAG_PREDISPATCH;
        pthread_mutex_lock(&queue->lock);
        *queue->waiting_end = event;
        queue->waiting_end = &event->next;
        const uint64_t one = 1;
        (void)write(queue->wake, &one, sizeof one);
        pthread_mutex_unlock(&queue->lock);
        return;
    }
    free(event);
}

}  // extern "C"
