// choreographer-test (ADR-0036, step a): a NativeActivity on native_app_glue whose android_main thread asks its
// AChoreographer for frames once it has the focus, and logs with the tag "choreographer-test" what Android 15
// fixes: a thread without a looper has no choreographer, one with a looper of its own has one, and a thread's
// stays the same; the refresh-rate callback's first call, on the looper's thread, and that of a second one
// registered later; the callbacks of one frame in the order they were posted, all with the frame's time; a
// callback posted inside one runs a frame later; a delayed one runs no earlier than its delay, and one posted
// from another thread while it waits runs before it, at once; one posted from another thread runs on the
// looper's thread; and the shape of the frame timelines. What differs by device, the periods and the number of timelines, goes to the tag
// "choreographer-test-device". At its first window it prefers 60 frames a second (ANativeWindow_setFrameRate),
// which the Mac's host gets as the request FRAME_RATE. After the first refresh-rate call it sets
// AWINDOW_FLAG_KEEP_SCREEN_ON, at which the Mac's host changes the period, so there the device lines show a
// second call. At the end it unregisters the refresh-rate callback and calls ANativeActivity_finish. A test that
// stalls ends after 10 s.
#include <android/choreographer.h>
#include <android/log.h>
#include <android/looper.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <android/window.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "android_native_app_glue.h"

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "choreographer-test", __VA_ARGS__)
#define DEVICE(...) __android_log_print(ANDROID_LOG_INFO, "choreographer-test-device", __VA_ARGS__)

static const int64_t kMillisecond = 1000000;

struct state {
    struct android_app *app;
    AChoreographer *choreographer;
    pthread_t looper_thread;
    bool started;
    bool done;
    int64_t deadline;         // CLOCK_MONOTONIC in ns: the end of a stalled test, 0 while none is due
    int refresh_calls;
    int second_refresh_calls;  // of the callback registered later
    int64_t frame_times[4];   // those A, B, V and L got
    int64_t posted_at;        // when the delayed callback was posted
    bool d_ran;
    pthread_t f_poster;
};

static int64_t now(void) {
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return (int64_t)time.tv_sec * 1000000000 + time.tv_nsec;
}

static const char *yes(bool value) {
    return value ? "yes" : "no";
}

static bool on_looper_thread(const struct state *state) {
    return pthread_equal(pthread_self(), state->looper_thread) != 0;
}

static void finish(struct state *state) {
    state->done = true;
    LOG("ANativeActivity_finish");
    ANativeActivity_finish(state->app->activity);
}

static void *without_looper(void *unused) {
    (void)unused;
    LOG("a thread without a looper: %s", AChoreographer_getInstance() == NULL ? "no choreographer" : "a choreographer");
    return NULL;
}

// A thread with a looper of its own gets a choreographer of its own, and then ends.
static void *with_looper(void *unused) {
    (void)unused;
    ALooper_prepare(0);
    LOG("a thread with a looper: %s", AChoreographer_getInstance() != NULL ? "a choreographer" : "no choreographer");
    return NULL;
}

static void on_refresh_rate(int64_t period, void *data);

static void on_second_refresh_rate(int64_t period, void *data);

static void on_e(int64_t time, void *data) {
    (void)time;
    struct state *state = data;
    LOG("frame E, posted from another thread: on the looper's thread: %s", yes(on_looper_thread(state)));
    AChoreographer_unregisterRefreshRateCallback(state->choreographer, on_refresh_rate, state);
    AChoreographer_unregisterRefreshRateCallback(state->choreographer, on_second_refresh_rate, state);
    DEVICE("refresh rate: %d calls, the second callback %d", state->refresh_calls, state->second_refresh_calls);
    pthread_join(state->f_poster, NULL);
    finish(state);
}

static void *post_from_another_thread(void *data) {
    struct state *state = data;
    AChoreographer_postFrameCallback64(state->choreographer, on_e, state);
    return NULL;
}

static void on_d2(long time, void *data) {
    struct state *state = data;
    LOG("frame D2, delayed 50 ms: not before its delay: %s", yes(time >= state->posted_at + 50 * kMillisecond));
    pthread_t poster;
    pthread_create(&poster, NULL, post_from_another_thread, state);
    pthread_join(poster, NULL);
}

static void on_d(int64_t time, void *data) {
    struct state *state = data;
    state->d_ran = true;
    LOG("frame D, delayed 100 ms: not before its delay: %s", yes(time >= state->posted_at + 100 * kMillisecond));
    state->posted_at = now();
    AChoreographer_postFrameCallbackDelayed(state->choreographer, on_d2, state, 50);
}

static void on_f(int64_t time, void *data) {
    struct state *state = data;
    LOG("frame F, posted from another thread while the delayed D waits: before it: %s, before its delay was up: %s",
        yes(!state->d_ran), yes(time < state->posted_at + 100 * kMillisecond));
}

// 20 ms later, while the looper sleeps until D's time.
static void *post_f(void *data) {
    struct state *state = data;
    const struct timespec wait = {0, 20 * kMillisecond};
    nanosleep(&wait, NULL);
    AChoreographer_postFrameCallback64(state->choreographer, on_f, state);
    return NULL;
}

// Registered after the first callback's first call: its own first call, at the period the first one has.
static void on_second_refresh_rate(int64_t period, void *data) {
    struct state *state = data;
    DEVICE("refresh rate, the second callback: period %" PRId64, period);
    if (++state->second_refresh_calls != 1) return;
    LOG("refresh rate: the second callback's first call, on the looper's thread: %s", yes(on_looper_thread(state)));
    state->posted_at = now();
    AChoreographer_postFrameCallbackDelayed64(state->choreographer, on_d, state, 100);
    pthread_create(&state->f_poster, NULL, post_f, state);
}

