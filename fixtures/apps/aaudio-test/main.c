// aaudio-test (ADR-0036, step c): a NativeActivity on native_app_glue whose test thread opens AAudio output
// streams and logs with the tag "aaudio-test" what Android 15 fixes for the subset Minato offers (decision 6):
// the text helpers; the values of a stream opened with nothing set and of one opened with everything set
// (a requested rate, channel count and format are kept, EXCLUSIVE becomes SHARED); a blocking stream's states
// from OPEN through STARTED, PAUSED, FLUSHED, STARTED again to STOPPED, and a flush while started; writes with
// a timeout before the start, while started and while paused with a full buffer; a callback stream's calls
// over 1 s, its timestamp, and a write before its start and while started; and a stream whose callback returns
// STOP after 10 calls. What differs by device, the rates, bursts, counts and times, goes to the tag
// "aaudio-test-device". Every stream plays zeros. At the end the test calls ANativeActivity_finish.
#include <aaudio/AAudio.h>
#include <android/log.h>
#include <android/native_activity.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "android_native_app_glue.h"

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "aaudio-test", __VA_ARGS__)
#define DEVICE(...) __android_log_print(ANDROID_LOG_INFO, "aaudio-test-device", __VA_ARGS__)

static const int64_t kMillisecond = 1000000;
static const int64_t kSecond = 1000000000;

static int64_t now(void) {
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return (int64_t)time.tv_sec * kSecond + time.tv_nsec;
}

static void sleep_ms(int64_t milliseconds) {
    struct timespec time = {milliseconds / 1000, (milliseconds % 1000) * kMillisecond};
    nanosleep(&time, NULL);
}

static const char *yes(bool value) {
    return value ? "yes" : "no";
}

// The names of the header, so that the compared lines do not depend on AAudio's own texts.
static const char *result_name(aaudio_result_t result) {
    if (result > 0) return "a count";
    switch (result) {
    case AAUDIO_OK: return "OK";
    case AAUDIO_ERROR_DISCONNECTED: return "ERROR_DISCONNECTED";
    case AAUDIO_ERROR_ILLEGAL_ARGUMENT: return "ERROR_ILLEGAL_ARGUMENT";
    case AAUDIO_ERROR_INTERNAL: return "ERROR_INTERNAL";
    case AAUDIO_ERROR_INVALID_STATE: return "ERROR_INVALID_STATE";
    case AAUDIO_ERROR_INVALID_HANDLE: return "ERROR_INVALID_HANDLE";
    case AAUDIO_ERROR_UNIMPLEMENTED: return "ERROR_UNIMPLEMENTED";
    case AAUDIO_ERROR_UNAVAILABLE: return "ERROR_UNAVAILABLE";
    case AAUDIO_ERROR_NO_FREE_HANDLES: return "ERROR_NO_FREE_HANDLES";
    case AAUDIO_ERROR_NO_MEMORY: return "ERROR_NO_MEMORY";
    case AAUDIO_ERROR_NULL: return "ERROR_NULL";
    case AAUDIO_ERROR_TIMEOUT: return "ERROR_TIMEOUT";
    case AAUDIO_ERROR_WOULD_BLOCK: return "ERROR_WOULD_BLOCK";
    case AAUDIO_ERROR_INVALID_FORMAT: return "ERROR_INVALID_FORMAT";
    case AAUDIO_ERROR_OUT_OF_RANGE: return "ERROR_OUT_OF_RANGE";
    case AAUDIO_ERROR_NO_SERVICE: return "ERROR_NO_SERVICE";
    case AAUDIO_ERROR_INVALID_RATE: return "ERROR_INVALID_RATE";
    default: return "another error";
    }
}

