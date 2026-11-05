// audio-input-test (ADR-0064): a NativeActivity on native_app_glue whose test thread opens an AAudio input stream and
// an OpenSL ES audio recorder, and logs with the tag TEST_TAG what Android 15 answers: the open's result (without
// RECORD_AUDIO's grant it fails, with it the stream's values), the calls on the stream (a read before the start, the
// start, a read of frames, the stop), and the recorder (its creation, Realize, the interfaces, a queue of buffers that
// the recording fills and calls back, the position, the stop). What the microphone hears is not compared: the emulator
// delivers silence. What differs by device or by timing (the position and its events) goes to the tag
// TEST_TAG-device. At the end the test calls ANativeActivity_finish.
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
#include <aaudio/AAudio.h>
#include <android/log.h>
#include <android/native_activity.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "android_native_app_glue.h"

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, TEST_TAG, __VA_ARGS__)
#define DEVICE(...) __android_log_print(ANDROID_LOG_INFO, TEST_TAG "-device", __VA_ARGS__)

static atomic_bool finished;

static void sleep_ms(long milliseconds) {
    struct timespec time = {milliseconds / 1000, (milliseconds % 1000) * 1000000};
    nanosleep(&time, NULL);
}

static const char *yes(bool value) {
    return value ? "yes" : "no";
}

static const char *result_name(SLresult result) {
    static const char *const names[] = {
        "SUCCESS", "PRECONDITIONS_VIOLATED", "PARAMETER_INVALID", "MEMORY_FAILURE", "RESOURCE_ERROR",
        "RESOURCE_LOST", "IO_ERROR", "BUFFER_INSUFFICIENT", "CONTENT_CORRUPTED", "CONTENT_UNSUPPORTED",
        "CONTENT_NOT_FOUND", "PERMISSION_DENIED", "FEATURE_UNSUPPORTED", "INTERNAL_ERROR", "UNKNOWN_ERROR",
        "OPERATION_ABORTED", "CONTROL_LOST"};
    return result < sizeof(names) / sizeof(names[0]) ? names[result] : "another result";
}

// MARK: - AAudio

static void aaudio_input(void) {
    AAudioStreamBuilder *builder = NULL;
    LOG("AAudio_createStreamBuilder: %s", AAudio_convertResultToText(AAudio_createStreamBuilder(&builder)));
    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_INPUT);
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(builder, 1);
    AAudioStreamBuilder_setSampleRate(builder, 16000);
    AAudioStream *stream = NULL;
    const aaudio_result_t opened = AAudioStreamBuilder_openStream(builder, &stream);
    LOG("input openStream: %s, a stream %s", AAudio_convertResultToText(opened), yes(stream != NULL));
    AAudioStreamBuilder_delete(builder);
    if (stream == NULL) return;
    LOG("direction %d, channels %d, format %d, rate %d", AAudioStream_getDirection(stream),
        AAudioStream_getChannelCount(stream), AAudioStream_getFormat(stream), AAudioStream_getSampleRate(stream));
    LOG("state %s", AAudio_convertStreamStateToText(AAudioStream_getState(stream)));
    DEVICE("burst %d, capacity %d", AAudioStream_getFramesPerBurst(stream),
           AAudioStream_getBufferCapacityInFrames(stream));
    int16_t frames[160];
    memset(frames, 0x55, sizeof frames);
    LOG("write on an input stream: %d", AAudioStream_write(stream, frames, 10, 0));
    const aaudio_result_t early = AAudioStream_read(stream, frames, 160, 0);
    LOG("read before the start: %s",
        early >= 0 ? (early == 0 ? "0 frames" : "frames") : AAudio_convertResultToText(early));
    LOG("requestStart: %s", AAudio_convertResultToText(AAudioStream_requestStart(stream)));
    aaudio_stream_state_t state = AAUDIO_STREAM_STATE_UNKNOWN;
    AAudioStream_waitForStateChange(stream, AAUDIO_STREAM_STATE_STARTING, &state, 1000000000LL);
    LOG("state after the start %s", AAudio_convertStreamStateToText(state));
    const aaudio_result_t read = AAudioStream_read(stream, frames, 160, 2000000000LL);
    LOG("read 160 frames, 2 s: %d", read);
    LOG("frames read so far at least 160: %s", yes(AAudioStream_getFramesRead(stream) >= 160));
    LOG("requestStop: %s", AAudio_convertResultToText(AAudioStream_requestStop(stream)));
    AAudioStream_waitForStateChange(stream, AAUDIO_STREAM_STATE_STOPPING, &state, 1000000000LL);
    LOG("state after the stop %s", AAudio_convertStreamStateToText(state));
    LOG("close: %s", AAudio_convertResultToText(AAudioStream_close(stream)));
}

// MARK: - OpenSL ES

static atomic_int buffers_done;
static atomic_int positions;

static void on_buffer(SLAndroidSimpleBufferQueueItf queue, void *context) {
    (void)queue;
    (void)context;
    atomic_fetch_add(&buffers_done, 1);
}

