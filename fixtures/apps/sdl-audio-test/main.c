// sdl-audio-test (the audio speed of an SDL2 game): a NativeActivity on native_app_glue whose test thread plays,
// with silence, the call sequences that SDL 2.30's two Android audio drivers make, and logs with the tag
// "sdl-audio-test" what the platform must give them, and with "sdl-audio-test-device" the counts and times.
//   - SDL_aaudio.c: a builder with the rate, the channel count and the format (I16), no performance mode and no data
//     callback; the stream's values read back; requestStart; then the audio thread, whose WaitDevice is the default
//     no-op, writes spec.size bytes, 1024 frames, with a timeout of 1 ms again and again, and ignores a positive
//     result. On Android a timeout above 0 makes the write fully blocking (AudioStreamTrack.cpp:458-460), which
//     paces the thread at the stream's rate: no write takes fewer frames than it was given, and the frames mixed
//     in 2 s are the rate's (plus the buffer filled at first).
//   - SDL_openslES.c: an engine and an output mix whose optional VOLUME interface, a player of 16-bit stereo PCM in
//     milliHz with a simple buffer queue of 2 buffers; the thread mixes into a buffer, enqueues it and waits for the
//     semaphore that the buffer-finished callback posts (initial count 1). The mixed frames of 2 s are the rate's too.
// At the end the test calls ANativeActivity_finish.
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
#include <aaudio/AAudio.h>
#include <android/log.h>
#include <android/native_activity.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "android_native_app_glue.h"

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "sdl-audio-test", __VA_ARGS__)
#define DEVICE(...) __android_log_print(ANDROID_LOG_INFO, "sdl-audio-test-device", __VA_ARGS__)

static const int64_t kSecond = 1000000000;
static const int kRate = 48000;
static atomic_bool finished;

static int64_t now(void) {
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return (int64_t)time.tv_sec * kSecond + time.tv_nsec;
}

static const char *yes(bool value) {
    return value ? "yes" : "no";
}

static void aaudio_driver(void) {
    AAudioStreamBuilder *builder = NULL;
    if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK) {
        LOG("aaudio: no builder");
        return;
    }
    AAudioStreamBuilder_setSampleRate(builder, kRate);
    AAudioStreamBuilder_setChannelCount(builder, 2);
    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
    AAudioStream *stream = NULL;
    const aaudio_result_t opened = AAudioStreamBuilder_openStream(builder, &stream);
    AAudioStreamBuilder_delete(builder);
    if (opened != AAUDIO_OK) {
        LOG("aaudio: open failed");
        return;
    }
    LOG("aaudio: opened, rate %d, channels %d, format %s", AAudioStream_getSampleRate(stream),
        AAudioStream_getChannelCount(stream),
        AAudioStream_getFormat(stream) == AAUDIO_FORMAT_PCM_I16 ? "I16" : "other");
    const int32_t burst = AAudioStream_getFramesPerBurst(stream);
    const int32_t capacity = AAudioStream_getBufferCapacityInFrames(stream);
    const int32_t size = AAudioStream_getBufferSizeInFrames(stream);
    LOG("aaudio: the buffer size starts at the capacity: %s", yes(size == capacity));
    DEVICE("aaudio: burst %d, capacity %d, buffer size %d", burst, capacity, size);
    if (AAudioStream_requestStart(stream) != AAUDIO_OK) {
        LOG("aaudio: no start");
        return;
    }
    const int chunk = 1024;
    int16_t *silence = calloc((size_t)chunk * 2, sizeof(int16_t));
    int writes = 0, partial = 0;
    int64_t accepted = 0;
    const int64_t begin = now();
    while (now() - begin < 2 * kSecond) {
        const aaudio_result_t result = AAudioStream_write(stream, silence, chunk, 1000000);
        ++writes;
        if (result < chunk) ++partial;
        if (result > 0) accepted += result;
    }
    const double seconds = (double)(now() - begin) / (double)kSecond;
    const double mixed_rate = (double)writes * chunk / seconds;
    DEVICE("aaudio: %d writes of %d frames in %.2f s, %d partial, %lld frames accepted, %.0f mixed frames/s, xruns %d",
           writes, chunk, seconds, partial, (long long)accepted, mixed_rate, AAudioStream_getXRunCount(stream));
    LOG("aaudio: 2 s of SDL's writes of 1024 frames with a timeout of 1 ms: none took fewer frames than given: %s",
        yes(partial == 0));
    LOG("aaudio: the thread mixed at about the stream's rate (at most 25 percent over): %s",
        yes(mixed_rate > kRate * 0.9 && mixed_rate < kRate * 1.25));
    free(silence);
    AAudioStream_requestStop(stream);
    AAudioStream_close(stream);
}

static sem_t play_semaphore;