static const char *state_name(aaudio_stream_state_t state) {
    switch (state) {
    case AAUDIO_STREAM_STATE_UNINITIALIZED: return "UNINITIALIZED";
    case AAUDIO_STREAM_STATE_UNKNOWN: return "UNKNOWN";
    case AAUDIO_STREAM_STATE_OPEN: return "OPEN";
    case AAUDIO_STREAM_STATE_STARTING: return "STARTING";
    case AAUDIO_STREAM_STATE_STARTED: return "STARTED";
    case AAUDIO_STREAM_STATE_PAUSING: return "PAUSING";
    case AAUDIO_STREAM_STATE_PAUSED: return "PAUSED";
    case AAUDIO_STREAM_STATE_FLUSHING: return "FLUSHING";
    case AAUDIO_STREAM_STATE_FLUSHED: return "FLUSHED";
    case AAUDIO_STREAM_STATE_STOPPING: return "STOPPING";
    case AAUDIO_STREAM_STATE_STOPPED: return "STOPPED";
    case AAUDIO_STREAM_STATE_CLOSING: return "CLOSING";
    case AAUDIO_STREAM_STATE_CLOSED: return "CLOSED";
    case AAUDIO_STREAM_STATE_DISCONNECTED: return "DISCONNECTED";
    default: return "another state";
    }
}

static const char *format_name(aaudio_format_t format) {
    switch (format) {
    case AAUDIO_FORMAT_UNSPECIFIED: return "UNSPECIFIED";
    case AAUDIO_FORMAT_PCM_I16: return "I16";
    case AAUDIO_FORMAT_PCM_FLOAT: return "FLOAT";
    case AAUDIO_FORMAT_PCM_I24_PACKED: return "I24_PACKED";
    case AAUDIO_FORMAT_PCM_I32: return "I32";
    default: return "another format";
    }
}

static const char *sharing_name(aaudio_sharing_mode_t mode) {
    switch (mode) {
    case AAUDIO_SHARING_MODE_EXCLUSIVE: return "EXCLUSIVE";
    case AAUDIO_SHARING_MODE_SHARED: return "SHARED";
    default: return "another mode";
    }
}

static const char *performance_name(aaudio_performance_mode_t mode) {
    switch (mode) {
    case AAUDIO_PERFORMANCE_MODE_NONE: return "NONE";
    case AAUDIO_PERFORMANCE_MODE_POWER_SAVING: return "POWER_SAVING";
    case AAUDIO_PERFORMANCE_MODE_LOW_LATENCY: return "LOW_LATENCY";
    default: return "another mode";
    }
}

static bool transient(aaudio_stream_state_t state) {
    return state == AAUDIO_STREAM_STATE_STARTING || state == AAUDIO_STREAM_STATE_PAUSING ||
           state == AAUDIO_STREAM_STATE_FLUSHING || state == AAUDIO_STREAM_STATE_STOPPING ||
           state == AAUDIO_STREAM_STATE_CLOSING;
}

// Waits through the transient states, at most 2 s, and returns the state it settles in; the states on the way
// go to the device lines under the name $what.
static aaudio_stream_state_t settle(AAudioStream *stream, const char *what) {
    char path[160] = "";
    aaudio_stream_state_t state = AAudioStream_getState(stream);
    strlcat(path, state_name(state), sizeof(path));
    const int64_t deadline = now() + 2 * kSecond;
    while (transient(state) && now() < deadline) {
        aaudio_stream_state_t next = AAUDIO_STREAM_STATE_UNINITIALIZED;
        AAudioStream_waitForStateChange(stream, state, &next, 100 * kMillisecond);
        if (next != state) {
            strlcat(path, " ", sizeof(path));
            strlcat(path, state_name(next), sizeof(path));
        }
        state = next;
    }
    DEVICE("%s: %s", what, path);
    return state;
}

// Waits until the stream is in $wanted, at most 2 s, through any other states on the way.
static aaudio_stream_state_t wait_for(AAudioStream *stream, aaudio_stream_state_t wanted) {
    aaudio_stream_state_t state = AAudioStream_getState(stream);
    const int64_t deadline = now() + 2 * kSecond;
    while (state != wanted && now() < deadline) {
        aaudio_stream_state_t next = AAUDIO_STREAM_STATE_UNINITIALIZED;
        AAudioStream_waitForStateChange(stream, state, &next, 100 * kMillisecond);
        state = next;
    }
    return state;
}

static AAudioStreamBuilder *builder_with_everything(void) {
    AAudioStreamBuilder *builder = NULL;
    AAudio_createStreamBuilder(&builder);
    AAudioStreamBuilder_setSampleRate(builder, 44100);
    AAudioStreamBuilder_setChannelCount(builder, 2);
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_EXCLUSIVE);
    AAudioStreamBuilder_setUsage(builder, AAUDIO_USAGE_GAME);
    return builder;
}