static void on_record(SLRecordItf record, void *context, SLuint32 event) {
    (void)record;
    (void)context;
    if (event == SL_RECORDEVENT_HEADATNEWPOS) atomic_fetch_add(&positions, 1);
}

static void recorder(void) {
    SLObjectItf engine_object = NULL;
    LOG("slCreateEngine: %s", result_name(slCreateEngine(&engine_object, 0, NULL, 0, NULL, NULL)));
    if (engine_object == NULL) return;
    LOG("Realize the engine: %s", result_name((*engine_object)->Realize(engine_object, SL_BOOLEAN_FALSE)));
    SLEngineItf engine = NULL;
    (*engine_object)->GetInterface(engine_object, SL_IID_ENGINE, &engine);

    SLDataLocator_IODevice device = {SL_DATALOCATOR_IODEVICE, SL_IODEVICE_AUDIOINPUT, SL_DEFAULTDEVICEID_AUDIOINPUT,
                                     NULL};
    SLDataSource source = {&device, NULL};
    SLDataLocator_AndroidSimpleBufferQueue queue_locator = {SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, 3};
    SLDataFormat_PCM format = {SL_DATAFORMAT_PCM, 1, SL_SAMPLINGRATE_16, SL_PCMSAMPLEFORMAT_FIXED_16,
                               SL_PCMSAMPLEFORMAT_FIXED_16, SL_SPEAKER_FRONT_CENTER, SL_BYTEORDER_LITTLEENDIAN};
    SLDataSink sink = {&queue_locator, &format};
    const SLInterfaceID ids[] = {SL_IID_ANDROIDSIMPLEBUFFERQUEUE};
    const SLboolean required[] = {SL_BOOLEAN_TRUE};
    SLObjectItf recorder_object = NULL;
    LOG("CreateAudioRecorder: %s", result_name((*engine)->CreateAudioRecorder(engine, &recorder_object, &source, &sink,
                                                                               1, ids, required)));
    LOG("a recorder object: %s", yes(recorder_object != NULL));
    if (recorder_object != NULL) {
        LOG("Realize the recorder: %s", result_name((*recorder_object)->Realize(recorder_object, SL_BOOLEAN_FALSE)));
        SLRecordItf record = NULL;
        SLAndroidSimpleBufferQueueItf queue = NULL;
        LOG("GetInterface RECORD: %s",
            result_name((*recorder_object)->GetInterface(recorder_object, SL_IID_RECORD, &record)));
        LOG("GetInterface the buffer queue: %s", result_name((*recorder_object)->GetInterface(
                                                      recorder_object, SL_IID_ANDROIDSIMPLEBUFFERQUEUE, &queue)));
        if (record != NULL && queue != NULL) {
            static int16_t buffers[3][160];
            LOG("RegisterCallback of the queue: %s",
                result_name((*queue)->RegisterCallback(queue, on_buffer, NULL)));
            (*record)->RegisterCallback(record, on_record, NULL);
            (*record)->SetCallbackEventsMask(record, SL_RECORDEVENT_HEADATNEWPOS);
            (*record)->SetPositionUpdatePeriod(record, 50);
            for (int index = 0; index < 3; ++index) {
                LOG("Enqueue %d: %s", index,
                    result_name((*queue)->Enqueue(queue, buffers[index], sizeof buffers[index])));
            }
            LOG("a fourth Enqueue: %s", result_name((*queue)->Enqueue(queue, buffers[0], sizeof buffers[0])));
            SLuint32 state = 0;
            (*record)->GetRecordState(record, &state);
            LOG("record state before: %u", (unsigned)state);
            LOG("SetRecordState RECORDING: %s",
                result_name((*record)->SetRecordState(record, SL_RECORDSTATE_RECORDING)));
            for (int wait = 0; wait < 60 && atomic_load(&buffers_done) < 3; ++wait) sleep_ms(50);
            LOG("the three buffers were called back: %s", yes(atomic_load(&buffers_done) >= 3));
            DEVICE("a new position event came: %s", yes(atomic_load(&positions) >= 1));
            SLAndroidSimpleBufferQueueState queue_state;
            (*queue)->GetState(queue, &queue_state);
            LOG("queue count %u", (unsigned)queue_state.count);
            SLmillisecond position = 0;
            (*record)->GetPosition(record, &position);
            DEVICE("position at least 50 ms: %s", yes(position >= 50));
            LOG("SetRecordState STOPPED: %s", result_name((*record)->SetRecordState(record, SL_RECORDSTATE_STOPPED)));
            (*record)->GetRecordState(record, &state);
            LOG("record state after: %u", (unsigned)state);
        }
        (*recorder_object)->Destroy(recorder_object);
        LOG("the recorder destroyed");
    }
    (*engine_object)->Destroy(engine_object);
}

static void *run_tests(void *unused) {
    (void)unused;
    aaudio_input();
    recorder();
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
