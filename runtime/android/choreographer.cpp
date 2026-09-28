// AChoreographer of libandroid.so (ADR-0036, decision 2), written for Minato from the NDK's documentation and
// Android 15's answers (fixtures/apps/choreographer-test), in the way AOSP's Choreographer.cpp works. A thread
// with a looper has one, whose vsync channel (guest_interface.h) sits in that looper. A callback that is due
// asks for the next tick; a delayed one waits as a looper message (internal.h) first; one posted from another
// thread asks through a message as well. At a tick every callback that is due runs with the tick's time, in
// the order of the times they were due, so one posted inside runs a tick later. A refresh-rate callback runs
// once after its registration, through a message, and then whenever the period the channel gives changes.
#include <android/choreographer.h>
#include <android/log.h>
#include <android/looper.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "APKRuntime/guest_interface.h"
#include "internal.h"

namespace {

#define CHOREOGRAPHER_LOG(priority, ...) __android_log_print(ANDROID_LOG_##priority, "Choreographer", __VA_ARGS__)

struct FrameCallback {
    AChoreographer_frameCallback callback;
    AChoreographer_frameCallback64 callback64;
    AChoreographer_vsyncCallback vsync_callback;
    void* data;
    int64_t due;  // CLOCK_MONOTONIC in ns
};

struct RefreshRateCallback {
    AChoreographer_refreshRateCallback callback;
    void* data;
    bool first_call_made;
};

[[noreturn]] void fatal(const char* what) {
    __android_log_assert(nullptr, "Choreographer", "%s", what);
}

int64_t now() {
    timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return time.tv_sec * 1'000'000'000LL + time.tv_nsec;
}

// Room for one more of `count` items in `*items`, growing it.
template <typename Item>
void reserve(Item** items, size_t count, size_t* capacity) {
    if (count < *capacity) return;
    const size_t grown_capacity = *capacity == 0 ? 8 : *capacity * 2;
    auto* grown = static_cast<Item*>(realloc(*items, grown_capacity * sizeof(Item)));
    if (grown == nullptr) fatal("Out of memory");
    *items = grown;
    *capacity = grown_capacity;
}

}  // namespace

struct AChoreographer {
    ALooper* looper;  // acquired
    pthread_t thread;
    int channel;           // the vsync channel's descriptor
    pthread_mutex_t lock;  // for what other threads post and register
    FrameCallback* callbacks;  // by the time they are due, then in the order posted
    size_t callback_count;
    size_t callback_capacity;
    RefreshRateCallback* refresh_callbacks;
    size_t refresh_count;
    size_t refresh_capacity;
    // The looper's thread alone.
    bool waiting;            // for the tick it asked for
    int64_t channel_period;  // the period the channel gave last
    int64_t latest_period;   // the period the refresh-rate callbacks were told last, -1 before
    bool in_callback;        // a vsync callback runs, whose data is valid meanwhile
};

struct AChoreographerFrameCallbackData {
    AChoreographer* choreographer;
    apkrt_vsync vsync;
};

