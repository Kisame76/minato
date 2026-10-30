// Ours (the compatibility plan's gap 13, the Unity test APK probe: FMOD's output thread): the natives of our AudioTrack
// (framework/managers/android/media/AudioTrack.java), which on Android stand on libaudioclient's AudioTrack and the
// audio server. A track keeps the frames it was given in a buffer of its own, as the server's shared memory holds
// them, and plays them on a float stream of libaaudio in callback mode, converting each encoding as the mixer does.
// The states and their order are those of libaudioclient's AudioTrack at android-15.0.0_r1 [from memory, checked by
// audiotrack-test against Android 15]: a blocking write waits for room, also before play; a pause or a stop wakes a
// write that waits, which returns what it wrote; a stop plays out what is left, and the position reads 0 while stopped
// or flushed; a flush drops the frames of a paused or stopped track; a start after a stop or a flush counts from 0. A
// static track plays its buffer once from the start, and its write copies to the buffer's start. A release wakes a
// waiting write and waits for it to leave. Not here: loops and markers, rate changes, timestamps, routing.
#include <aaudio/AAudio.h>
#include <dlfcn.h>
#include <jni.h>
#include <nativehelper/JNIHelp.h>
#include <stdint.h>
#include <string.h>

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <vector>

#include "core_jni_helpers.h"

