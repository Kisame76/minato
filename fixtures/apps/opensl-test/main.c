// opensl-test (ADR-0036, step c): a NativeActivity on native_app_glue whose test thread uses OpenSL ES and logs
// with the tag "opensl-test" what Android 15 fixes for the subset Minato offers (decision 7): the engine and the
// output mix; an audio player from an Android simple buffer queue of 16-bit PCM, configured through
// ANDROIDCONFIGURATION before Realize (stream type and performance mode, read back) and refused after it; its
// play states; its buffer queue, full at two buffers, cleared by Clear but not by STOPPED, with the callback
// while it plays; its volume; and a second player from a buffer queue of float PCM (SLAndroidDataFormat_PCM_EX),
// which also asks for EFFECTSEND, as the NDK's native-audio sample does, and sets its direct level.
// What differs by device, the number of callbacks, goes to the tag "opensl-test-device". Every player plays
// zeros. At the end the test calls ANativeActivity_finish.
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
#include <SLES/OpenSLES_AndroidConfiguration.h>
#include <android/log.h>
#include <android/native_activity.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "android_native_app_glue.h"

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "opensl-test", __VA_ARGS__)
#define DEVICE(...) __android_log_print(ANDROID_LOG_INFO, "opensl-test-device", __VA_ARGS__)

static void sleep_ms(long milliseconds) {
    struct timespec time = {milliseconds / 1000, (milliseconds % 1000) * 1000000};
    nanosleep(&time, NULL);
}

static const char *yes(bool value) {
    return value ? "yes" : "no";
}

// The names of the header, so that the compared lines do not depend on the library.
static const char *result_name(SLresult result) {
    static const char *const names[] = {
        "SUCCESS", "PRECONDITIONS_VIOLATED", "PARAMETER_INVALID", "MEMORY_FAILURE", "RESOURCE_ERROR",
        "RESOURCE_LOST", "IO_ERROR", "BUFFER_INSUFFICIENT", "CONTENT_CORRUPTED", "CONTENT_UNSUPPORTED",
        "CONTENT_NOT_FOUND", "PERMISSION_DENIED", "FEATURE_UNSUPPORTED", "INTERNAL_ERROR", "UNKNOWN_ERROR",
        "OPERATION_ABORTED", "CONTROL_LOST"};
    return result < sizeof(names) / sizeof(names[0]) ? names[result] : "another result";
}

static const char *play_state_name(SLuint32 state) {
    switch (state) {
    case SL_PLAYSTATE_STOPPED: return "STOPPED";
    case SL_PLAYSTATE_PAUSED: return "PAUSED";
    case SL_PLAYSTATE_PLAYING: return "PLAYING";
    default: return "another state";
    }
}

// 10 ms of 16-bit stereo zeros at 44.1 kHz, and of float mono zeros at 48 kHz.
static int16_t stereo[441 * 2];
static float mono[480];

struct queue {
    pthread_t test_thread;
    atomic_int calls;
    atomic_bool refill;
    atomic_bool on_test_thread;
    void *buffer;
    SLuint32 size;
};

static void on_buffer(SLAndroidSimpleBufferQueueItf queue, void *context) {
    struct queue *state = context;
    atomic_fetch_add(&state->calls, 1);
    if (pthread_equal(pthread_self(), state->test_thread)) atomic_store(&state->on_test_thread, true);
    if (atomic_load(&state->refill)) (*queue)->Enqueue(queue, state->buffer, state->size);
}

static void on_float_buffer(SLBufferQueueItf queue, void *context) {
    (void)queue;
    struct queue *state = context;
    atomic_fetch_add(&state->calls, 1);
}