namespace {

pthread_key_t instance_key;
pthread_once_t instance_key_once = PTHREAD_ONCE_INIT;

bool on_own_thread(const AChoreographer* choreographer) {
    return pthread_equal(pthread_self(), choreographer->thread) != 0;
}

// Asks for the next tick, once until it comes (DisplayEventDispatcher::scheduleVsync).
void schedule_vsync(AChoreographer* choreographer) {
    if (choreographer->waiting) return;
    if (syscall(APKRT_NR_VSYNC_REQUEST, choreographer->channel) != 0) {
        CHOREOGRAPHER_LOG(ERROR, "Failed to request the next vsync: %s", strerror(errno));
        return;
    }
    choreographer->waiting = true;
}

void on_schedule_vsync(void* data) {
    schedule_vsync(static_cast<AChoreographer*>(data));
}

// A delayed callback's time came: a tick, if the first callback is due by now.
void on_schedule_callbacks(void* data) {
    auto* choreographer = static_cast<AChoreographer*>(data);
    pthread_mutex_lock(&choreographer->lock);
    const bool due = choreographer->callback_count > 0 && choreographer->callbacks[0].due <= now();
    pthread_mutex_unlock(&choreographer->lock);
    if (due) schedule_vsync(choreographer);
}

// Choreographer::handleRefreshRateUpdates with the period `pending`: every callback that was not called yet,
// and all of them when the period changed.
void refresh_rate_updates(AChoreographer* choreographer, int64_t pending) {
    const int64_t last = choreographer->latest_period;
    if (pending > 0) choreographer->latest_period = pending;
    const bool changed = pending > 0 && pending != last;
    pthread_mutex_lock(&choreographer->lock);
    size_t count = 0;
    for (size_t index = 0; index < choreographer->refresh_count; ++index) {
        if (changed || !choreographer->refresh_callbacks[index].first_call_made) ++count;
    }
    auto* calls = count > 0 ? static_cast<RefreshRateCallback*>(malloc(count * sizeof(RefreshRateCallback))) : nullptr;
    if (count > 0 && calls == nullptr) fatal("Out of memory");
    count = 0;
    for (size_t index = 0; index < choreographer->refresh_count; ++index) {
        RefreshRateCallback& callback = choreographer->refresh_callbacks[index];
        if (changed || !callback.first_call_made) calls[count++] = callback;
        callback.first_call_made = true;
    }
    pthread_mutex_unlock(&choreographer->lock);
    for (size_t index = 0; index < count; ++index) calls[index].callback(pending, calls[index].data);
    free(calls);
}

void on_refresh_rate_updates(void* data) {
    auto* choreographer = static_cast<AChoreographer*>(data);
    refresh_rate_updates(choreographer, choreographer->channel_period);
}

// A tick: every callback due by now runs with its time.
void dispatch_vsync(AChoreographer* choreographer, const apkrt_vsync& vsync) {
    const int64_t time = now();
    pthread_mutex_lock(&choreographer->lock);
    size_t due = 0;
    while (due < choreographer->callback_count && choreographer->callbacks[due].due < time) ++due;
    auto* callbacks = due > 0 ? static_cast<FrameCallback*>(malloc(due * sizeof(FrameCallback))) : nullptr;
    if (due > 0 && callbacks == nullptr) fatal("Out of memory");
    if (due > 0) {
        memcpy(callbacks, choreographer->callbacks, due * sizeof(FrameCallback));
        choreographer->callback_count -= due;
        memmove(choreographer->callbacks, choreographer->callbacks + due,
                choreographer->callback_count * sizeof(FrameCallback));
    }
    pthread_mutex_unlock(&choreographer->lock);
    const AChoreographerFrameCallbackData data{choreographer, vsync};
    for (size_t index = 0; index < due; ++index) {
        const FrameCallback& callback = callbacks[index];
        if (callback.vsync_callback != nullptr) {
            choreographer->in_callback = true;
            callback.vsync_callback(&data, callback.data);
            choreographer->in_callback = false;
        } else if (callback.callback64 != nullptr) {
            callback.callback64(vsync.time, callback.data);
        } else {
            callback.callback(static_cast<long>(vsync.time), callback.data);
        }
    }
    free(callbacks);
}

// The channel is readable: a tick it asked for, or a new period.
int on_vsync(int, int, void* data) {
    auto* choreographer = static_cast<AChoreographer*>(data);
    apkrt_vsync vsync;
    const long got = syscall(APKRT_NR_VSYNC_READ, choreographer->channel, &vsync);
    if (got < 0) {
        CHOREOGRAPHER_LOG(ERROR, "Failed to read the vsync channel: %s", strerror(errno));
        return 1;
    }
    choreographer->channel_period = vsync.period;
    refresh_rate_updates(choreographer, vsync.period);
    if (got == 1) {
        choreographer->waiting = false;
        dispatch_vsync(choreographer, vsync);
    }
    return 1;
}

void post(AChoreographer* choreographer, AChoreographer_frameCallback callback,
          AChoreographer_frameCallback64 callback64, AChoreographer_vsyncCallback vsync_callback, void* data,
          int64_t delay) {
    const int64_t time = now();
    const FrameCallback posted{callback, callback64, vsync_callback, data, time + delay};
    pthread_mutex_lock(&choreographer->lock);
    reserve(&choreographer->callbacks, choreographer->callback_count, &choreographer->callback_capacity);
    size_t index = choreographer->callback_count;
    while (index > 0 && choreographer->callbacks[index - 1].due > posted.due) --index;
    memmove(choreographer->callbacks + index + 1, choreographer->callbacks + index,
            (choreographer->callback_count - index) * sizeof(FrameCallback));
    choreographer->callbacks[index] = posted;
    ++choreographer->callback_count;
    pthread_mutex_unlock(&choreographer->lock);
    if (posted.due > time) {
        minato_looper_send(choreographer->looper, posted.due, on_schedule_callbacks, choreographer);
    } else if (on_own_thread(choreographer)) {
        schedule_vsync(choreographer);
    } else {
        minato_looper_send(choreographer->looper, time, on_schedule_vsync, choreographer);
    }
}

// The end of the thread whose choreographer it is.
void destroy(void* instance) {
    auto* choreographer = static_cast<AChoreographer*>(instance);
    ALooper_removeFd(choreographer->looper, choreographer->channel);
    minato_looper_remove(choreographer->looper, choreographer);
    close(choreographer->channel);
    ALooper_release(choreographer->looper);
    pthread_mutex_destroy(&choreographer->lock);
    free(choreographer->callbacks);
    free(choreographer->refresh_callbacks);
    free(choreographer);
}

void make_instance_key() {
    errno = pthread_key_create(&instance_key, destroy);
    if (errno != 0) fatal("Could not allocate TLS key");
}

const AChoreographerFrameCallbackData* valid(const AChoreographerFrameCallbackData* data) {
    if (!data->choreographer->in_callback) fatal("Data is only valid in callback");
    return data;
}

const apkrt_frame_timeline& timeline(const AChoreographerFrameCallbackData* data, size_t index) {
    if (index >= APKRT_FRAME_TIMELINES) fatal("Error: index out of bounds");
    return valid(data)->vsync.timelines[index];
}

}  // namespace