namespace android {
namespace {

// AudioFormat's encodings that a track can play, with their bytes per sample.
constexpr int kPcm16 = 2, kPcm8 = 3, kPcmFloat = 4, kPcm24Packed = 21, kPcm32 = 22;
// AudioTrack.java's codes.
constexpr jint kSuccess = 0, kInvalidOperation = -3;

int bytes_per_sample(int encoding) {
    switch (encoding) {
        case kPcm8: return 1;
        case kPcm16: return 2;
        case kPcm24Packed: return 3;
        case kPcmFloat:
        case kPcm32: return 4;
        default: return 0;
    }
}

enum class State { stopped, active, paused, flushed };

struct Track {
    std::mutex lock;
    std::condition_variable changed;  // room, a wake-up or a writer's leave
    AAudioStream *stream = nullptr;
    int channels = 0;
    int encoding = 0;
    size_t frame_bytes = 0;
    size_t frames = 0;  // the buffer's capacity
    bool is_static = false;
    std::vector<uint8_t> buffer;
    uint64_t written = 0;  // streaming: frames written and read, so written - read are waiting
    uint64_t read = 0;     // static: the next frame to play
    State state = State::stopped;
    bool draining = false;  // stopped while frames were left, which still play
    bool running = false;   // the stream was started and not paused since
    uint64_t wakes = 0;     // pauses, stops and the release, which end a write's wait
    bool released = false;
    int writers = 0;
    uint32_t position = 0;  // frames played since the start, as AudioTrack::getPosition wraps them
    int underruns = 0;
    bool starved = false;
    float left = 1.0f, right = 1.0f;
};

float sample(const uint8_t *at, int encoding) {
    switch (encoding) {
        case kPcm8: return (static_cast<int>(*at) - 128) / 128.0f;
        case kPcm16: {
            int16_t value;
            memcpy(&value, at, sizeof value);
            return value / 32768.0f;
        }
        case kPcm24Packed: {
            const uint32_t bits = static_cast<uint32_t>(at[0]) << 8 | static_cast<uint32_t>(at[1]) << 16 |
                                  static_cast<uint32_t>(at[2]) << 24;
            const int32_t value = static_cast<int32_t>(bits);
            return value / 2147483648.0f;
        }
        case kPcm32: {
            int32_t value;
            memcpy(&value, at, sizeof value);
            return value / 2147483648.0f;
        }
        default: {
            float value;
            memcpy(&value, at, sizeof value);
            return value;
        }
    }
}

// Converts `count` frames of the buffer from frame `first` on (wrapping) into `out`, with the track's gains.
void convert(const Track &track, uint64_t first, size_t count, float *out) {
    const int width = bytes_per_sample(track.encoding);
    for (size_t frame = 0; frame < count; ++frame) {
        const uint8_t *at = track.buffer.data() + ((first + frame) % track.frames) * track.frame_bytes;
        for (int channel = 0; channel < track.channels; ++channel) {
            const float gain = channel == 1 ? track.right : track.left;
            *out++ = sample(at + channel * width, track.encoding) * gain;
        }
    }
}

/// libaaudio's (runtime/android/aaudio.cpp, guest_interface.h): the frames of this call that are audio. The NDK's
/// stub has no such name, so it comes from the runtime's libaaudio through dlsym, once.
void set_callback_frames(AAudioStream *stream, int32_t frames) {
    static const auto set = reinterpret_cast<void (*)(AAudioStream *, int32_t)>(
            dlsym(RTLD_DEFAULT, "minato_AAudioStream_setCallbackFrames"));
    if (set != nullptr) set(stream, frames);
}

aaudio_data_callback_result_t pull(AAudioStream *stream, void *user, void *audio, int32_t count) {
    Track &track = *static_cast<Track *>(user);
    float *out = static_cast<float *>(audio);
    const size_t asked = static_cast<size_t>(count);
    size_t given = 0;
    {
        std::lock_guard<std::mutex> hold(track.lock);
        if (track.state == State::active || track.draining) {
            const uint64_t available = track.is_static ? track.frames - track.read : track.written - track.read;
            given = static_cast<size_t>(std::min<uint64_t>(available, asked));
            convert(track, track.read, given, out);
            track.read += given;
            track.position += static_cast<uint32_t>(given);
            if (given < asked && !track.is_static && track.state == State::active) {
                if (!track.starved) ++track.underruns;
                track.starved = true;
            } else {
                track.starved = false;
            }
            if (track.draining && track.written == track.read) track.draining = false;
            if (given > 0) track.changed.notify_all();
        }
    }
    std::fill(out + given * track.channels, out + asked * track.channels, 0.0f);
    // Only the frames the track had are audio: a track that is slow to write leaves a gap that the host's read of a
    // short ring is, and not zeros that the ring would play later in the place of the frames about to come.
    set_callback_frames(stream, static_cast<int32_t>(given));
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

Track *track_of(jlong handle) {
    return reinterpret_cast<Track *>(static_cast<intptr_t>(handle));
}

jlong create(JNIEnv *, jclass, jint sample_rate, jint channels, jint encoding, jint frames, jboolean is_static,
             jboolean low_latency) {
    const int width = bytes_per_sample(encoding);
    if (width == 0 || channels < 1 || frames < 1) return 0;
    auto *track = new Track;
    track->channels = channels;
    track->encoding = encoding;
    track->frame_bytes = static_cast<size_t>(channels) * width;
    track->frames = static_cast<size_t>(frames);
    track->is_static = is_static;
    track->buffer.assign(track->frames * track->frame_bytes, 0);
    AAudioStreamBuilder *builder = nullptr;
    if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK) {
        delete track;
        return 0;
    }
    AAudioStreamBuilder_setSampleRate(builder, sample_rate);
    AAudioStreamBuilder_setChannelCount(builder, channels);
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_FLOAT);
    AAudioStreamBuilder_setPerformanceMode(builder, low_latency ? AAUDIO_PERFORMANCE_MODE_LOW_LATENCY
                                                                : AAUDIO_PERFORMANCE_MODE_NONE);
    AAudioStreamBuilder_setDataCallback(builder, pull, track);
    const aaudio_result_t opened = AAudioStreamBuilder_openStream(builder, &track->stream);
    AAudioStreamBuilder_delete(builder);
    if (opened != AAUDIO_OK) {
        delete track;
        return 0;
    }
    return static_cast<jlong>(reinterpret_cast<intptr_t>(track));
}

void release(JNIEnv *, jclass, jlong handle) {
    Track *track = track_of(handle);
    {
        std::unique_lock<std::mutex> hold(track->lock);
        track->released = true;
        ++track->wakes;
        track->changed.notify_all();
        track->changed.wait(hold, [track] { return track->writers == 0; });
    }
    AAudioStream_close(track->stream);
    delete track;
}

void start(JNIEnv *, jclass, jlong handle) {
    Track *track = track_of(handle);
    bool run;
    {
        std::lock_guard<std::mutex> hold(track->lock);
        if (track->state == State::active) return;
        if (track->state == State::stopped || track->state == State::flushed) {
            track->position = 0;
            if (track->is_static && track->state == State::flushed) track->read = 0;
        }
        track->state = State::active;
        track->starved = false;
        run = !track->running;
        track->running = true;
    }
    if (run) AAudioStream_requestStart(track->stream);
}

void stop(JNIEnv *, jclass, jlong handle) {
    Track *track = track_of(handle);
    std::lock_guard<std::mutex> hold(track->lock);
    if (track->state != State::active && track->state != State::paused) return;
    if (track->is_static) {
        track->read = 0;
    } else if (track->state == State::paused) {
        // A paused track's frames do not play.
        track->read = track->written;
    } else {
        track->draining = track->written != track->read;
    }
    track->state = State::stopped;
    ++track->wakes;
    track->changed.notify_all();
}

void pause(JNIEnv *, jclass, jlong handle) {
    Track *track = track_of(handle);
    bool was_running;
    {
        std::lock_guard<std::mutex> hold(track->lock);
        if (track->state != State::active) return;
        track->state = State::paused;
        ++track->wakes;
        track->changed.notify_all();
        was_running = track->running;
        track->running = false;
    }
    if (was_running) AAudioStream_requestPause(track->stream);
}

void flush(JNIEnv *, jclass, jlong handle) {
    Track *track = track_of(handle);
    std::lock_guard<std::mutex> hold(track->lock);
    if (track->is_static || track->state == State::active) return;
    track->read = track->written;
    track->draining = false;
    track->position = 0;
    track->state = State::flushed;
    track->changed.notify_all();
}

jint reload_static(JNIEnv *, jclass, jlong handle) {
    Track *track = track_of(handle);
    std::lock_guard<std::mutex> hold(track->lock);
    if (track->state == State::active) return kInvalidOperation;
    track->read = 0;
    track->position = 0;
    return kSuccess;
}

// Bytes written: whole frames into a streaming track's buffer, waiting for room when blocking until a wake-up; the
// bytes that fit from the start of a static track's buffer.
size_t write(Track *track, const uint8_t *data, size_t bytes, bool blocking) {
    std::unique_lock<std::mutex> hold(track->lock);
    if (track->is_static) {
        const size_t size = std::min(bytes, track->buffer.size());
        memcpy(track->buffer.data(), data, size);
        return size;
    }
    const size_t wanted = bytes / track->frame_bytes;
    const uint64_t wakes = track->wakes;
    size_t done = 0;
    ++track->writers;
    while (done < wanted && !track->released) {
        const size_t room = track->frames - static_cast<size_t>(track->written - track->read);
        if (room == 0) {
            if (!blocking || track->wakes != wakes) break;
            track->changed.wait(hold);
            continue;
        }
        size_t count = std::min(room, wanted - done);
        const size_t at = static_cast<size_t>(track->written % track->frames);
        count = std::min(count, track->frames - at);
        memcpy(track->buffer.data() + at * track->frame_bytes, data + done * track->frame_bytes,
               count * track->frame_bytes);
        track->written += count;
        done += count;
    }
    if (--track->writers == 0) track->changed.notify_all();
    return done * track->frame_bytes;
}

template <typename Array, typename Element>
jint write_array(JNIEnv *env, Array array, jint offset, jint size, jboolean blocking,
                 Element *(JNIEnv::*get)(Array, jboolean *), void (JNIEnv::*put)(Array, Element *, jint),
                 jlong handle) {
    Element *elements = (env->*get)(array, nullptr);
    if (elements == nullptr) return -2;
    const size_t written = write(track_of(handle), reinterpret_cast<const uint8_t *>(elements + offset),
                                 static_cast<size_t>(size) * sizeof(Element), blocking);
    (env->*put)(array, elements, JNI_ABORT);
    return static_cast<jint>(written / sizeof(Element));
}

jint write_bytes(JNIEnv *env, jclass, jlong handle, jbyteArray data, jint offset, jint size, jboolean blocking) {
    return write_array(env, data, offset, size, blocking, &JNIEnv::GetByteArrayElements,
                       &JNIEnv::ReleaseByteArrayElements, handle);
}

jint write_shorts(JNIEnv *env, jclass, jlong handle, jshortArray data, jint offset, jint size, jboolean blocking) {
    return write_array(env, data, offset, size, blocking, &JNIEnv::GetShortArrayElements,
                       &JNIEnv::ReleaseShortArrayElements, handle);
}

jint write_floats(JNIEnv *env, jclass, jlong handle, jfloatArray data, jint offset, jint size, jboolean blocking) {
    return write_array(env, data, offset, size, blocking, &JNIEnv::GetFloatArrayElements,
                       &JNIEnv::ReleaseFloatArrayElements, handle);
}

jint write_direct(JNIEnv *env, jclass, jlong handle, jobject data, jint offset, jint size, jboolean blocking) {
    const auto *bytes = static_cast<const uint8_t *>(env->GetDirectBufferAddress(data));
    if (bytes == nullptr) return -2;
    return static_cast<jint>(write(track_of(handle), bytes + offset, static_cast<size_t>(size), blocking));
}

jint position(JNIEnv *, jclass, jlong handle) {
    Track *track = track_of(handle);
    std::lock_guard<std::mutex> hold(track->lock);
    const bool zero = track->state == State::stopped || track->state == State::flushed;
    return zero ? 0 : static_cast<jint>(track->position);
}

jint underruns(JNIEnv *, jclass, jlong handle) {
    Track *track = track_of(handle);
    std::lock_guard<std::mutex> hold(track->lock);
    return track->underruns;
}

jint buffer_frames(JNIEnv *, jclass, jlong handle) {
    return static_cast<jint>(track_of(handle)->frames);
}

void set_volume(JNIEnv *, jclass, jlong handle, jfloat left, jfloat right) {
    Track *track = track_of(handle);
    std::lock_guard<std::mutex> hold(track->lock);
    track->left = left;
    track->right = right;
}

const JNINativeMethod kMethods[] = {
    {"nativeCreate", "(IIIIZZ)J", reinterpret_cast<void *>(create)},
    {"nativeRelease", "(J)V", reinterpret_cast<void *>(release)},
    {"nativeStart", "(J)V", reinterpret_cast<void *>(start)},
    {"nativeStop", "(J)V", reinterpret_cast<void *>(stop)},
    {"nativePause", "(J)V", reinterpret_cast<void *>(pause)},
    {"nativeFlush", "(J)V", reinterpret_cast<void *>(flush)},
    {"nativeReloadStatic", "(J)I", reinterpret_cast<void *>(reload_static)},
    {"nativeWriteBytes", "(J[BIIZ)I", reinterpret_cast<void *>(write_bytes)},
    {"nativeWriteShorts", "(J[SIIZ)I", reinterpret_cast<void *>(write_shorts)},
    {"nativeWriteFloats", "(J[FIIZ)I", reinterpret_cast<void *>(write_floats)},
    {"nativeWriteDirect", "(JLjava/nio/ByteBuffer;IIZ)I", reinterpret_cast<void *>(write_direct)},
    {"nativePosition", "(J)I", reinterpret_cast<void *>(position)},
    {"nativeUnderruns", "(J)I", reinterpret_cast<void *>(underruns)},
    {"nativeBufferFrames", "(J)I", reinterpret_cast<void *>(buffer_frames)},
    {"nativeSetVolume", "(JFF)V", reinterpret_cast<void *>(set_volume)},
};

}  // namespace

int register_android_media_AudioTrack(JNIEnv *env) {
    return RegisterMethodsOrDie(env, "android/media/AudioTrack", kMethods, NELEM(kMethods));
}

}  // namespace android