static void texts(void) {
    const aaudio_result_t results[] = {AAUDIO_OK, AAUDIO_ERROR_INVALID_STATE, AAUDIO_ERROR_UNAVAILABLE,
                                       AAUDIO_ERROR_TIMEOUT};
    for (size_t i = 0; i < sizeof(results) / sizeof(results[0]); i++) {
        LOG("AAudio_convertResultToText(%d): %s", results[i], AAudio_convertResultToText(results[i]));
    }
    const aaudio_stream_state_t states[] = {AAUDIO_STREAM_STATE_OPEN, AAUDIO_STREAM_STATE_STARTED,
                                            AAUDIO_STREAM_STATE_STOPPED, AAUDIO_STREAM_STATE_CLOSED};
    for (size_t i = 0; i < sizeof(states) / sizeof(states[0]); i++) {
        LOG("AAudio_convertStreamStateToText(%d): %s", states[i], AAudio_convertStreamStateToText(states[i]));
    }
}

static void nothing_set(void) {
    AAudioStreamBuilder *builder = NULL;
    AAudio_createStreamBuilder(&builder);
    AAudioStream *stream = NULL;
    const aaudio_result_t result = AAudioStreamBuilder_openStream(builder, &stream);
    AAudioStreamBuilder_delete(builder);
    LOG("nothing set: open %s", result_name(result));
    if (result != AAUDIO_OK) return;
    LOG("nothing set: direction %s, sharing %s, usage %d, content type %d, session ID %d, state %s",
        AAudioStream_getDirection(stream) == AAUDIO_DIRECTION_OUTPUT ? "OUTPUT" : "INPUT",
        sharing_name(AAudioStream_getSharingMode(stream)), AAudioStream_getUsage(stream),
        AAudioStream_getContentType(stream), AAudioStream_getSessionId(stream),
        state_name(AAudioStream_getState(stream)));
    DEVICE("nothing set: rate %d, channels %d, format %s, performance %s, burst %d, capacity %d",
           AAudioStream_getSampleRate(stream), AAudioStream_getChannelCount(stream),
           format_name(AAudioStream_getFormat(stream)), performance_name(AAudioStream_getPerformanceMode(stream)),
           AAudioStream_getFramesPerBurst(stream), AAudioStream_getBufferCapacityInFrames(stream));
    LOG("nothing set: close %s", result_name(AAudioStream_close(stream)));
}

// Every frame of a write, with a timeout; the count or the error.
static aaudio_result_t write_zeros(AAudioStream *stream, int32_t frames, int64_t timeout) {
    int16_t *zeros = calloc((size_t)frames * 2, sizeof(int16_t));
    const aaudio_result_t result = AAudioStream_write(stream, zeros, frames, timeout);
    free(zeros);
    return result;
}