static void configure(SLObjectItf player) {
    SLAndroidConfigurationItf configuration = NULL;
    LOG("GetInterface ANDROIDCONFIGURATION before Realize: %s",
        result_name((*player)->GetInterface(player, SL_IID_ANDROIDCONFIGURATION, &configuration)));
    if (configuration == NULL) return;
    SLint32 stream_type = SL_ANDROID_STREAM_MEDIA;
    LOG("SetConfiguration stream type MEDIA: %s",
        result_name((*configuration)->SetConfiguration(configuration, SL_ANDROID_KEY_STREAM_TYPE, &stream_type,
                                                       sizeof(stream_type))));
    SLuint32 mode = SL_ANDROID_PERFORMANCE_LATENCY;
    LOG("SetConfiguration performance mode LATENCY: %s",
        result_name((*configuration)->SetConfiguration(configuration, SL_ANDROID_KEY_PERFORMANCE_MODE, &mode,
                                                       sizeof(mode))));
    SLint32 read_type = -1;
    SLuint32 size = sizeof(read_type);
    const SLresult type_result =
        (*configuration)->GetConfiguration(configuration, SL_ANDROID_KEY_STREAM_TYPE, &size, &read_type);
    SLuint32 read_mode = UINT32_MAX;
    size = sizeof(read_mode);
    const SLresult mode_result =
        (*configuration)->GetConfiguration(configuration, SL_ANDROID_KEY_PERFORMANCE_MODE, &size, &read_mode);
    LOG("GetConfiguration: stream type %s, %d; performance mode %s, %u", result_name(type_result), read_type,
        result_name(mode_result), read_mode);
    LOG("Realize the player: %s", result_name((*player)->Realize(player, SL_BOOLEAN_FALSE)));
    mode = SL_ANDROID_PERFORMANCE_NONE;
    LOG("SetConfiguration performance mode after Realize: %s",
        result_name((*configuration)->SetConfiguration(configuration, SL_ANDROID_KEY_PERFORMANCE_MODE, &mode,
                                                       sizeof(mode))));
}

static void play_state(SLPlayItf play, SLuint32 state) {
    const SLresult result = (*play)->SetPlayState(play, state);
    SLuint32 read = 0;
    (*play)->GetPlayState(play, &read);
    LOG("SetPlayState %s: %s, reads %s", play_state_name(state), result_name(result), play_state_name(read));
}

static void queue_count(SLAndroidSimpleBufferQueueItf queue, const char *when) {
    SLAndroidSimpleBufferQueueState state = {0};
    const SLresult result = (*queue)->GetState(queue, &state);
    LOG("buffer queue %s: GetState %s, count %u", when, result_name(result), state.count);
}

