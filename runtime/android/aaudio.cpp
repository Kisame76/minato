// libaaudio.so (ADR-0036, decision 6), written for Minato from the NDK's AAudio.h and Android 15's answers
// (fixtures/apps/aaudio-test): output streams on the runtime's audio streams (guest_interface.h), which keep the ring
// and the state in host memory. A write converts the app's frames to float32 and hands them over with audio_write,
// which waits there until its deadline. A stream with a data or an error callback has a thread of its own from the
// open to the release: while the stream plays, the thread calls the data callback whenever the buffer has room and
// keeps the frames that did not fit for the next room, in the output's audio workgroup (audio_join); a call that
// does not answer CONTINUE stops the stream, and its frames are not played, as AudioStreamLegacy's onMoreData hands
// back none [from memory of AOSP, not read again]. It calls the error callback once the stream is DISCONNECTED.
// Input streams (ADR-0064) read the microphone through audio_read: AAudioStream_read converts the captured float32
// frames to the app's format, and a stream with a data callback has a thread that fills the callback's frames and
// calls it. Opening one needs the microphone grant (AAUDIO_ERROR_UNAVAILABLE without it, as Android's). EXCLUSIVE
// becomes SHARED, and MMAP is never used.
#include <aaudio/AAudio.h>
#include <android/log.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "APKRuntime/guest_interface.h"

#define AAUDIO_LOG(priority, ...) __android_log_print(ANDROID_LOG_##priority, "AAudio", __VA_ARGS__)

struct AAudioStreamBuilderStruct {
    int32_t device_id;
    int32_t sample_rate;
    int32_t channel_count;
    aaudio_channel_mask_t channel_mask;
    aaudio_format_t format;
    aaudio_sharing_mode_t sharing_mode;
    aaudio_direction_t direction;
    aaudio_performance_mode_t performance_mode;
    aaudio_usage_t usage;
    aaudio_content_type_t content_type;
    aaudio_input_preset_t input_preset;
    aaudio_allowed_capture_policy_t capture_policy;
    aaudio_session_id_t session_id;
    aaudio_spatialization_behavior_t spatialization_behavior;
    bool content_spatialized;
    bool privacy_sensitive;
    int32_t frames_per_data_callback;
    AAudioStream_dataCallback data_callback;
    void* data_user;
    AAudioStream_errorCallback error_callback;
    void* error_user;
    int32_t buffer_capacity;
};

struct AAudioStreamStruct {
    uint64_t handle;  // the runtime's stream
    bool input;       // an input stream (ADR-0064): read and the data callback take frames from the microphone
    bool released;    // AAudioStream_release closed it; `last` holds its final status
    apkrt_audio_status last;
    // What the builder asked for and the open gave.
    int32_t sample_rate;
    int32_t channels;
    int32_t burst;
    int32_t capacity;
    int32_t hardware_rate;
    aaudio_format_t format;
    aaudio_channel_mask_t channel_mask;
    aaudio_performance_mode_t performance_mode;
    aaudio_usage_t usage;
    aaudio_content_type_t content_type;
    aaudio_input_preset_t input_preset;
    aaudio_allowed_capture_policy_t capture_policy;
    aaudio_session_id_t session_id;
    aaudio_spatialization_behavior_t spatialization_behavior;
    bool content_spatialized;
    bool privacy_sensitive;
    int32_t frames_per_data_callback;
    AAudioStream_dataCallback data_callback;
    void* data_user;
    AAudioStream_errorCallback error_callback;
    void* error_user;
    // The stream's thread, for the callbacks.
    bool has_thread;
    pthread_t thread;
    pthread_mutex_t gate;   // openStream holds it until the stream is whole; the thread starts once it is free
    pthread_mutex_t feed;   // the thread's writes of callback frames, against flushes and stops
    uint32_t drops;         // under `feed`: the flushes and stops so far, which drop the frames not handed over
    bool claimed;           // the thread's: the data callback under way claimed its frames
    uint32_t claimed_at;    // under `feed`: `drops` at that claim
    int32_t per_call;       // the frames of a data callback
    int32_t given;          // the thread's: the frames of the call under way that are audio, -1 for all of them
    void* callback_frames;  // per_call frames in the app's format
    float* floats;          // chunk frames, converted
    int32_t chunk;
};

