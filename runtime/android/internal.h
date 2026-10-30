// What the parts of libandroid.so share and do not export (its version script keeps them local).
#ifndef MINATO_ANDROID_INTERNAL_H
#define MINATO_ANDROID_INTERNAL_H

#include <android/configuration.h>
#include <android/looper.h>
#include <stdint.h>

#include "APKRuntime/guest_interface.h"

/// Sets every field of `config` to the value ADR-0028's decision 9 chooses from the session's configuration.
void minato_configuration_from_session(AConfiguration *config, const struct apkrt_configuration *session);

/// A message for `looper`, as Android's Looper::sendMessageAtTime: its poll calls `handler` with `data` on the
/// looper's thread once CLOCK_MONOTONIC reaches `uptime` (ns), before the callbacks of its descriptors; messages
/// run in the order of their times, and in the order sent for the same time. From any thread.
void minato_looper_send(ALooper *looper, int64_t uptime, void (*handler)(void *data), void *data);

/// Takes the messages with `data` that have not run out of `looper`.
void minato_looper_remove(ALooper *looper, void *data);

/// An event of an input queue (input_queue.cpp), which input.cpp's getters read: the record input_read gives
/// (ADR-0036, decision 3), a motion event or, since ADR-0068 step 2e, a key, as its kind says.
struct AInputEvent {
    union {
        struct apkrt_motion_event motion;
        struct apkrt_key_event key;
    };
    struct AInputEvent *next;  // the queue's, while a key waits for its delivery after the predispatch
    uint64_t id;               // a local queue's (ADR-0069 step 3): its poster's ID, which its finish answers
};

/// A queue without a channel, filled in the process (ADR-0069 step 3): NativeActivity's InputQueue on the Java path,
/// whose natives post its events and hear of each finish, on whatever thread finishes it, through `finished`.
typedef void (*minato_input_finished)(void *context, uint64_t id, int handled);

/// Android's AKEY_EVENT_FLAG_PREDISPATCH (Input.h), which the NDK does not name: a key's first delivery, before the
/// input method's stage.
#define MINATO_KEY_FLAG_PREDISPATCH 0x20000000

#endif