extern "C" {

AChoreographer* AChoreographer_getInstance() {
    pthread_once(&instance_key_once, make_instance_key);
    auto* choreographer = static_cast<AChoreographer*>(pthread_getspecific(instance_key));
    if (choreographer != nullptr) return choreographer;
    ALooper* looper = ALooper_forThread();
    if (looper == nullptr) {
        CHOREOGRAPHER_LOG(WARN, "No looper prepared for thread");
        return nullptr;
    }
    const long channel = syscall(APKRT_NR_VSYNC_CONNECT);
    if (channel < 0) {
        CHOREOGRAPHER_LOG(ERROR, "Failed to connect the vsync channel: %s", strerror(errno));
        return nullptr;
    }
    choreographer = static_cast<AChoreographer*>(calloc(1, sizeof(AChoreographer)));
    if (choreographer == nullptr) {
        close(static_cast<int>(channel));
        return nullptr;
    }
    ALooper_acquire(looper);
    choreographer->looper = looper;
    choreographer->thread = pthread_self();
    choreographer->channel = static_cast<int>(channel);
    pthread_mutex_init(&choreographer->lock, nullptr);
    choreographer->latest_period = -1;
    apkrt_vsync vsync;
    if (syscall(APKRT_NR_VSYNC_READ, choreographer->channel, &vsync) >= 0) choreographer->channel_period = vsync.period;
    if (ALooper_addFd(looper, choreographer->channel, ALOOPER_POLL_CALLBACK, ALOOPER_EVENT_INPUT, on_vsync,
                      choreographer) != 1) {
        destroy(choreographer);
        return nullptr;
    }
    pthread_setspecific(instance_key, choreographer);
    return choreographer;
}

void AChoreographer_postFrameCallback(AChoreographer* choreographer, AChoreographer_frameCallback callback, void* data) {
    post(choreographer, callback, nullptr, nullptr, data, 0);
}

void AChoreographer_postFrameCallbackDelayed(AChoreographer* choreographer, AChoreographer_frameCallback callback,
                                             void* data, long delayMillis) {
    post(choreographer, callback, nullptr, nullptr, data, delayMillis * 1'000'000LL);
}

void AChoreographer_postFrameCallback64(AChoreographer* choreographer, AChoreographer_frameCallback64 callback,
                                        void* data) {
    post(choreographer, nullptr, callback, nullptr, data, 0);
}

void AChoreographer_postFrameCallbackDelayed64(AChoreographer* choreographer, AChoreographer_frameCallback64 callback,
                                               void* data, uint32_t delayMillis) {
    post(choreographer, nullptr, callback, nullptr, data, delayMillis * 1'000'000LL);
}

void AChoreographer_postVsyncCallback(AChoreographer* choreographer, AChoreographer_vsyncCallback callback,
                                      void* data) {
    post(choreographer, nullptr, nullptr, callback, data, 0);
}

// The same callback with the same data is taken once.
void AChoreographer_registerRefreshRateCallback(AChoreographer* choreographer,
                                                AChoreographer_refreshRateCallback callback, void* data) {
    pthread_mutex_lock(&choreographer->lock);
    for (size_t index = 0; index < choreographer->refresh_count; ++index) {
        const RefreshRateCallback& registered = choreographer->refresh_callbacks[index];
        if (registered.callback == callback && registered.data == data) {
            pthread_mutex_unlock(&choreographer->lock);
            return;
        }
    }
    reserve(&choreographer->refresh_callbacks, choreographer->refresh_count, &choreographer->refresh_capacity);
    choreographer->refresh_callbacks[choreographer->refresh_count++] = {callback, data, false};
    pthread_mutex_unlock(&choreographer->lock);
    minato_looper_send(choreographer->looper, now(), on_refresh_rate_updates, choreographer);
}

void AChoreographer_unregisterRefreshRateCallback(AChoreographer* choreographer,
                                                  AChoreographer_refreshRateCallback callback, void* data) {
    pthread_mutex_lock(&choreographer->lock);
    size_t kept = 0;
    for (size_t index = 0; index < choreographer->refresh_count; ++index) {
        const RefreshRateCallback& registered = choreographer->refresh_callbacks[index];
        if (registered.callback != callback || registered.data != data) {
            choreographer->refresh_callbacks[kept++] = registered;
        }
    }
    choreographer->refresh_count = kept;
    pthread_mutex_unlock(&choreographer->lock);
}

int64_t AChoreographerFrameCallbackData_getFrameTimeNanos(const AChoreographerFrameCallbackData* data) {
    return valid(data)->vsync.time;
}

size_t AChoreographerFrameCallbackData_getFrameTimelinesLength(const AChoreographerFrameCallbackData* data) {
    valid(data);
    return APKRT_FRAME_TIMELINES;
}

size_t AChoreographerFrameCallbackData_getPreferredFrameTimelineIndex(const AChoreographerFrameCallbackData* data) {
    return valid(data)->vsync.preferred_timeline;
}

AVsyncId AChoreographerFrameCallbackData_getFrameTimelineVsyncId(const AChoreographerFrameCallbackData* data,
                                                                 size_t index) {
    return timeline(data, index).vsync_id;
}

int64_t AChoreographerFrameCallbackData_getFrameTimelineExpectedPresentationTimeNanos(
    const AChoreographerFrameCallbackData* data, size_t index) {
    return timeline(data, index).expected_present_time;
}

int64_t AChoreographerFrameCallbackData_getFrameTimelineDeadlineNanos(const AChoreographerFrameCallbackData* data,
                                                                      size_t index) {
    return timeline(data, index).deadline;
}

}  // extern "C"
