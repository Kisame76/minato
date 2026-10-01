// audio-live (ADR-0036, after M3): a NativeActivity on native_app_glue that plays a 440 Hz sine at -12 dBFS on
// an AAudio output stream (LOW_LATENCY, FLOAT, 2 channels, the device's rate, data callback) until the user quits.
// The stream opens in android_main, starts at APP_CMD_RESUME, pauses at APP_CMD_PAUSE and closes at
// APP_CMD_DESTROY. Its error callback logs the error once per stream and wakes the app's thread, which closes the
// stream and opens a new one with the same settings, started when the activity is resumed: AAudio's documented
// way, which never opens or closes a stream in the error callback itself. BACK calls ANativeActivity_finish, so
// the reference can quit on Android, where BACK on a task's root activity only moves the task back.
// With the tag "audio-live" it logs what Android 15 fixes: each stream's open and its values, each start, pause
// and close with the state it settles in, and the error callback. What differs by device goes to the tag
// "audio-live-device": the rate, burst, buffer, performance mode, the callback's calls and frames, the xruns at
// each pause and close, the frames played after a reopen, and the opens that failed before one did.
#include <aaudio/AAudio.h>
#include <android/input.h>
#include <android/log.h>
#include <android/looper.h>
#include <android/native_activity.h>
#include <fcntl.h>
#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "android_native_app_glue.h"

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "audio-live", __VA_ARGS__)
#define DEVICE(...) __android_log_print(ANDROID_LOG_INFO, "audio-live-device", __VA_ARGS__)

static const int64_t kMillisecond = 1000000;
static const int64_t kSecond = 1000000000;
static const double kPi = 3.14159265358979323846;
static const double kFrequency = 440;
// -12 dBFS.
static const float kAmplitude = 0.25f;
static const int32_t kChannels = 2;

static int64_t now(void) {
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return (int64_t)time.tv_sec * kSecond + time.tv_nsec;
}

