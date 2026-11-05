// Ours (ADR-0064): the natives of our AudioRecord (framework/managers/android/media/AudioRecord.java), which on Android
// stand on libaudioclient's AudioRecord and the audio server. A recorder reads an input stream of libaaudio in float32
// (runtime/android/aaudio.cpp, which reads the host's microphone) and converts each read to the app's encoding. A
// blocking read waits until it has all its frames, a stop or the release, in slices so that either is seen at once; a
// read that is not blocking, or one before the start, gives what the stream holds. Opening the stream needs
// RECORD_AUDIO's grant: without it nativeCreate answers 0 and the recorder stays uninitialized, as Android's does.
#include <aaudio/AAudio.h>
#include <jni.h>
#include <nativehelper/JNIHelp.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <vector>

#include "core_jni_helpers.h"

namespace android {
namespace {

constexpr int kPcm16 = 2, kPcm8 = 3, kPcmFloat = 4;
// AudioRecord.java's codes.
constexpr jint kSuccess = 0, kBadValue = -2, kInvalidOperation = -3, kDeadObject = -6;
constexpr int64_t kSlice = 20'000'000;  // ns: how long a blocking read waits before it looks at the flags again

int bytes_per_sample(int encoding) {
    switch (encoding) {
        case kPcm8: return 1;
        case kPcm16: return 2;
        case kPcmFloat: return 4;
        default: return 0;
    }
}

struct Recorder {
    AAudioStream *stream = nullptr;
    int channels = 0;
    int encoding = 0;
    int frames = 0;  // the stream's ring, in frames
    std::atomic<bool> recording{false};
    std::atomic<bool> released{false};
    std::atomic<int> readers{0};
};

Recorder *recorder_of(jlong handle) {
    return reinterpret_cast<Recorder *>(static_cast<intptr_t>(handle));
}

jlong create(JNIEnv *, jclass, jint sample_rate, jint channels, jint encoding, jint frames) {
    if (bytes_per_sample(encoding) == 0 || channels < 1 || frames < 1) return 0;
    auto *recorder = new Recorder;
    recorder->channels = channels;
    recorder->encoding = encoding;
    AAudioStreamBuilder *builder = nullptr;
    if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK) {
        delete recorder;
        return 0;
    }
    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_INPUT);
    AAudioStreamBuilder_setSampleRate(builder, sample_rate);
    AAudioStreamBuilder_setChannelCount(builder, channels);
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_FLOAT);
    AAudioStreamBuilder_setBufferCapacityInFrames(builder, frames);
    const aaudio_result_t opened = AAudioStreamBuilder_openStream(builder, &recorder->stream);
    AAudioStreamBuilder_delete(builder);
    if (opened != AAUDIO_OK) {
        delete recorder;
        return 0;
    }
    recorder->frames = std::max(frames, AAudioStream_getBufferCapacityInFrames(recorder->stream));
    return static_cast<jlong>(reinterpret_cast<intptr_t>(recorder));
}

void release(JNIEnv *, jclass, jlong handle) {
    Recorder *recorder = recorder_of(handle);
    recorder->released = true;
    recorder->recording = false;
    AAudioStream_requestStop(recorder->stream);
    while (recorder->readers.load() != 0) usleep(1000);  // a blocked read sees the flags within a slice
    AAudioStream_close(recorder->stream);
    delete recorder;
}

jint start(JNIEnv *, jclass, jlong handle) {
    Recorder *recorder = recorder_of(handle);
    if (AAudioStream_requestStart(recorder->stream) != AAUDIO_OK) return kInvalidOperation;
    recorder->recording = true;
    return kSuccess;
}

void stop(JNIEnv *, jclass, jlong handle) {
    Recorder *recorder = recorder_of(handle);
    recorder->recording = false;
    AAudioStream_requestStop(recorder->stream);
}

jint buffer_frames(JNIEnv *, jclass, jlong handle) {
    return recorder_of(handle)->frames;
}

/// The frame bytes of the app's encoding.
size_t frame_bytes(const Recorder &recorder) {
    return static_cast<size_t>(recorder.channels) * static_cast<size_t>(bytes_per_sample(recorder.encoding));
}

void to_encoding(int encoding, const float *from, uint8_t *to, size_t samples) {
    for (size_t index = 0; index < samples; ++index) {
        const float value = std::min(1.0f, std::max(-1.0f, from[index]));
        switch (encoding) {
            case kPcm8:
                *to++ = static_cast<uint8_t>(value * 127.0f + 128.5f);
                break;
            case kPcm16: {
                const auto sample =
                    static_cast<int16_t>(value >= 1.0f ? 32767 : static_cast<int32_t>(value * 32768.0f));
                memcpy(to, &sample, sizeof sample);
                to += sizeof sample;
                break;
            }
            default:
                memcpy(to, &value, sizeof value);
                to += sizeof value;
                break;
        }
    }
}