namespace {

constexpr int64_t kForever = INT64_MAX;
constexpr int32_t kMostChannels = 28;  // Android's FCC_LIMIT; the runtime takes APKRT_AUDIO_CHANNELS

aaudio_policy_t mmap_policy = AAUDIO_UNSPECIFIED;
aaudio_session_id_t last_session_id = 0;

int64_t now(clockid_t clock) {
    timespec time;
    clock_gettime(clock, &time);
    return time.tv_sec * 1'000'000'000LL + time.tv_nsec;
}

/// The deadline `nanoseconds` (above 0) from now on CLOCK_MONOTONIC, saturated.
int64_t deadline_after(int64_t nanoseconds) {
    const int64_t start = now(CLOCK_MONOTONIC);
    return nanoseconds > kForever - start ? kForever : start + nanoseconds;
}

bool passed(int64_t deadline) {
    return deadline <= 0 || deadline <= now(CLOCK_MONOTONIC);
}

int64_t smaller(int64_t a, int64_t b) {
    return a < b ? a : b;
}

bool released(const AAudioStream* stream) {
    return __atomic_load_n(&stream->released, __ATOMIC_ACQUIRE);
}

/// A private syscall with every argument as the 64 bits the runtime reads: syscall() takes varargs, where a
/// narrower integer leaves the upper half of its register undefined.
long call(int number, uint64_t a0, uint64_t a1 = 0, uint64_t a2 = 0, uint64_t a3 = 0) {
    return syscall(number, a0, a1, a2, a3);
}

uint64_t bits(const void* pointer) {
    return reinterpret_cast<uintptr_t>(pointer);
}

uint64_t bits(int64_t value) {
    return static_cast<uint64_t>(value);
}

aaudio_result_t result_of(int error) {
    switch (error) {
        case EPERM:
        case EBADF:
            return AAUDIO_ERROR_INVALID_STATE;
        case EPIPE:
            return AAUDIO_ERROR_DISCONNECTED;
        case EINVAL:
            return AAUDIO_ERROR_ILLEGAL_ARGUMENT;
        case ENOMEM:
            return AAUDIO_ERROR_NO_MEMORY;
        case EMFILE:
            return AAUDIO_ERROR_NO_FREE_HANDLES;
        case EACCES:
        case ENODEV:
        case EIO:
            return AAUDIO_ERROR_UNAVAILABLE;
        case ETIMEDOUT:
            return AAUDIO_ERROR_TIMEOUT;
        default:
            return AAUDIO_ERROR_INTERNAL;
    }
}

/// The stream's status: the runtime's while it is open, the last one after the release.
bool status_of(const AAudioStream* stream, apkrt_audio_status* status) {
    if (released(stream)) {
        *status = stream->last;
        return false;
    }
    if (call(APKRT_NR_AUDIO_STATUS, stream->handle, bits(status)) == 0) return true;
    *status = stream->last;
    return false;
}

int32_t sample_bytes(aaudio_format_t format) {
    switch (format) {
        case AAUDIO_FORMAT_PCM_I16:
            return 2;
        case AAUDIO_FORMAT_PCM_I24_PACKED:
            return 3;
        default:
            return 4;
    }
}

/// `samples` samples in `format` as float32, as AAudio's own conversions scale them.
void to_float(aaudio_format_t format, const void* source, float* target, int64_t samples) {
    switch (format) {
        case AAUDIO_FORMAT_PCM_I16: {
            const auto* from = static_cast<const int16_t*>(source);
            for (int64_t index = 0; index < samples; ++index) target[index] = from[index] * (1.0f / 32768.0f);
            break;
        }
        case AAUDIO_FORMAT_PCM_I24_PACKED: {
            const auto* from = static_cast<const uint8_t*>(source);
            for (int64_t index = 0; index < samples; ++index) {
                const uint32_t bits = (uint32_t{from[3 * index]} << 8) | (uint32_t{from[3 * index + 1]} << 16) |
                                      (uint32_t{from[3 * index + 2]} << 24);
                target[index] = static_cast<float>(static_cast<int32_t>(bits) >> 8) * (1.0f / 8388608.0f);
            }
            break;
        }
        case AAUDIO_FORMAT_PCM_I32: {
            const auto* from = static_cast<const int32_t*>(source);
            for (int64_t index = 0; index < samples; ++index) {
                target[index] = static_cast<float>(from[index]) * (1.0f / 2147483648.0f);
            }
            break;
        }
        default:
            memcpy(target, source, static_cast<size_t>(samples) * sizeof(float));
            break;
    }
}

/// A float rounded to the nearest integer, halves away from zero.
int32_t rounded(float value) {
    return static_cast<int32_t>(value + (value >= 0 ? 0.5f : -0.5f));
}

/// `samples` float samples as `format`, as AAudio's own conversions scale them.
void from_float(aaudio_format_t format, const float* source, void* target, int64_t samples) {
    switch (format) {
        case AAUDIO_FORMAT_PCM_I16: {
            auto* to = static_cast<int16_t*>(target);
            for (int64_t index = 0; index < samples; ++index) {
                const float value = source[index] * 32768.0f;
                to[index] = static_cast<int16_t>(value >= 32767.0f    ? 32767
                                                 : value <= -32768.0f ? -32768
                                                                      : rounded(value));
            }
            break;
        }
        case AAUDIO_FORMAT_PCM_I24_PACKED: {
            auto* to = static_cast<uint8_t*>(target);
            for (int64_t index = 0; index < samples; ++index) {
                const float value = source[index] * 8388608.0f;
                const int32_t clamped =
                    value >= 8388607.0f ? 8388607 : value <= -8388608.0f ? -8388608 : rounded(value);
                to[3 * index] = static_cast<uint8_t>(clamped);
                to[3 * index + 1] = static_cast<uint8_t>(clamped >> 8);
                to[3 * index + 2] = static_cast<uint8_t>(clamped >> 16);
            }
            break;
        }
        case AAUDIO_FORMAT_PCM_I32: {
            auto* to = static_cast<int32_t*>(target);
            for (int64_t index = 0; index < samples; ++index) {
                const double value = static_cast<double>(source[index]) * 2147483648.0;
                to[index] = value >= 2147483647.0    ? INT32_MAX
                            : value <= -2147483648.0 ? INT32_MIN
                                                     : static_cast<int32_t>(value + (value >= 0 ? 0.5 : -0.5));
            }
            break;
        }
        default:
            memcpy(target, source, static_cast<size_t>(samples) * sizeof(float));
            break;
    }
}

/// Reads `count` frames of an input stream into `target` as the stream's format, waiting until `deadline`: the
/// frames read, or -errno when none were.
int64_t take_in(AAudioStream* stream, void* target, int64_t count, int64_t deadline) {
    auto* bytes = static_cast<uint8_t*>(target);
    const int64_t frame_bytes = int64_t{sample_bytes(stream->format)} * stream->channels;
    int64_t done = 0;
    while (done < count) {
        const int64_t part = smaller(count - done, stream->chunk);
        const long result = call(APKRT_NR_AUDIO_READ, stream->handle, bits(stream->floats), bits(part), bits(deadline));
        if (result < 0) {
            if (errno == EINTR && !passed(deadline)) continue;
            return done > 0 ? done : -errno;
        }
        from_float(stream->format, stream->floats, bytes + done * frame_bytes, result * stream->channels);
        done += result;
        if (result < part) break;  // the deadline passed, or the stream does not capture
    }
    return done;
}

/// The thread of an input stream with callbacks: while the stream captures, it fills per_call frames from the
/// microphone and hands them to the data callback; a callback that does not continue stops the stream.
void* run_input_callbacks(AAudioStream* stream) {
    bool reported = false;
    int64_t filled = 0;  // frames of the next callback read so far
    while (true) {
        apkrt_audio_status status;
        if (call(APKRT_NR_AUDIO_STATUS, stream->handle, bits(&status)) != 0) break;  // released
        if (status.state == AAUDIO_STREAM_STATE_DISCONNECTED && !reported) {
            reported = true;
            if (stream->error_callback != nullptr) {
                stream->error_callback(stream, stream->error_user, AAUDIO_ERROR_DISCONNECTED);
            }
        }
        const bool captures = stream->data_callback != nullptr && (status.state == AAUDIO_STREAM_STATE_STARTING ||
                                                                   status.state == AAUDIO_STREAM_STATE_STARTED);
        if (captures) {
            // In slices, so that a pause, a stop or the release is seen while the frames come.
            const int64_t frame_bytes = int64_t{sample_bytes(stream->format)} * stream->channels;
            const int64_t got = take_in(stream, static_cast<uint8_t*>(stream->callback_frames) + filled * frame_bytes,
                                        stream->per_call - filled, deadline_after(20'000'000));
            if (got < 0) {
                if (errno == EBADF) break;
                continue;  // disconnected: the state says so at the top
            }
            filled += got;
            if (filled < stream->per_call) continue;  // the slice ended: look at the state again, keep what came
            filled = 0;
            if (stream->data_callback(stream, stream->data_user, stream->callback_frames, stream->per_call) !=
                AAUDIO_CALLBACK_RESULT_CONTINUE) {
                call(APKRT_NR_AUDIO_CONTROL, stream->handle, APKRT_AUDIO_STOP);
            }
            continue;
        }
        if (call(APKRT_NR_AUDIO_WAIT, stream->handle, status.sequence, bits(kForever)) != 0 && errno == EBADF) break;
    }
    return nullptr;
}

/// Hands `count` float frames to the runtime, waiting until `deadline`: the frames taken, or -errno when none were.
/// With `block`, the wait has no deadline while the stream plays: AudioStreamTrack::write makes any timeout above 0 a
/// blocking AudioTrack::write (AudioStreamTrack.cpp:458-460, android-15.0.0_r1: "TODO add timeout to AudioTrack",
/// blocking = timeoutNanoseconds > 0), which waits for room until everything is written. A stream that does not
/// play (paused, stopped, flushed or not started) has no room coming, so its write returns what fitted at once, as
/// the emulator's does (aaudio-test: a write of twice the capacity while paused, timeout 50 ms, took 0 ms).
int64_t hand_over(AAudioStream* stream, const float* frames, int64_t count, int64_t deadline, bool block = false) {
    int64_t taken = 0;
    while (taken < count) {
        // A blocking wait goes in slices, so that a pause or a stop during it is seen.
        const int64_t until = block ? deadline_after(20'000'000) : deadline;
        const long result = call(APKRT_NR_AUDIO_WRITE, stream->handle, bits(frames + taken * stream->channels),
                                 bits(count - taken), bits(until));
        if (result < 0) {
            if (errno != EINTR) return taken > 0 ? taken : -errno;
            if (passed(until)) break;  // a handler without SA_RESTART ran: wait on until the deadline
            continue;
        }
        taken += result;
        if (taken >= count) break;
        if (block) {
            apkrt_audio_status status;
            if (call(APKRT_NR_AUDIO_STATUS, stream->handle, bits(&status)) != 0) break;
            if (status.state != APKRT_AUDIO_STARTING && status.state != APKRT_AUDIO_STARTED) break;
            continue;
        }
        if (passed(deadline)) break;
    }
    return taken;
}

/// The thread of a stream with callbacks, from the open to the release. While the stream plays, it calls the data
/// callback whenever the buffer has room, and hands over the frames that fit; the rest waits for the next room, as
/// AAudio's block adapter keeps it, unless a flush or a stop drops it first. One during the call drops its frames,
/// unless the callback claimed them after it (minato_AAudioStream_claimCallbackFrames).
void* run_callbacks(void* argument) {
    auto* stream = static_cast<AAudioStream*>(argument);
    pthread_mutex_lock(&stream->gate);  // free once openStream has given the app the stream
    pthread_mutex_unlock(&stream->gate);
    if (stream->input) return run_input_callbacks(stream);
    bool reported = false;  // the error callback ran
    bool playing = false;
    const float* pending = nullptr;  // converted frames the buffer had no room for yet
    int64_t pending_count = 0;
    uint32_t drops = 0;  // stream->drops as the thread saw it last
    while (true) {
        apkrt_audio_status status;
        if (call(APKRT_NR_AUDIO_STATUS, stream->handle, bits(&status)) != 0) break;  // released
        if (status.state == AAUDIO_STREAM_STATE_DISCONNECTED && !reported) {
            reported = true;
            if (stream->error_callback != nullptr) {
                stream->error_callback(stream, stream->error_user, AAUDIO_ERROR_DISCONNECTED);
            }
        }
        const bool plays = stream->data_callback != nullptr && (status.state == AAUDIO_STREAM_STATE_STARTING ||
                                                                status.state == AAUDIO_STREAM_STATE_STARTED);
        // Each run joins the output's workgroup again: the host's engine may have started anew in between.
        if (plays && !playing) call(APKRT_NR_AUDIO_JOIN, stream->handle, 1);
        playing = plays;
        if (playing && status.buffer_size - (status.frames_written - status.frames_read) > 0) {
            pthread_mutex_lock(&stream->feed);
            if (stream->drops != drops) {
                drops = stream->drops;
                pending_count = 0;
            }
            if (pending_count > 0) {
                const long taken = call(APKRT_NR_AUDIO_WRITE, stream->handle, bits(pending), bits(pending_count));
                if (taken > 0) {
                    pending += taken * stream->channels;
                    pending_count -= taken;
                }
            }
            pthread_mutex_unlock(&stream->feed);
            if (pending_count > 0) continue;
            stream->claimed = false;
            stream->given = -1;
            const aaudio_data_callback_result_t result =
                stream->data_callback(stream, stream->data_user, stream->callback_frames, stream->per_call);
            if (result != AAUDIO_CALLBACK_RESULT_CONTINUE) {
                // As AudioStreamLegacy: the frames of that call are not played, and the stream stops; the callback
                // comes again only after the next start.
                call(APKRT_NR_AUDIO_CONTROL, stream->handle, APKRT_AUDIO_STOP);
                continue;
            }
            const int32_t audio =
                stream->given >= 0 && stream->given < stream->per_call ? stream->given : stream->per_call;
            const float* frames = static_cast<const float*>(stream->callback_frames);
            if (stream->format != AAUDIO_FORMAT_PCM_FLOAT) {
                to_float(stream->format, stream->callback_frames, stream->floats, int64_t{audio} * stream->channels);
                frames = stream->floats;
            }
            pthread_mutex_lock(&stream->feed);
            if (stream->claimed) drops = stream->claimed_at;
            if (stream->drops == drops && audio > 0) {  // no flush or stop came during the callback, or after its claim
                const long taken = call(APKRT_NR_AUDIO_WRITE, stream->handle, bits(frames), bits(audio));
                const int64_t took = taken > 0 ? taken : 0;
                pending = frames + took * stream->channels;
                pending_count = audio - took;
            }
            pthread_mutex_unlock(&stream->feed);
            // A source with no frames to give is asked again soon, and not at once.
            if (audio == 0) call(APKRT_NR_AUDIO_WAIT, stream->handle, status.sequence, bits(deadline_after(1'000'000)));
            continue;
        }
        if (call(APKRT_NR_AUDIO_WAIT, stream->handle, status.sequence, bits(kForever)) != 0 && errno == EBADF) break;
    }
    return nullptr;
}

bool on_own_thread(const AAudioStream* stream) {
    return stream->has_thread && pthread_equal(pthread_self(), stream->thread);
}

/// Closes the runtime's stream and ends the stream's thread; the stream keeps its values.
void release(AAudioStream* stream) {
    if (released(stream)) return;
    apkrt_audio_status status;
    if (call(APKRT_NR_AUDIO_STATUS, stream->handle, bits(&status)) == 0) stream->last = status;
    call(APKRT_NR_AUDIO_CLOSE, stream->handle);
    stream->last.state = AAUDIO_STREAM_STATE_CLOSING;
    __atomic_store_n(&stream->released, true, __ATOMIC_RELEASE);
    if (stream->has_thread) {
        pthread_join(stream->thread, nullptr);
        stream->has_thread = false;
    }
}

void free_stream(AAudioStream* stream) {
    pthread_mutex_destroy(&stream->gate);
    pthread_mutex_destroy(&stream->feed);
    free(stream->callback_frames);
    free(stream->floats);
    free(stream);
}

aaudio_result_t command(AAudioStream* stream, int which) {
    if (released(stream)) return AAUDIO_ERROR_INVALID_STATE;
    // A flush or a stop drops the callback frames the buffer has not taken yet (run_callbacks).
    const bool drops = which == APKRT_AUDIO_FLUSH || which == APKRT_AUDIO_STOP;
    if (drops) {
        pthread_mutex_lock(&stream->feed);
        ++stream->drops;
    }
    const long result = call(APKRT_NR_AUDIO_CONTROL, stream->handle, bits(which));
    const int error = errno;
    if (drops) pthread_mutex_unlock(&stream->feed);
    return result == 0 ? AAUDIO_OK : result_of(error);
}

}  // namespace