static void on_c(int64_t time, void *data) {
    struct state *state = data;
    LOG("frame C, posted in A: a later frame: %s", yes(time > state->frame_times[0]));
    AChoreographer_registerRefreshRateCallback(state->choreographer, on_second_refresh_rate, state);
}

static void on_a(int64_t time, void *data) {
    struct state *state = data;
    LOG("frame A");
    state->frame_times[0] = time;
    AChoreographer_postFrameCallback64(state->choreographer, on_c, state);
}

static void on_b(int64_t time, void *data) {
    struct state *state = data;
    LOG("frame B");
    state->frame_times[1] = time;
}

static void on_v(const AChoreographerFrameCallbackData *data, void *user) {
    struct state *state = user;
    LOG("vsync V");
    const int64_t frame_time = AChoreographerFrameCallbackData_getFrameTimeNanos(data);
    state->frame_times[2] = frame_time;
    const size_t count = AChoreographerFrameCallbackData_getFrameTimelinesLength(data);
    const size_t preferred = AChoreographerFrameCallbackData_getPreferredFrameTimelineIndex(data);
    DEVICE("timelines: %zu, preferred %zu", count, preferred);
    bool ids = true, presents = true, deadlines = true;
    for (size_t index = 0; index < count; ++index) {
        const int64_t present = AChoreographerFrameCallbackData_getFrameTimelineExpectedPresentationTimeNanos(data, index);
        deadlines = deadlines && AChoreographerFrameCallbackData_getFrameTimelineDeadlineNanos(data, index) < present;
        if (index == 0) continue;
        ids = ids && AChoreographerFrameCallbackData_getFrameTimelineVsyncId(data, index) >
                         AChoreographerFrameCallbackData_getFrameTimelineVsyncId(data, index - 1);
        presents = presents &&
                   present > AChoreographerFrameCallbackData_getFrameTimelineExpectedPresentationTimeNanos(data, index - 1);
    }
    LOG("timelines: at least one: %s, the preferred among them: %s", yes(count >= 1), yes(preferred < count));
    LOG("timelines: vsync IDs rise: %s, expected presents rise: %s, each deadline before its present: %s", yes(ids),
        yes(presents), yes(deadlines));
    if (preferred < count) {
        LOG("timelines: the preferred deadline after the frame time: %s",
            yes(AChoreographerFrameCallbackData_getFrameTimelineDeadlineNanos(data, preferred) > frame_time));
    }
}

static void on_l(long time, void *data) {
    struct state *state = data;
    LOG("frame L");
    state->frame_times[3] = time;
    const int64_t *times = state->frame_times;
    LOG("one frame time for A, B, V and L: %s", yes(times[1] == times[0] && times[2] == times[0] && times[3] == times[0]));
}

static void on_refresh_rate(int64_t period, void *data) {
    struct state *state = data;
    DEVICE("refresh rate: period %" PRId64, period);
    if (++state->refresh_calls != 1) return;
    LOG("refresh rate: first call, on the looper's thread: %s", yes(on_looper_thread(state)));
    // The Mac's host changes the period at this request.
    ANativeActivity_setWindowFlags(state->app->activity, AWINDOW_FLAG_KEEP_SCREEN_ON, 0);
    AChoreographer_postFrameCallback64(state->choreographer, on_a, state);
    AChoreographer_postFrameCallback64(state->choreographer, on_b, state);
    AChoreographer_postVsyncCallback(state->choreographer, on_v, state);
    AChoreographer_postFrameCallback(state->choreographer, on_l, state);
}

static void on_command(struct android_app *app, int32_t command) {
    struct state *state = app->userData;
    if (command == APP_CMD_INIT_WINDOW && !state->started) {
        LOG("ANativeWindow_setFrameRate: %d",
            ANativeWindow_setFrameRate(app->window, 60, ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_DEFAULT));
    }
    if (command != APP_CMD_GAINED_FOCUS || state->started) return;
    state->started = true;
    state->deadline = now() + 10000 * kMillisecond;
    state->choreographer = AChoreographer_getInstance();
    LOG("the looper's thread: %s", state->choreographer != NULL ? "a choreographer" : "no choreographer");
    if (state->choreographer == NULL) {
        finish(state);
        return;
    }
    LOG("the same choreographer again: %s", yes(AChoreographer_getInstance() == state->choreographer));
    // The second registration of the same callback and data is ignored: one first call.
    AChoreographer_registerRefreshRateCallback(state->choreographer, on_refresh_rate, state);
    AChoreographer_registerRefreshRateCallback(state->choreographer, on_refresh_rate, state);
}

void android_main(struct android_app *app) {
    struct state state = {0};
    state.app = app;
    state.looper_thread = pthread_self();
    app->userData = &state;
    app->onAppCmd = on_command;
    LOG("android_main");
    pthread_t thread;
    pthread_create(&thread, NULL, without_looper, NULL);
    pthread_join(thread, NULL);
    pthread_create(&thread, NULL, with_looper, NULL);
    pthread_join(thread, NULL);

    while (!app->destroyRequested) {
        int timeout = -1;
        if (state.deadline != 0 && !state.done) {
            const int64_t left = state.deadline - now();
            timeout = left > 0 ? (int)((left + kMillisecond - 1) / kMillisecond) : 0;
        }
        struct android_poll_source *source = NULL;
        const int result = ALooper_pollOnce(timeout, NULL, NULL, (void **)&source);
        if ((result == LOOPER_ID_MAIN || result == LOOPER_ID_INPUT) && source != NULL) source->process(app, source);
        if (state.deadline != 0 && !state.done && now() >= state.deadline) {
            LOG("stalled after %d refresh-rate calls", state.refresh_calls);
            finish(&state);
        }
    }
    LOG("android_main returns");
}
