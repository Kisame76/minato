// AInputQueue of libandroid.so (ADR-0028, decision 9): the queue activity-host hands the app at
// INPUT_QUEUE_CREATED, of the input channel ADR-0036 brings with M3 step 6. Until then a queue has no
// events and nothing to watch: attaching it to a looper only records the looper, and native_app_glue's
// LOOPER_ID_INPUT never comes. getEvent and the others stay generated stubs until step 6.
#include <android/input.h>
#include <stdint.h>
#include <stdlib.h>

#include "APKRuntime/guest_interface.h"

struct AInputQueue {
    uint64_t channel;
    ALooper *looper;
};

extern "C" {

AInputQueue *minato_AInputQueue_create(uint64_t channel) {
    auto *queue = static_cast<AInputQueue *>(calloc(1, sizeof(AInputQueue)));
    if (queue != nullptr) queue->channel = channel;
    return queue;
}

void minato_AInputQueue_delete(AInputQueue *queue) {
    free(queue);
}

void AInputQueue_attachLooper(AInputQueue *queue, ALooper *looper, int, ALooper_callbackFunc, void *) {
    if (queue != nullptr) queue->looper = looper;
}

void AInputQueue_detachLooper(AInputQueue *queue) {
    if (queue != nullptr) queue->looper = nullptr;
}

}  // extern "C"