static void blocking(void) {
    AAudioStreamBuilder *builder = builder_with_everything();
    AAudioStream *stream = NULL;
    const aaudio_result_t result = AAudioStreamBuilder_openStream(builder, &stream);
    AAudioStreamBuilder_delete(builder);
    LOG("everything set: open %s", result_name(result));
    if (result != AAUDIO_OK) return;
    const int32_t burst = AAudioStream_getFramesPerBurst(stream);
    const int32_t capacity = AAudioStream_getBufferCapacityInFrames(stream);
    LOG("everything set: rate %d, channels %d, format %s, sharing %s, usage %d, burst above 0: %s, "
        "capacity at least a burst: %s, state %s",
        AAudioStream_getSampleRate(stream), AAudioStream_getChannelCount(stream),
        format_name(AAudioStream_getFormat(stream)), sharing_name(AAudioStream_getSharingMode(stream)),
        AAudioStream_getUsage(stream), yes(burst > 0), yes(capacity >= burst),
        state_name(AAudioStream_getState(stream)));
    DEVICE("everything set: burst %d, capacity %d, buffer size %d, performance %s, device ID %d", burst, capacity,
           AAudioStream_getBufferSizeInFrames(stream), performance_name(AAudioStream_getPerformanceMode(stream)),
           AAudioStream_getDeviceId(stream));

    // Before the start nothing plays, so a write without a timeout fills the buffer and the next one finds it full.
    const aaudio_result_t first = write_zeros(stream, capacity + burst, 0);
    const aaudio_result_t second = write_zeros(stream, burst, 0);
    LOG("write before the start, timeout 0: frames written: %s; again: %s", yes(first > 0),
        second == 0 ? "none" : result_name(second));
    DEVICE("write before the start: %d of %d frames", first, capacity + burst);

    LOG("requestStart: %s", result_name(AAudioStream_requestStart(stream)));
    LOG("after requestStart: %s", state_name(settle(stream, "requestStart")));
    LOG("requestFlush while started: %s", result_name(AAudioStream_requestFlush(stream)));
    const int32_t tenth = AAudioStream_getSampleRate(stream) / 10;
    int64_t start = now();
    const aaudio_result_t played = write_zeros(stream, tenth, kSecond);
    LOG("write of 100 ms while started, timeout 1 s: %s",
        played == tenth ? "every frame" : played >= 0 ? "some frames" : result_name(played));
    DEVICE("write of 100 ms: %d frames in %" PRId64 " ms", played, (now() - start) / kMillisecond);

    LOG("requestPause: %s", result_name(AAudioStream_requestPause(stream)));
    LOG("after requestPause: %s", state_name(settle(stream, "requestPause")));
    const int32_t asked = 2 * capacity;
    start = now();
    const aaudio_result_t paused = write_zeros(stream, asked, 50 * kMillisecond);
    LOG("write of twice the capacity while paused, timeout 50 ms: fewer frames than asked: %s",
        paused >= 0 ? yes(paused < asked) : result_name(paused));
    DEVICE("write while paused: %d of %d frames in %" PRId64 " ms", paused, asked, (now() - start) / kMillisecond);

    LOG("requestFlush: %s", result_name(AAudioStream_requestFlush(stream)));
    LOG("after requestFlush: %s", state_name(settle(stream, "requestFlush")));
    write_zeros(stream, capacity, 0);
    LOG("requestStart again: %s", result_name(AAudioStream_requestStart(stream)));
    LOG("after requestStart again: %s", state_name(settle(stream, "requestStart again")));
    LOG("requestStop: %s", result_name(AAudioStream_requestStop(stream)));
    LOG("after requestStop: %s", state_name(settle(stream, "requestStop")));
    LOG("everything set: close %s", result_name(AAudioStream_close(stream)));
}

struct calls {
    pthread_t test_thread;
    atomic_int count;
    atomic_int after_stop;      // calls after the one that returned STOP
    atomic_int_least64_t frames;
    atomic_int fewest;          // the fewest frames in a call
    atomic_int most;
    atomic_bool on_test_thread;
    atomic_int errors;
    int stop_at;                // the call that returns STOP, 0 for none
};