static void pcm_player(SLEngineItf engine, SLObjectItf mix) {
    SLDataLocator_AndroidSimpleBufferQueue queue_locator = {SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, 2};
    SLDataFormat_PCM format = {SL_DATAFORMAT_PCM, 2, SL_SAMPLINGRATE_44_1, SL_PCMSAMPLEFORMAT_FIXED_16,
                               SL_PCMSAMPLEFORMAT_FIXED_16, SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT,
                               SL_BYTEORDER_LITTLEENDIAN};
    SLDataSource source = {&queue_locator, &format};
    SLDataLocator_OutputMix mix_locator = {SL_DATALOCATOR_OUTPUTMIX, mix};
    SLDataSink sink = {&mix_locator, NULL};
    const SLInterfaceID ids[] = {SL_IID_ANDROIDSIMPLEBUFFERQUEUE, SL_IID_VOLUME, SL_IID_ANDROIDCONFIGURATION};
    const SLboolean required[] = {SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE};
    SLObjectItf player = NULL;
    LOG("CreateAudioPlayer, 16-bit PCM from a simple buffer queue: %s",
        result_name((*engine)->CreateAudioPlayer(engine, &player, &source, &sink, 3, ids, required)));
    if (player == NULL) return;
    configure(player);

    SLPlayItf play = NULL;
    SLAndroidSimpleBufferQueueItf queue = NULL;
    SLVolumeItf volume = NULL;
    LOG("GetInterface PLAY %s, ANDROIDSIMPLEBUFFERQUEUE %s, VOLUME %s",
        result_name((*player)->GetInterface(player, SL_IID_PLAY, &play)),
        result_name((*player)->GetInterface(player, SL_IID_ANDROIDSIMPLEBUFFERQUEUE, &queue)),
        result_name((*player)->GetInterface(player, SL_IID_VOLUME, &volume)));
    if (play == NULL || queue == NULL || volume == NULL) {
        (*player)->Destroy(player);
        return;
    }

    SLuint32 state = 0;
    (*play)->GetPlayState(play, &state);
    LOG("the first play state: %s", play_state_name(state));
    play_state(play, SL_PLAYSTATE_PLAYING);
    play_state(play, SL_PLAYSTATE_PAUSED);
    play_state(play, SL_PLAYSTATE_STOPPED);

    struct queue context = {.test_thread = pthread_self(), .buffer = stereo, .size = sizeof(stereo)};
    LOG("RegisterCallback: %s", result_name((*queue)->RegisterCallback(queue, on_buffer, &context)));
    LOG("Enqueue 1: %s", result_name((*queue)->Enqueue(queue, stereo, sizeof(stereo))));
    LOG("Enqueue 2: %s", result_name((*queue)->Enqueue(queue, stereo, sizeof(stereo))));
    LOG("Enqueue 3 into a queue of 2: %s", result_name((*queue)->Enqueue(queue, stereo, sizeof(stereo))));
    queue_count(queue, "with 2 buffers");
    LOG("Clear: %s", result_name((*queue)->Clear(queue)));
    queue_count(queue, "after Clear");

    atomic_store(&context.refill, true);
    (*queue)->Enqueue(queue, stereo, sizeof(stereo));
    (*queue)->Enqueue(queue, stereo, sizeof(stereo));
    play_state(play, SL_PLAYSTATE_PLAYING);
    sleep_ms(1000);
    const int calls = atomic_load(&context.calls);
    LOG("after 1 s of PLAYING: the callback ran: %s, never on the test thread: %s", yes(calls > 0),
        yes(!atomic_load(&context.on_test_thread)));
    DEVICE("after 1 s of PLAYING: %d callbacks of 10 ms", calls);
    atomic_store(&context.refill, false);
    play_state(play, SL_PLAYSTATE_PAUSED);
    sleep_ms(50);
    // Two buffers queued while paused, so that no callback changes the count before STOPPED.
    (*queue)->Clear(queue);
    (*queue)->Enqueue(queue, stereo, sizeof(stereo));
    (*queue)->Enqueue(queue, stereo, sizeof(stereo));
    play_state(play, SL_PLAYSTATE_STOPPED);
    queue_count(queue, "with 2 buffers queued while paused, after STOPPED");

    SLmillibel max = 1;
    LOG("GetMaxVolumeLevel: %s, %d", result_name((*volume)->GetMaxVolumeLevel(volume, &max)), max);
    SLmillibel level = 0;
    const SLresult set_level = (*volume)->SetVolumeLevel(volume, -600);
    LOG("SetVolumeLevel -600: %s, reads %s, %d", result_name(set_level),
        result_name((*volume)->GetVolumeLevel(volume, &level)), level);
    SLboolean mute = SL_BOOLEAN_FALSE;
    const SLresult set_mute = (*volume)->SetMute(volume, SL_BOOLEAN_TRUE);
    LOG("SetMute: %s, reads %s, %s", result_name(set_mute), result_name((*volume)->GetMute(volume, &mute)),
        mute ? "muted" : "not muted");
    (*player)->Destroy(player);
    LOG("the 16-bit player destroyed");
}