static void buffer_done(SLAndroidSimpleBufferQueueItf queue, void *context) {
    (void)queue;
    (void)context;
    sem_post(&play_semaphore);
}

static void opensl_driver(int samples) {
    SLObjectItf engine_object = NULL, mix = NULL, player = NULL;
    SLEngineItf engine = NULL;
    if (slCreateEngine(&engine_object, 0, NULL, 0, NULL, NULL) != SL_RESULT_SUCCESS ||
        (*engine_object)->Realize(engine_object, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS ||
        (*engine_object)->GetInterface(engine_object, SL_IID_ENGINE, &engine) != SL_RESULT_SUCCESS) {
        LOG("opensl: no engine");
        return;
    }
    const SLInterfaceID mix_ids[1] = {SL_IID_VOLUME};
    const SLboolean mix_required[1] = {SL_BOOLEAN_FALSE};
    if ((*engine)->CreateOutputMix(engine, &mix, 1, mix_ids, mix_required) != SL_RESULT_SUCCESS ||
        (*mix)->Realize(mix, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS) {
        LOG("opensl: no output mix");
        return;
    }
    SLDataLocator_AndroidSimpleBufferQueue queue_locator = {SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, 2};
    SLDataFormat_PCM format = {SL_DATAFORMAT_PCM, 2, (SLuint32)kRate * 1000, SL_PCMSAMPLEFORMAT_FIXED_16,
                               SL_PCMSAMPLEFORMAT_FIXED_16, SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT,
                               SL_BYTEORDER_LITTLEENDIAN};
    SLDataSource source = {&queue_locator, &format};
    SLDataLocator_OutputMix mix_locator = {SL_DATALOCATOR_OUTPUTMIX, mix};
    SLDataSink sink = {&mix_locator, NULL};
    const SLInterfaceID ids[2] = {SL_IID_ANDROIDSIMPLEBUFFERQUEUE, SL_IID_VOLUME};
    const SLboolean required[2] = {SL_BOOLEAN_TRUE, SL_BOOLEAN_FALSE};
    SLPlayItf play = NULL;
    SLAndroidSimpleBufferQueueItf queue = NULL;
    if ((*engine)->CreateAudioPlayer(engine, &player, &source, &sink, 2, ids, required) != SL_RESULT_SUCCESS ||
        (*player)->Realize(player, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS ||
        (*player)->GetInterface(player, SL_IID_PLAY, &play) != SL_RESULT_SUCCESS ||
        (*player)->GetInterface(player, SL_IID_ANDROIDSIMPLEBUFFERQUEUE, &queue) != SL_RESULT_SUCCESS ||
        (*queue)->RegisterCallback(queue, buffer_done, NULL) != SL_RESULT_SUCCESS) {
        LOG("opensl: no player");
        return;
    }
    sem_init(&play_semaphore, 0, 1);
    int16_t *buffers[2] = {calloc((size_t)samples * 2, sizeof(int16_t)), calloc((size_t)samples * 2, sizeof(int16_t))};
    if ((*play)->SetPlayState(play, SL_PLAYSTATE_PLAYING) != SL_RESULT_SUCCESS) {
        LOG("opensl: no play");
        return;
    }
    int next = 0, failed = 0;
    int64_t mixed = 0;
    const int64_t begin = now();
    while (now() - begin < 2 * kSecond) {
        // SDL_RunAudio: mix, PlayDevice (Enqueue), WaitDevice (the semaphore).
        if ((*queue)->Enqueue(queue, buffers[next], (SLuint32)samples * 4) != SL_RESULT_SUCCESS) {
            ++failed;
            sem_post(&play_semaphore);
        } else {
            mixed += samples;
        }
        next ^= 1;
        struct timespec limit;
        clock_gettime(CLOCK_REALTIME, &limit);
        limit.tv_sec += 1;
        if (sem_timedwait(&play_semaphore, &limit) != 0) break;
    }
    const double seconds = (double)(now() - begin) / (double)kSecond;
    const double mixed_rate = (double)mixed / seconds;
    DEVICE("opensl: %d-frame buffers: %lld frames in %.2f s, %.0f mixed frames/s, %d enqueue failures", samples,
           (long long)mixed, seconds, mixed_rate, failed);
    LOG("opensl: %d-frame buffers: the thread was paced at about the player's rate (at most 25 percent over): %s",
        samples, yes(mixed_rate > kRate * 0.9 && mixed_rate < kRate * 1.25));
    (*play)->SetPlayState(play, SL_PLAYSTATE_STOPPED);
    (*player)->Destroy(player);
    (*mix)->Destroy(mix);
    (*engine_object)->Destroy(engine_object);
    free(buffers[0]);
    free(buffers[1]);
    sem_destroy(&play_semaphore);
}

static void *run_tests(void *argument) {
    (void)argument;
    aaudio_driver();
    opensl_driver(512);
    opensl_driver(1024);
    LOG("done");
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