static aaudio_data_callback_result_t on_data(AAudioStream *stream, void *context, void *data, int32_t frames) {
    struct calls *calls = context;
    const int count = atomic_fetch_add(&calls->count, 1) + 1;
    if (calls->stop_at != 0 && count > calls->stop_at) atomic_fetch_add(&calls->after_stop, 1);
    atomic_fetch_add(&calls->frames, frames);
    if (frames < atomic_load(&calls->fewest)) atomic_store(&calls->fewest, frames);
    if (frames > atomic_load(&calls->most)) atomic_store(&calls->most, frames);
    if (pthread_equal(pthread_self(), calls->test_thread)) atomic_store(&calls->on_test_thread, true);
    memset(data, 0, (size_t)frames * (size_t)AAudioStream_getChannelCount(stream) * sizeof(int16_t));
    return calls->stop_at != 0 && count == calls->stop_at ? AAUDIO_CALLBACK_RESULT_STOP
                                                          : AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static void on_error(AAudioStream *stream, void *context, aaudio_result_t error) {
    (void)stream;
    (void)error;
    struct calls *calls = context;
    atomic_fetch_add(&calls->errors, 1);
}

static AAudioStream *open_with_callback(struct calls *calls, const char *what) {
    calls->test_thread = pthread_self();
    atomic_store(&calls->fewest, INT32_MAX);
    AAudioStreamBuilder *builder = builder_with_everything();
    AAudioStreamBuilder_setDataCallback(builder, on_data, calls);
    AAudioStreamBuilder_setErrorCallback(builder, on_error, calls);
    AAudioStream *stream = NULL;
    const aaudio_result_t result = AAudioStreamBuilder_openStream(builder, &stream);
    AAudioStreamBuilder_delete(builder);
    LOG("%s: open %s", what, result_name(result));
    return result == AAUDIO_OK ? stream : NULL;
}

static void with_callback(void) {
    struct calls calls = {0};
    AAudioStream *stream = open_with_callback(&calls, "callback");
    if (stream == NULL) return;
    // AAudioStream_write refuses a callback stream only while its callback is active.
    const aaudio_result_t before = write_zeros(stream, 64, 0);
    LOG("callback: write of 64 frames before the start: %s", before == 64 ? "every frame" : result_name(before));
    LOG("callback: requestStart: %s", result_name(AAudioStream_requestStart(stream)));
    LOG("callback: after requestStart: %s", state_name(settle(stream, "callback requestStart")));
    LOG("callback: write while started: %s", result_name(write_zeros(stream, 64, 0)));
    sleep_ms(1000);
    const int count = atomic_load(&calls.count);
    const int64_t written = AAudioStream_getFramesWritten(stream);
    int64_t position = 0;
    int64_t time = 0;
    const aaudio_result_t stamped = AAudioStream_getTimestamp(stream, CLOCK_MONOTONIC, &position, &time);
    LOG("callback: after 1 s: called: %s, frames in every call: %s, never on the test thread: %s, "
        "frames written: %s, errors %d", yes(count > 0), yes(atomic_load(&calls.fewest) > 0),
        yes(!atomic_load(&calls.on_test_thread)), yes(written > 0), atomic_load(&calls.errors));
    LOG("callback: getTimestamp: %s, its position at most the frames written: %s", result_name(stamped),
        stamped == AAUDIO_OK ? yes(position <= written) : "-");
    DEVICE("callback: %d calls, %" PRId64 " frames, %d to %d a call, %" PRId64 " written, %" PRId64
           " read, %d xruns; timestamp at frame %" PRId64 ", %" PRId64 " ms before now",
           count, atomic_load(&calls.frames), atomic_load(&calls.fewest), atomic_load(&calls.most), written,
           AAudioStream_getFramesRead(stream), AAudioStream_getXRunCount(stream), position,
           (now() - time) / kMillisecond);
    LOG("callback: requestStop: %s", result_name(AAudioStream_requestStop(stream)));
    LOG("callback: after requestStop: %s", state_name(settle(stream, "callback requestStop")));
    LOG("callback: close %s", result_name(AAudioStream_close(stream)));
}

static void stop_from_callback(void) {
    struct calls calls = {.stop_at = 10};
    AAudioStream *stream = open_with_callback(&calls, "STOP from the callback");
    if (stream == NULL) return;
    LOG("STOP from the callback: requestStart: %s", result_name(AAudioStream_requestStart(stream)));
    const aaudio_stream_state_t state = wait_for(stream, AAUDIO_STREAM_STATE_STOPPED);
    sleep_ms(200);
    LOG("STOP from the callback, after the 10th call: %s; 200 ms later: %d calls, %d after STOP",
        state_name(state), atomic_load(&calls.count), atomic_load(&calls.after_stop));
    LOG("STOP from the callback: close %s", result_name(AAudioStream_close(stream)));
}

static atomic_bool finished;

static void *run_tests(void *unused) {
    (void)unused;
    texts();
    nothing_set();
    blocking();
    with_callback();
    stop_from_callback();
    atomic_store(&finished, true);
    return NULL;
}

void android_main(struct android_app *app) {
    LOG("android_main");
    pthread_t tests;
    pthread_create(&tests, NULL, run_tests, NULL);
    bool finishing = false;
    while (!app->destroyRequested) {
        struct android_poll_source *source = NULL;
        const int result = ALooper_pollOnce(50, NULL, NULL, (void **)&source);
        if ((result == LOOPER_ID_MAIN || result == LOOPER_ID_INPUT) && source != NULL) source->process(app, source);
        if (!finishing && atomic_load(&finished)) {
            finishing = true;
            pthread_join(tests, NULL);
            LOG("ANativeActivity_finish");
            ANativeActivity_finish(app->activity);
        }
    }
    LOG("android_main returns");
}
