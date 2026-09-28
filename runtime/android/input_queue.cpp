// AInputQueue of libandroid.so (ADR-0036, decision 3), as InputQueue.cpp keeps NativeActivity's: the queue
// activity-host hands the app at INPUT_QUEUE_CREATED, on the input channel whose ID that event carries
// (minato_AInputQueue_create). The channel's descriptor is readable while it holds an event, as InputQueue.cpp's
// wake byte: attachLooper has each looper watch it, getEvent takes the oldest event (input_read), and taking the
// last ends the readiness (pop and drain). preDispatchEvent passes every event on, since only key events stop
// there and M3 has none; finishEvent frees the event. A queue of channel 0, or of a channel it cannot connect
// to, has no events.
#include <android/input.h>
#include <android/log.h>
#include <android/looper.h>
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "APKRuntime/guest_interface.h"
#include "internal.h"

struct AInputQueue {
    int fd;              // the channel's descriptor, -1 for a queue without events
    ALooper **loopers;   // those attached, each once
    size_t looper_count;
};

extern "C" {

AInputQueue *minato_AInputQueue_create(uint64_t channel) {
    auto *queue = static_cast<AInputQueue *>(calloc(1, sizeof(AInputQueue)));
    if (queue == nullptr) return nullptr;
    queue->fd = -1;
    if (channel == 0) return queue;
    const long fd = syscall(APKRT_NR_INPUT_CONNECT, channel);
    if (fd >= 0) {
        queue->fd = static_cast<int>(fd);
    } else {
        __android_log_print(ANDROID_LOG_WARN, "libandroid", "input channel %llu: %s; the queue has no events",
                            static_cast<unsigned long long>(channel), strerror(errno));
    }
    return queue;
}

/// Once onInputQueueDestroyed has returned, which detaches the loopers, as Android's queue expects: closing the
/// descriptor ends the channel.
void minato_AInputQueue_delete(AInputQueue *queue) {
    if (queue == nullptr) return;
    if (queue->fd >= 0) close(queue->fd);
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
}

void AInputQueue_detachLooper(AInputQueue *queue) {
    for (size_t index = 0; index < queue->looper_count; ++index) {
        if (queue->fd >= 0) ALooper_removeFd(queue->loopers[index], queue->fd);
    }
    queue->looper_count = 0;
}

/// 1 or 0, and a negative errno when the channel cannot be asked, as input.h allows.
int32_t AInputQueue_hasEvents(AInputQueue *queue) {
    if (queue->fd < 0) return 0;
    struct pollfd entry = {queue->fd, POLLIN, 0};
    const int ready = poll(&entry, 1, 0);
    if (ready < 0) return -errno;
    return ready == 1 && (entry.revents & POLLIN) != 0 ? 1 : 0;
}

/// 0 and the oldest event, or WOULD_BLOCK (-EWOULDBLOCK) and NULL once the queue is empty.
int32_t AInputQueue_getEvent(AInputQueue *queue, AInputEvent **outEvent) {
    *outEvent = nullptr;
    if (queue->fd < 0) return -EWOULDBLOCK;
    auto *event = static_cast<AInputEvent *>(malloc(sizeof(AInputEvent)));
    if (event == nullptr) return -ENOMEM;  // NO_MEMORY
    if (syscall(APKRT_NR_INPUT_READ, queue->fd, &event->motion) != 0) {
        free(event);
        return -EWOULDBLOCK;
    }
    *outEvent = event;
    return 0;
}

int32_t AInputQueue_preDispatchEvent(AInputQueue *, AInputEvent *) {
    return 0;
}

void AInputQueue_finishEvent(AInputQueue *, AInputEvent *event, int) {
    free(event);
}

}  // extern "C"