static void float_player(SLEngineItf engine, SLObjectItf mix) {
    SLDataLocator_BufferQueue queue_locator = {SL_DATALOCATOR_BUFFERQUEUE, 1};
    SLAndroidDataFormat_PCM_EX format = {SL_ANDROID_DATAFORMAT_PCM_EX, 1, SL_SAMPLINGRATE_48,
                                         SL_PCMSAMPLEFORMAT_FIXED_32, SL_PCMSAMPLEFORMAT_FIXED_32,
                                         SL_SPEAKER_FRONT_CENTER, SL_BYTEORDER_LITTLEENDIAN,
                                         SL_ANDROID_PCM_REPRESENTATION_FLOAT};
    SLDataSource source = {&queue_locator, &format};
    SLDataLocator_OutputMix mix_locator = {SL_DATALOCATOR_OUTPUTMIX, mix};
    SLDataSink sink = {&mix_locator, NULL};
    const SLInterfaceID ids[] = {SL_IID_BUFFERQUEUE, SL_IID_EFFECTSEND};
    const SLboolean required[] = {SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE};
    SLObjectItf player = NULL;
    LOG("CreateAudioPlayer, float PCM from a buffer queue: %s",
        result_name((*engine)->CreateAudioPlayer(engine, &player, &source, &sink, 2, ids, required)));
    if (player == NULL) return;
    LOG("Realize the float player: %s", result_name((*player)->Realize(player, SL_BOOLEAN_FALSE)));
    SLPlayItf play = NULL;
    SLBufferQueueItf queue = NULL;
    LOG("GetInterface PLAY %s, BUFFERQUEUE %s", result_name((*player)->GetInterface(player, SL_IID_PLAY, &play)),
        result_name((*player)->GetInterface(player, SL_IID_BUFFERQUEUE, &queue)));
    SLEffectSendItf send = NULL;
    const SLresult got_send = (*player)->GetInterface(player, SL_IID_EFFECTSEND, &send);
    SLmillibel direct = 1;
    const SLresult set_direct = send != NULL ? (*send)->SetDirectLevel(send, -300) : got_send;
    const SLresult get_direct = send != NULL ? (*send)->GetDirectLevel(send, &direct) : got_send;
    LOG("GetInterface EFFECTSEND %s, SetDirectLevel -300 %s, reads %s, %d", result_name(got_send),
        result_name(set_direct), result_name(get_direct), direct);
    if (play != NULL && queue != NULL) {
        struct queue context = {0};
        (*queue)->RegisterCallback(queue, on_float_buffer, &context);
        LOG("Enqueue float: %s", result_name((*queue)->Enqueue(queue, mono, sizeof(mono))));
        play_state(play, SL_PLAYSTATE_PLAYING);
        for (int i = 0; i < 100 && atomic_load(&context.calls) == 0; i++) sleep_ms(10);
        LOG("the float player's callback ran: %s", yes(atomic_load(&context.calls) > 0));
        play_state(play, SL_PLAYSTATE_STOPPED);
    }
    (*player)->Destroy(player);
    LOG("the float player destroyed");
}

static atomic_bool finished;

static void *run_tests(void *unused) {
    (void)unused;
    SLObjectItf engine_object = NULL;
    LOG("slCreateEngine: %s", result_name(slCreateEngine(&engine_object, 0, NULL, 0, NULL, NULL)));
    if (engine_object != NULL) {
        LOG("Realize the engine: %s", result_name((*engine_object)->Realize(engine_object, SL_BOOLEAN_FALSE)));
        SLEngineItf engine = NULL;
        LOG("GetInterface ENGINE: %s",
            result_name((*engine_object)->GetInterface(engine_object, SL_IID_ENGINE, &engine)));
        SLObjectItf mix = NULL;
        if (engine != NULL) LOG("CreateOutputMix: %s", result_name((*engine)->CreateOutputMix(engine, &mix, 0, NULL, NULL)));
        if (mix != NULL) {
            LOG("Realize the output mix: %s", result_name((*mix)->Realize(mix, SL_BOOLEAN_FALSE)));
            pcm_player(engine, mix);
            float_player(engine, mix);
            (*mix)->Destroy(mix);
        }
        (*engine_object)->Destroy(engine_object);
        LOG("the engine destroyed");
    }
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