// Reads up to `bytes` bytes (whole frames) into `out`: the bytes read, or a negative AudioRecord code.
jint read_bytes(Recorder *recorder, uint8_t *out, size_t bytes, bool blocking) {
    if (recorder->released.load()) return kInvalidOperation;
    const size_t width = frame_bytes(*recorder);
    const size_t wanted = bytes / width;
    recorder->readers.fetch_add(1);
    std::vector<float> chunk(static_cast<size_t>(4096) * static_cast<size_t>(recorder->channels));
    size_t done = 0;
    jint error = kSuccess;
    while (done < wanted && !recorder->released.load()) {
        const size_t part = std::min<size_t>(wanted - done, 4096);
        const int64_t wait = blocking && recorder->recording.load() ? kSlice : 0;
        const aaudio_result_t got =
            AAudioStream_read(recorder->stream, chunk.data(), static_cast<int32_t>(part), wait);
        if (got < 0) {
            error = got == AAUDIO_ERROR_DISCONNECTED ? kDeadObject : kInvalidOperation;
            break;
        }
        to_encoding(recorder->encoding, chunk.data(), out + done * width,
                    static_cast<size_t>(got) * recorder->channels);
        done += static_cast<size_t>(got);
        if (static_cast<size_t>(got) < part) {
            // The slice ended with fewer frames: go on while blocking and recording, else give what there is.
            if (!blocking || !recorder->recording.load()) break;
        }
    }
    recorder->readers.fetch_sub(1);
    if (done == 0 && error != kSuccess) return error;
    return static_cast<jint>(done * width);
}

jint read_bytes_array(JNIEnv *env, jclass, jlong handle, jbyteArray data, jint offset, jint size, jboolean blocking) {
    jbyte *elements = env->GetByteArrayElements(data, nullptr);
    if (elements == nullptr) return kBadValue;
    const jint got = read_bytes(recorder_of(handle), reinterpret_cast<uint8_t *>(elements + offset),
                                static_cast<size_t>(size), blocking);
    env->ReleaseByteArrayElements(data, elements, got > 0 ? 0 : JNI_ABORT);
    return got;
}

jint read_shorts(JNIEnv *env, jclass, jlong handle, jshortArray data, jint offset, jint size, jboolean blocking) {
    Recorder *recorder = recorder_of(handle);
    if (recorder->encoding != kPcm16) return kInvalidOperation;
    jshort *elements = env->GetShortArrayElements(data, nullptr);
    if (elements == nullptr) return kBadValue;
    const jint got = read_bytes(recorder, reinterpret_cast<uint8_t *>(elements + offset),
                                static_cast<size_t>(size) * sizeof(jshort), blocking);
    env->ReleaseShortArrayElements(data, elements, got > 0 ? 0 : JNI_ABORT);
    return got > 0 ? static_cast<jint>(got / sizeof(jshort)) : got;
}

jint read_floats(JNIEnv *env, jclass, jlong handle, jfloatArray data, jint offset, jint size, jboolean blocking) {
    Recorder *recorder = recorder_of(handle);
    jfloat *elements = env->GetFloatArrayElements(data, nullptr);
    if (elements == nullptr) return kBadValue;
    const jint got = read_bytes(recorder, reinterpret_cast<uint8_t *>(elements + offset),
                                static_cast<size_t>(size) * sizeof(jfloat), blocking);
    env->ReleaseFloatArrayElements(data, elements, got > 0 ? 0 : JNI_ABORT);
    return got > 0 ? static_cast<jint>(got / sizeof(jfloat)) : got;
}

jint read_direct(JNIEnv *env, jclass, jlong handle, jobject data, jint size, jboolean blocking) {
    auto *bytes = static_cast<uint8_t *>(env->GetDirectBufferAddress(data));
    if (bytes == nullptr) return kBadValue;
    return read_bytes(recorder_of(handle), bytes, static_cast<size_t>(size), blocking);
}

const JNINativeMethod kMethods[] = {
    {"nativeCreate", "(IIII)J", reinterpret_cast<void *>(create)},
    {"nativeRelease", "(J)V", reinterpret_cast<void *>(release)},
    {"nativeStart", "(J)I", reinterpret_cast<void *>(start)},
    {"nativeStop", "(J)V", reinterpret_cast<void *>(stop)},
    {"nativeBufferFrames", "(J)I", reinterpret_cast<void *>(buffer_frames)},
    {"nativeReadBytes", "(J[BIIZ)I", reinterpret_cast<void *>(read_bytes_array)},
    {"nativeReadShorts", "(J[SIIZ)I", reinterpret_cast<void *>(read_shorts)},
    {"nativeReadFloats", "(J[FIIZ)I", reinterpret_cast<void *>(read_floats)},
    {"nativeReadDirect", "(JLjava/nio/ByteBuffer;IZ)I", reinterpret_cast<void *>(read_direct)},
};

}  // namespace

int register_android_media_AudioRecord(JNIEnv *env) {
    return RegisterMethodsOrDie(env, "android/media/AudioRecord", kMethods, NELEM(kMethods));
}

}  // namespace android