static void sleep_ms(int64_t milliseconds) {
    struct timespec time = {milliseconds / 1000, (milliseconds % 1000) * kMillisecond};
    nanosleep(&time, NULL);
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

// Waits through the transient states, at most 2 s, and returns the state it settles in.
static aaudio_stream_state_t settle(AAudioStream *stream) {
    aaudio_stream_state_t state = AAudioStream_getState(stream);
    const int64_t deadline = now() + 2 * kSecond;
    while (transient(state) && now() < deadline) {
        aaudio_stream_state_t next = AAUDIO_STREAM_STATE_UNINITIALIZED;
        AAudioStream_waitForStateChange(stream, state, &next, 100 * kMillisecond);
        state = next;
    }
    return state;
}

struct player {
    // Only the app's thread changes it; the error callback compares its stream with it.
    _Atomic(AAudioStream *) stream;
    // The error callback ran for the current stream.
    atomic_bool reported;
    // The callback's work, for the device lines; the phase is the callback thread's alone, and goes on across a
    // reopen, so the sine has no jump but the gap.
    atomic_long calls;
    atomic_long frames;
    double phase;
    double rate;
    bool resumed;
    // After a reopen that started: when the device line on its frames is due, and the frames before it.
    int64_t reopen_check_at;
    long reopen_frames;
    // The error callback writes a byte, the app's looper wakes.
    int wake[2];
};

static aaudio_data_callback_result_t on_data(AAudioStream *stream, void *context, void *audio, int32_t frames) {
    (void)stream;
    struct player *player = context;
    float *out = audio;
    const double step = 2 * kPi * kFrequency / player->rate;
    for (int32_t frame = 0; frame < frames; frame++) {
        const float value = kAmplitude * (float)sin(player->phase);
        for (int32_t channel = 0; channel < kChannels; channel++) out[frame * kChannels + channel] = value;
        player->phase += step;
        if (player->phase >= 2 * kPi) player->phase -= 2 * kPi;
    }
    atomic_fetch_add(&player->calls, 1);
    atomic_fetch_add(&player->frames, frames);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

// On AAudio's own thread: once per stream, and only for the current one.
static void on_error(AAudioStream *stream, void *context, aaudio_result_t error) {
    struct player *player = context;
    if (stream != atomic_load(&player->stream) || atomic_exchange(&player->reported, true)) return;
    LOG("error callback: %s", result_name(error));
    const char byte = 1;
    if (write(player->wake[1], &byte, 1) != 1) DEVICE("the wake-up could not be written");
}

// Opens the stream; after a disconnect it tries again for up to 5 s, as the service may come back first.
static bool open_stream(struct player *player, const char *what) {
    AAudioStream *stream = NULL;
    aaudio_result_t result = AAUDIO_OK;
    int failures = 0;
    const int64_t deadline = now() + 5 * kSecond;
    for (;;) {
        AAudioStreamBuilder *builder = NULL;
        result = AAudio_createStreamBuilder(&builder);
        if (result == AAUDIO_OK) {
            AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
            AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
            AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_FLOAT);
            AAudioStreamBuilder_setChannelCount(builder, kChannels);
            AAudioStreamBuilder_setDataCallback(builder, on_data, player);
            AAudioStreamBuilder_setErrorCallback(builder, on_error, player);
            result = AAudioStreamBuilder_openStream(builder, &stream);
            AAudioStreamBuilder_delete(builder);
        }
        if (result == AAUDIO_OK || now() >= deadline) break;
        failures++;
        sleep_ms(100);
    }
    if (failures > 0) DEVICE("%s: %d opens failed before the last", what, failures);
    LOG("%s: open %s", what, result_name(result));
    if (result != AAUDIO_OK) return false;
    player->rate = AAudioStream_getSampleRate(stream);
    LOG("%s: format %s, channels %d, sharing %s, state %s", what, format_name(AAudioStream_getFormat(stream)),
        AAudioStream_getChannelCount(stream), sharing_name(AAudioStream_getSharingMode(stream)),
        state_name(AAudioStream_getState(stream)));
    DEVICE("%s: rate %d, burst %d, buffer %d of %d, performance %s", what, AAudioStream_getSampleRate(stream),
        AAudioStream_getFramesPerBurst(stream), AAudioStream_getBufferSizeInFrames(stream),
        AAudioStream_getBufferCapacityInFrames(stream), performance_name(AAudioStream_getPerformanceMode(stream)));
    atomic_store(&player->reported, false);
    atomic_store(&player->stream, stream);
    return true;
}

static void start_stream(struct player *player, const char *what) {
    AAudioStream *stream = atomic_load(&player->stream);
    if (stream == NULL) return;
    const aaudio_result_t result = AAudioStream_requestStart(stream);
    LOG("%s: requestStart %s, then %s", what, result_name(result), state_name(settle(stream)));
}

static void pause_stream(struct player *player, const char *what) {
    AAudioStream *stream = atomic_load(&player->stream);
    if (stream == NULL) return;
    const aaudio_result_t result = AAudioStream_requestPause(stream);
    LOG("%s: requestPause %s, then %s", what, result_name(result), state_name(settle(stream)));
    DEVICE("%s: %ld calls, %ld frames so far, %d xruns", what, atomic_load(&player->calls),
        atomic_load(&player->frames), AAudioStream_getXRunCount(stream));
}

static void close_stream(struct player *player, const char *what) {
    AAudioStream *stream = atomic_load(&player->stream);
    if (stream == NULL) return;
    DEVICE("%s: %ld calls, %ld frames so far, %d xruns", what, atomic_load(&player->calls),
        atomic_load(&player->frames), AAudioStream_getXRunCount(stream));
    atomic_store(&player->stream, NULL);
    LOG("%s: close %s", what, result_name(AAudioStream_close(stream)));
}

static void on_command(struct android_app *app, int32_t command) {
    struct player *player = app->userData;
    switch (command) {
    case APP_CMD_RESUME:
        player->resumed = true;
        start_stream(player, "resume");
        break;
    case APP_CMD_PAUSE:
        player->resumed = false;
        pause_stream(player, "pause");
        break;
    case APP_CMD_DESTROY:
        close_stream(player, "destroy");
        break;
    default:
        break;
    }
}

static int32_t on_input(struct android_app *app, AInputEvent *event) {
    if (AInputEvent_getType(event) != AINPUT_EVENT_TYPE_KEY || AKeyEvent_getKeyCode(event) != AKEYCODE_BACK) return 0;
    if (AKeyEvent_getAction(event) == AKEY_EVENT_ACTION_UP) {
        DEVICE("BACK: ANativeActivity_finish");
        ANativeActivity_finish(app->activity);
    }
    return 1;
}

// The error callback's wake-up: the stream it reported goes, and a new one comes.
static int on_wake(int fd, int events, void *context) {
    (void)events;
    struct player *player = context;
    char byte;
    while (read(fd, &byte, 1) == 1) {
    }
    close_stream(player, "after the error");
    if (open_stream(player, "reopen") && player->resumed) {
        start_stream(player, "reopen");
        // The device lines say whether the new stream plays: a second later, the frames since the reopen.
        player->reopen_frames = atomic_load(&player->frames);
        player->reopen_check_at = now() + kSecond;
    }
    return 1;
}

void android_main(struct android_app *app) {
    LOG("android_main");
    static struct player player;
    atomic_init(&player.stream, NULL);
    if (pipe(player.wake) != 0) {
        LOG("no pipe");
        return;
    }
    // A full pipe must not block the error callback.
    for (int index = 0; index < 2; index++) {
        const int flags = fcntl(player.wake[index], F_GETFL);
        fcntl(player.wake[index], F_SETFL, flags | O_NONBLOCK);
    }
    app->userData = &player;
    app->onAppCmd = on_command;
    app->onInputEvent = on_input;
    ALooper_addFd(app->looper, player.wake[0], LOOPER_ID_USER, ALOOPER_EVENT_INPUT, on_wake, &player);
    open_stream(&player, "start");
    bool playing = false;
    while (!app->destroyRequested) {
        struct android_poll_source *source = NULL;
        const int result = ALooper_pollOnce(50, NULL, NULL, (void **)&source);
        if ((result == LOOPER_ID_MAIN || result == LOOPER_ID_INPUT) && source != NULL) source->process(app, source);
        // For the reference's waits: the first frames played.
        if (!playing && atomic_load(&player.frames) > 0) {
            playing = true;
            DEVICE("playing");
        }
        if (player.reopen_check_at != 0 && now() >= player.reopen_check_at) {
            player.reopen_check_at = 0;
            DEVICE("reopen: %ld frames in the second after it", atomic_load(&player.frames) - player.reopen_frames);
        }
    }
    ALooper_removeFd(app->looper, player.wake[0]);
    close(player.wake[0]);
    close(player.wake[1]);
    LOG("android_main returns");
}