extern "C" {

// MARK: - Texts and policies

const char* AAudio_convertResultToText(aaudio_result_t returnCode) {
#define AAUDIO_RESULT_TEXT(name) \
    case name:                   \
        return #name
    switch (returnCode) {
        AAUDIO_RESULT_TEXT(AAUDIO_OK);
        AAUDIO_RESULT_TEXT(AAUDIO_ERROR_DISCONNECTED);
        AAUDIO_RESULT_TEXT(AAUDIO_ERROR_ILLEGAL_ARGUMENT);
        AAUDIO_RESULT_TEXT(AAUDIO_ERROR_INTERNAL);
        AAUDIO_RESULT_TEXT(AAUDIO_ERROR_INVALID_STATE);
        AAUDIO_RESULT_TEXT(AAUDIO_ERROR_INVALID_HANDLE);
        AAUDIO_RESULT_TEXT(AAUDIO_ERROR_UNIMPLEMENTED);
        AAUDIO_RESULT_TEXT(AAUDIO_ERROR_UNAVAILABLE);
        AAUDIO_RESULT_TEXT(AAUDIO_ERROR_NO_FREE_HANDLES);
        AAUDIO_RESULT_TEXT(AAUDIO_ERROR_NO_MEMORY);
        AAUDIO_RESULT_TEXT(AAUDIO_ERROR_NULL);
        AAUDIO_RESULT_TEXT(AAUDIO_ERROR_TIMEOUT);
        AAUDIO_RESULT_TEXT(AAUDIO_ERROR_WOULD_BLOCK);
        AAUDIO_RESULT_TEXT(AAUDIO_ERROR_INVALID_FORMAT);
        AAUDIO_RESULT_TEXT(AAUDIO_ERROR_OUT_OF_RANGE);
        AAUDIO_RESULT_TEXT(AAUDIO_ERROR_NO_SERVICE);
        AAUDIO_RESULT_TEXT(AAUDIO_ERROR_INVALID_RATE);
    }
#undef AAUDIO_RESULT_TEXT
    return "Unrecognized AAudio error.";
}

const char* AAudio_convertStreamStateToText(aaudio_stream_state_t state) {
#define AAUDIO_STATE_TEXT(name) \
    case name:                  \
        return #name
    switch (state) {
        AAUDIO_STATE_TEXT(AAUDIO_STREAM_STATE_UNINITIALIZED);
        AAUDIO_STATE_TEXT(AAUDIO_STREAM_STATE_UNKNOWN);
        AAUDIO_STATE_TEXT(AAUDIO_STREAM_STATE_OPEN);
        AAUDIO_STATE_TEXT(AAUDIO_STREAM_STATE_STARTING);
        AAUDIO_STATE_TEXT(AAUDIO_STREAM_STATE_STARTED);
        AAUDIO_STATE_TEXT(AAUDIO_STREAM_STATE_PAUSING);
        AAUDIO_STATE_TEXT(AAUDIO_STREAM_STATE_PAUSED);
        AAUDIO_STATE_TEXT(AAUDIO_STREAM_STATE_FLUSHING);
        AAUDIO_STATE_TEXT(AAUDIO_STREAM_STATE_FLUSHED);
        AAUDIO_STATE_TEXT(AAUDIO_STREAM_STATE_STOPPING);
        AAUDIO_STATE_TEXT(AAUDIO_STREAM_STATE_STOPPED);
        AAUDIO_STATE_TEXT(AAUDIO_STREAM_STATE_CLOSING);
        AAUDIO_STATE_TEXT(AAUDIO_STREAM_STATE_CLOSED);
        AAUDIO_STATE_TEXT(AAUDIO_STREAM_STATE_DISCONNECTED);
    }
#undef AAUDIO_STATE_TEXT
    return "Unrecognized AAudio state.";
}

aaudio_result_t AAudio_setMMapPolicy(aaudio_policy_t policy) {
    switch (policy) {
        case AAUDIO_UNSPECIFIED:
        case AAUDIO_POLICY_NEVER:
        case AAUDIO_POLICY_AUTO:
        case AAUDIO_POLICY_ALWAYS:
            __atomic_store_n(&mmap_policy, policy, __ATOMIC_RELAXED);  // taken; MMAP is never used
            return AAUDIO_OK;
        default:
            return AAUDIO_ERROR_ILLEGAL_ARGUMENT;
    }
}

aaudio_policy_t AAudio_getMMapPolicy() {
    return __atomic_load_n(&mmap_policy, __ATOMIC_RELAXED);
}

// MARK: - The builder

aaudio_result_t AAudio_createStreamBuilder(AAudioStreamBuilder** builder) {
    if (builder == nullptr) return AAUDIO_ERROR_NULL;
    auto* made = static_cast<AAudioStreamBuilder*>(calloc(1, sizeof(AAudioStreamBuilder)));
    if (made == nullptr) return AAUDIO_ERROR_NO_MEMORY;
    made->sharing_mode = AAUDIO_SHARING_MODE_SHARED;
    made->direction = AAUDIO_DIRECTION_OUTPUT;
    made->performance_mode = AAUDIO_PERFORMANCE_MODE_NONE;
    made->usage = AAUDIO_USAGE_MEDIA;
    made->content_type = AAUDIO_CONTENT_TYPE_MUSIC;
    made->input_preset = AAUDIO_INPUT_PRESET_VOICE_RECOGNITION;
    made->capture_policy = AAUDIO_ALLOW_CAPTURE_BY_ALL;
    made->session_id = AAUDIO_SESSION_ID_NONE;
    made->spatialization_behavior = AAUDIO_SPATIALIZATION_BEHAVIOR_AUTO;
    *builder = made;
    return AAUDIO_OK;
}

aaudio_result_t AAudioStreamBuilder_delete(AAudioStreamBuilder* builder) {
    if (builder == nullptr) return AAUDIO_ERROR_NULL;
    free(builder);
    return AAUDIO_OK;
}

void AAudioStreamBuilder_setDeviceId(AAudioStreamBuilder* builder, int32_t deviceId) {
    builder->device_id = deviceId;
}

void AAudioStreamBuilder_setPackageName(AAudioStreamBuilder*, const char*) {}

void AAudioStreamBuilder_setAttributionTag(AAudioStreamBuilder*, const char*) {}

void AAudioStreamBuilder_setSampleRate(AAudioStreamBuilder* builder, int32_t sampleRate) {
    builder->sample_rate = sampleRate;
}

// As Android's: the channel count and the channel mask replace each other, the last one set counts.
void AAudioStreamBuilder_setChannelCount(AAudioStreamBuilder* builder, int32_t channelCount) {
    builder->channel_count = channelCount;
    builder->channel_mask = AAUDIO_UNSPECIFIED;
}

void AAudioStreamBuilder_setSamplesPerFrame(AAudioStreamBuilder* builder, int32_t samplesPerFrame) {
    AAudioStreamBuilder_setChannelCount(builder, samplesPerFrame);
}

void AAudioStreamBuilder_setChannelMask(AAudioStreamBuilder* builder, aaudio_channel_mask_t channelMask) {
    builder->channel_mask = channelMask;
    builder->channel_count = AAUDIO_UNSPECIFIED;
}

void AAudioStreamBuilder_setFormat(AAudioStreamBuilder* builder, aaudio_format_t format) {
    builder->format = format;
}

void AAudioStreamBuilder_setSharingMode(AAudioStreamBuilder* builder, aaudio_sharing_mode_t sharingMode) {
    builder->sharing_mode = sharingMode;
}

void AAudioStreamBuilder_setDirection(AAudioStreamBuilder* builder, aaudio_direction_t direction) {
    builder->direction = direction;
}

// The capacity is the runtime's, APKRT_AUDIO_BURSTS bursts.
void AAudioStreamBuilder_setBufferCapacityInFrames(AAudioStreamBuilder* builder, int32_t numFrames) {
    builder->buffer_capacity = numFrames;  // an input stream's ring holds at least that many frames
}

void AAudioStreamBuilder_setPerformanceMode(AAudioStreamBuilder* builder, aaudio_performance_mode_t mode) {
    builder->performance_mode = mode;
}

void AAudioStreamBuilder_setUsage(AAudioStreamBuilder* builder, aaudio_usage_t usage) {
    builder->usage = usage;
}

void AAudioStreamBuilder_setContentType(AAudioStreamBuilder* builder, aaudio_content_type_t contentType) {
    builder->content_type = contentType;
}

void AAudioStreamBuilder_setSpatializationBehavior(AAudioStreamBuilder* builder,
                                                   aaudio_spatialization_behavior_t spatializationBehavior) {
    builder->spatialization_behavior = spatializationBehavior;
}

void AAudioStreamBuilder_setIsContentSpatialized(AAudioStreamBuilder* builder, bool isSpatialized) {
    builder->content_spatialized = isSpatialized;
}

void AAudioStreamBuilder_setInputPreset(AAudioStreamBuilder* builder, aaudio_input_preset_t inputPreset) {
    builder->input_preset = inputPreset;
}

void AAudioStreamBuilder_setAllowedCapturePolicy(AAudioStreamBuilder* builder,
                                                 aaudio_allowed_capture_policy_t capturePolicy) {
    builder->capture_policy = capturePolicy;
}

void AAudioStreamBuilder_setSessionId(AAudioStreamBuilder* builder, aaudio_session_id_t sessionId) {
    builder->session_id = sessionId;
}

void AAudioStreamBuilder_setPrivacySensitive(AAudioStreamBuilder* builder, bool privacySensitive) {
    builder->privacy_sensitive = privacySensitive;
}

void AAudioStreamBuilder_setDataCallback(AAudioStreamBuilder* builder, AAudioStream_dataCallback callback,
                                         void* userData) {
    builder->data_callback = callback;
    builder->data_user = userData;
}

void AAudioStreamBuilder_setFramesPerDataCallback(AAudioStreamBuilder* builder, int32_t numFrames) {
    builder->frames_per_data_callback = numFrames;
}

void AAudioStreamBuilder_setErrorCallback(AAudioStreamBuilder* builder, AAudioStream_errorCallback callback,
                                          void* userData) {
    builder->error_callback = callback;
    builder->error_user = userData;
}

aaudio_result_t AAudioStreamBuilder_openStream(AAudioStreamBuilder* builder, AAudioStream** streamPtr) {
    if (builder == nullptr || streamPtr == nullptr) return AAUDIO_ERROR_NULL;
    *streamPtr = nullptr;
    // AudioStreamBuilder's checks, in its order.
    if (builder->direction != AAUDIO_DIRECTION_OUTPUT && builder->direction != AAUDIO_DIRECTION_INPUT) {
        return AAUDIO_ERROR_ILLEGAL_ARGUMENT;
    }
    if (builder->sharing_mode != AAUDIO_SHARING_MODE_SHARED && builder->sharing_mode != AAUDIO_SHARING_MODE_EXCLUSIVE) {
        return AAUDIO_ERROR_ILLEGAL_ARGUMENT;
    }
    switch (builder->format) {
        case AAUDIO_FORMAT_UNSPECIFIED:
        case AAUDIO_FORMAT_PCM_I16:
        case AAUDIO_FORMAT_PCM_FLOAT:
        case AAUDIO_FORMAT_PCM_I24_PACKED:
        case AAUDIO_FORMAT_PCM_I32:
            break;
        default:
            return AAUDIO_ERROR_INVALID_FORMAT;
    }
    if (builder->sample_rate != AAUDIO_UNSPECIFIED && (builder->sample_rate < 8'000 || builder->sample_rate > 1'600'000)) {
        return AAUDIO_ERROR_INVALID_RATE;
    }
    const int32_t channels = builder->channel_mask != AAUDIO_UNSPECIFIED ? __builtin_popcount(builder->channel_mask)
                             : builder->channel_count != AAUDIO_UNSPECIFIED ? builder->channel_count
                                                                            : 2;
    if (channels < 1 || channels > kMostChannels) return AAUDIO_ERROR_OUT_OF_RANGE;
    if (builder->performance_mode != AAUDIO_PERFORMANCE_MODE_NONE &&
        builder->performance_mode != AAUDIO_PERFORMANCE_MODE_POWER_SAVING &&
        builder->performance_mode != AAUDIO_PERFORMANCE_MODE_LOW_LATENCY) {
        return AAUDIO_ERROR_ILLEGAL_ARGUMENT;
    }
    if (builder->frames_per_data_callback < 0 || builder->frames_per_data_callback > 1024 * 1024) {
        return AAUDIO_ERROR_OUT_OF_RANGE;
    }
    if (channels > APKRT_AUDIO_CHANNELS) {
        AAUDIO_LOG(WARN, "openStream: %d channels, Minato plays at most %d", channels, APKRT_AUDIO_CHANNELS);
        return AAUDIO_ERROR_OUT_OF_RANGE;
    }

    const bool input = builder->direction == AAUDIO_DIRECTION_INPUT;
    apkrt_audio_config config = {builder->sample_rate, channels, builder->performance_mode, 0, 0,
                                 input ? builder->buffer_capacity : 0, 0, input ? 1 : 0};
    const long handle = call(APKRT_NR_AUDIO_OPEN, bits(&config));
    if (handle < 0) return result_of(errno);
    auto* stream = static_cast<AAudioStream*>(calloc(1, sizeof(AAudioStream)));
    const aaudio_format_t format =
        builder->format == AAUDIO_FORMAT_UNSPECIFIED ? AAUDIO_FORMAT_PCM_FLOAT : builder->format;
    const int32_t per_call = builder->frames_per_data_callback > 0 ? builder->frames_per_data_callback
                                                                   : config.frames_per_burst;
    const int32_t chunk = config.capacity > per_call ? config.capacity : per_call;
    if (stream != nullptr) {
        pthread_mutex_init(&stream->gate, nullptr);
        pthread_mutex_init(&stream->feed, nullptr);
        stream->floats = static_cast<float*>(malloc(static_cast<size_t>(chunk) * static_cast<size_t>(channels) *
                                                    sizeof(float)));
        stream->callback_frames = builder->data_callback == nullptr
                                      ? nullptr
                                      : malloc(static_cast<size_t>(per_call) * static_cast<size_t>(channels) *
                                               static_cast<size_t>(sample_bytes(format)));
    }
    if (stream == nullptr || stream->floats == nullptr ||
        (builder->data_callback != nullptr && stream->callback_frames == nullptr)) {
        call(APKRT_NR_AUDIO_CLOSE, bits(handle));
        if (stream != nullptr) free_stream(stream);
        return AAUDIO_ERROR_NO_MEMORY;
    }
    stream->handle = static_cast<uint64_t>(handle);
    stream->input = input;
    stream->sample_rate = config.sample_rate;
    stream->channels = channels;
    stream->burst = config.frames_per_burst;
    stream->capacity = config.capacity;
    stream->hardware_rate = config.hardware_rate;
    stream->format = format;
    stream->channel_mask = builder->channel_mask;
    stream->performance_mode = builder->performance_mode;
    stream->usage = builder->usage;
    stream->content_type = builder->content_type;
    stream->input_preset = builder->input_preset;
    stream->capture_policy = builder->capture_policy;
    stream->session_id = builder->session_id == AAUDIO_SESSION_ID_ALLOCATE
                             ? __atomic_add_fetch(&last_session_id, 1, __ATOMIC_RELAXED)
                             : builder->session_id;
    stream->spatialization_behavior = builder->spatialization_behavior;
    stream->content_spatialized = builder->content_spatialized;
    stream->privacy_sensitive = builder->privacy_sensitive;
    stream->frames_per_data_callback = builder->frames_per_data_callback;
    stream->data_callback = builder->data_callback;
    stream->data_user = builder->data_user;
    stream->error_callback = builder->error_callback;
    stream->error_user = builder->error_user;
    stream->per_call = per_call;
    stream->chunk = chunk;
    if (stream->data_callback == nullptr && stream->error_callback == nullptr) {
        *streamPtr = stream;
        return AAUDIO_OK;
    }
    // The thread starts once the stream is whole and the app has it: until then the gate stays closed.
    pthread_mutex_lock(&stream->gate);
    if (pthread_create(&stream->thread, nullptr, run_callbacks, stream) != 0) {
        pthread_mutex_unlock(&stream->gate);
        call(APKRT_NR_AUDIO_CLOSE, bits(handle));
        free_stream(stream);
        return AAUDIO_ERROR_INTERNAL;
    }
    stream->has_thread = true;
    *streamPtr = stream;
    pthread_mutex_unlock(&stream->gate);
    return AAUDIO_OK;
}

// MARK: - The stream

aaudio_result_t AAudioStream_release(AAudioStream* stream) {
    if (stream == nullptr) return AAUDIO_ERROR_NULL;
    if (on_own_thread(stream)) {
        AAUDIO_LOG(ERROR, "release() cannot be called from a callback!");
        return AAUDIO_ERROR_INVALID_STATE;
    }
    release(stream);
    return AAUDIO_OK;
}

aaudio_result_t AAudioStream_close(AAudioStream* stream) {
    if (stream == nullptr) return AAUDIO_ERROR_NULL;
    if (on_own_thread(stream)) {
        AAUDIO_LOG(ERROR, "close() cannot be called from a callback!");
        return AAUDIO_ERROR_INVALID_STATE;
    }
    release(stream);
    free_stream(stream);
    return AAUDIO_OK;
}

aaudio_result_t AAudioStream_requestStart(AAudioStream* stream) {
    return command(stream, APKRT_AUDIO_START);
}

aaudio_result_t AAudioStream_requestPause(AAudioStream* stream) {
    return command(stream, APKRT_AUDIO_PAUSE);
}

aaudio_result_t AAudioStream_requestFlush(AAudioStream* stream) {
    return command(stream, APKRT_AUDIO_FLUSH);
}

aaudio_result_t AAudioStream_requestStop(AAudioStream* stream) {
    return command(stream, APKRT_AUDIO_STOP);
}

aaudio_stream_state_t AAudioStream_getState(AAudioStream* stream) {
    apkrt_audio_status status;
    if (status_of(stream, &status)) return status.state;
    return released(stream) ? AAUDIO_STREAM_STATE_CLOSING : AAUDIO_STREAM_STATE_UNKNOWN;
}

aaudio_result_t AAudioStream_waitForStateChange(AAudioStream* stream, aaudio_stream_state_t inputState,
                                                aaudio_stream_state_t* nextState, int64_t timeoutNanoseconds) {
    const int64_t deadline = timeoutNanoseconds > 0 ? deadline_after(timeoutNanoseconds) : 0;
    while (true) {
        apkrt_audio_status status;
        const bool open = status_of(stream, &status);
        const aaudio_stream_state_t state =
            open ? status.state : released(stream) ? AAUDIO_STREAM_STATE_CLOSING : AAUDIO_STREAM_STATE_UNKNOWN;
        if (state != inputState || !open || passed(deadline)) {
            if (nextState != nullptr) *nextState = state;
            return state != inputState ? AAUDIO_OK : AAUDIO_ERROR_TIMEOUT;
        }
        call(APKRT_NR_AUDIO_WAIT, stream->handle, status.sequence, bits(deadline));  // whatever it answers: look again
    }
}

aaudio_result_t AAudioStream_read(AAudioStream* stream, void* buffer, int32_t numFrames, int64_t timeoutNanoseconds) {
    if (buffer == nullptr) return AAUDIO_ERROR_NULL;
    if (stream->data_callback != nullptr) return AAUDIO_ERROR_INVALID_STATE;
    if (!stream->input) return AAUDIO_ERROR_UNIMPLEMENTED;  // an output stream, as AudioStream's own read answers
    if (numFrames < 0) return AAUDIO_ERROR_ILLEGAL_ARGUMENT;
    if (numFrames == 0) return 0;
    if (released(stream)) return AAUDIO_ERROR_INVALID_STATE;
    const int64_t deadline = timeoutNanoseconds > 0 ? deadline_after(timeoutNanoseconds) : 0;
    const int64_t got = take_in(stream, buffer, numFrames, deadline);
    return got < 0 ? result_of(static_cast<int>(-got)) : static_cast<aaudio_result_t>(got);
}

aaudio_result_t AAudioStream_write(AAudioStream* stream, const void* buffer, int32_t numFrames,
                                   int64_t timeoutNanoseconds) {
    if (buffer == nullptr) return AAUDIO_ERROR_NULL;
    if (stream->input) return AAUDIO_ERROR_UNIMPLEMENTED;
    // AudioTrack refuses writes to a stream in callback mode, before its start too (Android 15's answer).
    if (stream->data_callback != nullptr) return AAUDIO_ERROR_INVALID_STATE;
    if (numFrames < 0) return AAUDIO_ERROR_ILLEGAL_ARGUMENT;
    if (numFrames == 0) return 0;
    if (released(stream)) return AAUDIO_ERROR_INVALID_STATE;
    const int64_t deadline = timeoutNanoseconds > 0 ? deadline_after(timeoutNanoseconds) : 0;
    // A legacy (not LOW_LATENCY) stream blocks on any timeout above 0, as Android's AudioStreamTrack does.
    const bool block = timeoutNanoseconds > 0 && stream->performance_mode != AAUDIO_PERFORMANCE_MODE_LOW_LATENCY;
    if (stream->format == AAUDIO_FORMAT_PCM_FLOAT) {
        const int64_t taken = hand_over(stream, static_cast<const float*>(buffer), numFrames, deadline, block);
        return taken < 0 ? result_of(static_cast<int>(-taken)) : static_cast<aaudio_result_t>(taken);
    }
    const auto* source = static_cast<const uint8_t*>(buffer);
    const int64_t frame_bytes = int64_t{sample_bytes(stream->format)} * stream->channels;
    int64_t written = 0;
    while (written < numFrames) {
        const int64_t count = smaller(numFrames - written, stream->chunk);
        to_float(stream->format, source + written * frame_bytes, stream->floats, count * stream->channels);
        const int64_t taken = hand_over(stream, stream->floats, count, deadline, block);
        if (taken < 0) return written > 0 ? static_cast<aaudio_result_t>(written) : result_of(static_cast<int>(-taken));
        written += taken;
        if (taken < count) break;  // the deadline passed
    }
    return static_cast<aaudio_result_t>(written);
}

aaudio_result_t AAudioStream_setBufferSizeInFrames(AAudioStream* stream, int32_t numFrames) {
    if (released(stream)) return AAUDIO_ERROR_INVALID_STATE;
    const long size = call(APKRT_NR_AUDIO_CONTROL, stream->handle, APKRT_AUDIO_BUFFER_SIZE, bits(numFrames));
    return size < 0 ? result_of(errno) : static_cast<aaudio_result_t>(size);
}

int32_t AAudioStream_getBufferSizeInFrames(AAudioStream* stream) {
    apkrt_audio_status status;
    status_of(stream, &status);
    return status.buffer_size;
}

int32_t AAudioStream_getFramesPerBurst(AAudioStream* stream) {
    return stream->burst;
}

int32_t AAudioStream_getBufferCapacityInFrames(AAudioStream* stream) {
    return stream->capacity;
}

int32_t AAudioStream_getFramesPerDataCallback(AAudioStream* stream) {
    return stream->frames_per_data_callback;
}

int32_t AAudioStream_getXRunCount(AAudioStream* stream) {
    apkrt_audio_status status;
    status_of(stream, &status);
    return status.xruns;
}

int32_t AAudioStream_getSampleRate(AAudioStream* stream) {
    return stream->sample_rate;
}

int32_t AAudioStream_getHardwareSampleRate(AAudioStream* stream) {
    return stream->hardware_rate;
}

int32_t AAudioStream_getChannelCount(AAudioStream* stream) {
    return stream->channels;
}

int32_t AAudioStream_getSamplesPerFrame(AAudioStream* stream) {
    return stream->channels;
}

int32_t AAudioStream_getHardwareChannelCount(AAudioStream* stream) {
    return stream->channels;  // the host's mixer takes the stream's channels as they are
}

aaudio_channel_mask_t AAudioStream_getChannelMask(AAudioStream* stream) {
    return stream->channel_mask;
}

int32_t AAudioStream_getDeviceId(AAudioStream*) {
    return AAUDIO_UNSPECIFIED;  // iOS routes by itself
}

aaudio_format_t AAudioStream_getFormat(AAudioStream* stream) {
    return stream->format;
}

aaudio_format_t AAudioStream_getHardwareFormat(AAudioStream*) {
    return AAUDIO_FORMAT_PCM_FLOAT;
}

aaudio_sharing_mode_t AAudioStream_getSharingMode(AAudioStream*) {
    return AAUDIO_SHARING_MODE_SHARED;  // EXCLUSIVE needs MMAP (AudioStreamBuilder.cpp)
}

aaudio_performance_mode_t AAudioStream_getPerformanceMode(AAudioStream* stream) {
    return stream->performance_mode;
}

aaudio_direction_t AAudioStream_getDirection(AAudioStream* stream) {
    return stream->input ? AAUDIO_DIRECTION_INPUT : AAUDIO_DIRECTION_OUTPUT;
}

int64_t AAudioStream_getFramesWritten(AAudioStream* stream) {
    apkrt_audio_status status;
    status_of(stream, &status);
    return status.frames_written;
}

int64_t AAudioStream_getFramesRead(AAudioStream* stream) {
    apkrt_audio_status status;
    status_of(stream, &status);
    return status.frames_read;
}

aaudio_session_id_t AAudioStream_getSessionId(AAudioStream* stream) {
    return stream->session_id;
}

aaudio_result_t AAudioStream_getTimestamp(AAudioStream* stream, clockid_t clockid, int64_t* framePosition,
                                          int64_t* timeNanoseconds) {
    if (framePosition == nullptr || timeNanoseconds == nullptr) return AAUDIO_ERROR_NULL;
    if (clockid != CLOCK_MONOTONIC && clockid != CLOCK_BOOTTIME) return AAUDIO_ERROR_ILLEGAL_ARGUMENT;
    apkrt_audio_status status;
    // As AudioStreamTrack's: only while the stream plays, and once a frame has been heard.
    if (!status_of(stream, &status) || status.state != AAUDIO_STREAM_STATE_STARTED || status.position < 0) {
        return AAUDIO_ERROR_INVALID_STATE;
    }
    int64_t time = status.time;
    if (clockid == CLOCK_BOOTTIME) time += now(CLOCK_BOOTTIME) - now(CLOCK_MONOTONIC);
    *framePosition = status.position;
    *timeNanoseconds = time;
    return AAUDIO_OK;
}

aaudio_usage_t AAudioStream_getUsage(AAudioStream* stream) {
    return stream->usage;
}

aaudio_content_type_t AAudioStream_getContentType(AAudioStream* stream) {
    return stream->content_type;
}

aaudio_spatialization_behavior_t AAudioStream_getSpatializationBehavior(AAudioStream* stream) {
    return stream->spatialization_behavior;
}

bool AAudioStream_isContentSpatialized(AAudioStream* stream) {
    return stream->content_spatialized;
}

aaudio_input_preset_t AAudioStream_getInputPreset(AAudioStream* stream) {
    return stream->input_preset;
}

aaudio_allowed_capture_policy_t AAudioStream_getAllowedCapturePolicy(AAudioStream* stream) {
    return stream->capture_policy;
}

bool AAudioStream_isMMapUsed(AAudioStream*) {
    return false;
}

bool AAudioStream_isPrivacySensitive(AAudioStream* stream) {
    return stream->privacy_sensitive;
}

// MARK: - Private

void minato_AAudioStream_setCallbackFrames(AAudioStream* stream, int32_t frames) {
    if (stream == nullptr || !on_own_thread(stream)) return;
    stream->given = frames < 0 ? 0 : frames;
}

void minato_AAudioStream_claimCallbackFrames(AAudioStream* stream) {
    if (stream == nullptr || !on_own_thread(stream)) return;
    pthread_mutex_lock(&stream->feed);
    stream->claimed_at = stream->drops;
    stream->claimed = true;
    pthread_mutex_unlock(&stream->feed);
}

}  // extern "C"
