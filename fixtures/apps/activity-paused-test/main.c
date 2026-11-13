// activity-paused-test: a NativeActivity without native_app_glue that logs libc.so's minato_activity_paused() at each
// lifecycle callback. The Mac behavioural test runtime.activity_paused plays start, background and foreground and
// checks the values: onResume reads 0, onPause and onStop read 1. That proves the SDL swap override's production path
// end to end -- libc exports the query, the loader points its flag at the host's toggled int (runtime/src/dl.cpp
// preload_sdl_swap, with libminato-sdl universally preloaded), and apkrt_session_post toggles it at onPause/onStop (1)
// and onResume (0). minato_activity_paused is a weak import, so where it is absent (Android, or a build that dropped
// the libc export) the app links and logs -1, and the test goes red. After the first return the app finishes itself,
// as activity-test does, so the host's quit ends the run.
#include <android/log.h>
#include <android/native_activity.h>
#include <stddef.h>

// libc.so's query; weak so the app links and runs where it is absent.
int minato_activity_paused(void) __attribute__((weak));

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "activity-paused-test", __VA_ARGS__)

static int seen_pause;

static int query(void) {
    return minato_activity_paused != NULL ? minato_activity_paused() : -1;
}

// onStart is not logged: the host posts START and RESUME back to back, so whether the query reads the paused 1 or the
// resumed 0 at onStart is a race. onResume, onPause and onStop are read after a single settled toggle, so they are
// deterministic -- and they are what the SDL swap override cares about.

static void on_resume(ANativeActivity *activity) {
    LOG("onResume: paused=%d", query());
    if (seen_pause) ANativeActivity_finish(activity);  // after the return, end the run (the one onDestroy without Java)
}

static void on_pause(ANativeActivity *activity) {
    (void)activity;
    seen_pause = 1;
    LOG("onPause: paused=%d", query());
}

static void on_stop(ANativeActivity *activity) {
    (void)activity;
    LOG("onStop: paused=%d", query());
}

__attribute__((visibility("default"))) void ANativeActivity_onCreate(ANativeActivity *activity, void *state,
                                                                     size_t size) {
    (void)state;
    (void)size;
    activity->callbacks->onResume = on_resume;
    activity->callbacks->onPause = on_pause;
    activity->callbacks->onStop = on_stop;
    LOG("onCreate: paused=%d", query());
}
