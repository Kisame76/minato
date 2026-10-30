// Ours (the compatibility plan's gap 13, SoundPool): the natives of AOSP's android.media.SoundPool
// (frameworks/base/media/jni/soundpool at android-15.0.0_r1, which stands on libaudioclient's AudioTrack, binder
// memory and a thread pool, none of which Minato has). The rules are AOSP's, read from its SoundPool.cpp,
// SoundManager.cpp, StreamManager.cpp and Stream.cpp [from reading, checked by soundpool-test against Android 15]:
//  - sound IDs count up from 1 and stream IDs count up from 1; a sound is READY only after its decode, which runs
//    off the calling thread and then posts SAMPLE_LOADED (the sound's ID and a status, 0 for success) to the pool's
//    Java object;
//  - play answers 0 for a sound that is unknown, still loading or failed; a volume outside 0..1 becomes 1 for both
//    sides, a rate is clamped to 0.125..8 and a loop under -1 becomes -1;
//  - a free place is used first; else the active stream of the lowest priority that is not above the new one is
//    stolen, the oldest of equal priority first; else play answers 0;
//  - pause and resume change a stream that plays or is paused; autoPause pauses the playing ones and marks them,
//    autoResume resumes the marked ones and clears the marks; stop and the end of a sound free the place, and a
//    stream ID that no longer names a stream is ignored; unload takes the sound from the pool, and a stream that
//    plays it plays on.
// All live streams are mixed by one callback of one libaaudio stream (float, stereo, 48 kHz, low latency), the
// sounds' frames resampled by linear interpolation at the sound's rate times the stream's, so the pool costs the
// runtime's audio one stream however many sounds play. The decoders are ours: WAV files of PCM 8, 16, 24, 32 bit and
// float, and Ogg Vorbis through stb_vorbis, which the pool turns into 16-bit frames; other files fail with a status, as
// a file that no decoder takes fails on Android. The decoders for the rest are the plan's steps 3 and 4
// (docs/plans/2026-10-30-soundpool-and-audio-codecs.md).
#include <aaudio/AAudio.h>
#include <dlfcn.h>
#include <jni.h>
#include <nativehelper/JNIHelp.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "core_jni_helpers.h"

// stb_vorbis.c (third_party/sources.json), built without stdio and the push-data API.
extern "C" int stb_vorbis_decode_memory(const unsigned char *mem, int len, int *channels, int *sample_rate,
                                        short **output);

namespace android {
namespace {

constexpr int32_t kOutRate = 48000;
constexpr size_t kMaxFileBytes = 64u << 20;  // a sound file that we read whole
constexpr jint kSampleLoaded = 1;            // SoundPool.SAMPLE_LOADED
constexpr jint kUnknownError = -1;           // the status of a file that does not decode

struct Sound {
    int id = 0;
    enum State { loading, ready, failed } state = loading;
    std::vector<int16_t> frames;  // interleaved
    int rate = 0;
    int channels = 0;
    size_t count() const { return channels ? frames.size() / channels : 0; }
};

struct Stream {
    enum State { idle, playing, paused } state = idle;
    int id = 0;
    std::shared_ptr<Sound> sound;
    bool auto_paused = false;
    float left = 1, right = 1;
    int priority = 0;
    int loop = 0;  // the loops left, -1 for endless
    float rate = 1;
    double position = 0;  // in the sound's frames
    uint64_t started = 0;  // the order of the plays, for stealing the oldest
};

struct Pool {
    std::mutex lock;
    int max_streams = 1;
    int next_sound = 0;
    int next_stream = 0;
    uint64_t plays = 0;
    bool muted = false;
    std::vector<std::shared_ptr<Sound>> sounds;
    std::vector<Stream> streams;
    JavaVM *vm = nullptr;
    jobject object = nullptr;  // a global reference to the SoundPool
    jmethodID post_event = nullptr;
    AAudioStream *output = nullptr;
    bool running = false;  // the stream was started and its callback has not stopped it
    bool released = false;
    int pending_posts = 0;  // loads whose decode threads still hold the pool
};

std::shared_ptr<Sound> find_sound(Pool &pool, int id) {
    for (const auto &sound : pool.sounds) {
        if (sound->id == id) return sound;
    }
    return nullptr;
}

Stream *find_stream(Pool &pool, int id) {
    if (id <= 0) return nullptr;
    for (auto &stream : pool.streams) {
        if (stream.id == id) return &stream;
    }
    return nullptr;
}

// ---- the decoder ----

uint32_t le32(const uint8_t *at) {
    return at[0] | at[1] << 8 | at[2] << 16 | static_cast<uint32_t>(at[3]) << 24;
}
uint16_t le16(const uint8_t *at) { return static_cast<uint16_t>(at[0] | at[1] << 8); }

/// A RIFF/WAVE file of PCM or IEEE float into 16-bit frames; false for anything else.
bool decode_wav(const std::vector<uint8_t> &file, Sound &sound) {
    if (file.size() < 12 || memcmp(file.data(), "RIFF", 4) != 0 || memcmp(file.data() + 8, "WAVE", 4) != 0) {
        return false;
    }
    int format = 0, channels = 0, bits = 0;
    uint32_t rate = 0;
    const uint8_t *data = nullptr;
    size_t data_size = 0;
    size_t at = 12;
    while (at + 8 <= file.size()) {
        const uint32_t size = le32(file.data() + at + 4);
        const uint8_t *body = file.data() + at + 8;
        const size_t available = file.size() - (at + 8);
        if (memcmp(file.data() + at, "fmt ", 4) == 0 && size >= 16 && available >= 16) {
            format = le16(body);
            channels = le16(body + 2);
            rate = le32(body + 4);
            bits = le16(body + 14);
            if (format == 0xFFFE && size >= 26 && available >= 26) format = le16(body + 24);  // WAVE_FORMAT_EXTENSIBLE
        } else if (memcmp(file.data() + at, "data", 4) == 0) {
            data = body;
            data_size = std::min<size_t>(size, available);
            break;
        }
        at += 8 + static_cast<size_t>(size) + (size & 1);
    }
    if (data == nullptr || channels < 1 || channels > 2 || rate < 1 || rate > 192000) return false;
    const int width = bits / 8;
    const bool pcm = format == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32);
    if (!pcm && !(format == 3 && bits == 32)) return false;
    const size_t samples = data_size / width / channels * channels;
    sound.frames.resize(samples);
    for (size_t i = 0; i < samples; ++i) {
        const uint8_t *p = data + i * width;
        float value;
        if (format == 3) {
            float f;
            memcpy(&f, p, 4);
            value = f;
        } else if (bits == 8) {
            value = (static_cast<int>(p[0]) - 128) / 128.0f;
        } else if (bits == 16) {
            value = static_cast<int16_t>(le16(p)) / 32768.0f;
        } else if (bits == 24) {
            value = (static_cast<int32_t>(p[0] << 8 | p[1] << 16 | static_cast<uint32_t>(p[2]) << 24)) / 2147483648.0f;
        } else {
            value = static_cast<int32_t>(le32(p)) / 2147483648.0f;
        }
        sound.frames[i] = static_cast<int16_t>(std::lround(std::clamp(value, -1.0f, 1.0f) * 32767.0f));
    }
    sound.rate = static_cast<int>(rate);
    sound.channels = channels;
    return sound.count() > 0;
}

/// An Ogg Vorbis file (mono or stereo) into frames by stb_vorbis; false for anything else, Opus in Ogg included.
bool decode_ogg(const std::vector<uint8_t> &file, Sound &sound) {
    if (file.size() < 4 || memcmp(file.data(), "OggS", 4) != 0 || file.size() > INT32_MAX) return false;
    int channels = 0, rate = 0;
    short *output = nullptr;
    const int frames = stb_vorbis_decode_memory(file.data(), static_cast<int>(file.size()), &channels, &rate, &output);
    if (frames <= 0 || output == nullptr) {
        free(output);
        return false;
    }
    bool usable = channels >= 1 && channels <= 2 && rate >= 1 && rate <= 192000;
    if (usable) {
        sound.frames.assign(output, output + static_cast<size_t>(frames) * channels);
        sound.rate = rate;
        sound.channels = channels;
    }
    free(output);
    return usable;
}

// ---- the mixer ----

void set_callback_frames(AAudioStream *stream, int32_t frames) {
    static const auto set = reinterpret_cast<void (*)(AAudioStream *, int32_t)>(
            dlsym(RTLD_DEFAULT, "minato_AAudioStream_setCallbackFrames"));
    if (set != nullptr) set(stream, frames);
}

aaudio_data_callback_result_t mix(AAudioStream *stream, void *user, void *audio, int32_t count) {
    Pool &pool = *static_cast<Pool *>(user);
    float *out = static_cast<float *>(audio);
    std::fill(out, out + static_cast<size_t>(count) * 2, 0.0f);
    bool any = false;
    {
        std::lock_guard<std::mutex> hold(pool.lock);
        for (Stream &stream_state : pool.streams) {
            if (stream_state.state != Stream::playing) continue;
            any = true;
            const Sound &sound = *stream_state.sound;
            const size_t frames = sound.count();
            const double step = static_cast<double>(sound.rate) * stream_state.rate / kOutRate;
            const float left = pool.muted ? 0.0f : stream_state.left;
            const float right = pool.muted ? 0.0f : stream_state.right;
            for (int32_t i = 0; i < count && stream_state.state == Stream::playing; ++i) {
                size_t first = static_cast<size_t>(stream_state.position);
                if (first >= frames) {
                    if (stream_state.loop != 0) {
                        if (stream_state.loop > 0) --stream_state.loop;
                        stream_state.position -= static_cast<double>(frames);
                        first = static_cast<size_t>(stream_state.position);
                    } else {
                        stream_state.state = Stream::idle;
                        stream_state.sound.reset();
                        break;
                    }
                }
                const double fraction = stream_state.position - static_cast<double>(first);
                size_t next = first + 1;
                if (next >= frames) next = stream_state.loop != 0 ? 0 : first;
                const int channels = sound.channels;
                const int16_t *a = &sound.frames[first * channels];
                const int16_t *b = &sound.frames[next * channels];
                const float l0 = a[0] / 32768.0f, l1 = b[0] / 32768.0f;
                const float r0 = channels == 2 ? a[1] / 32768.0f : l0, r1 = channels == 2 ? b[1] / 32768.0f : l1;
                out[i * 2] += (l0 + static_cast<float>(fraction) * (l1 - l0)) * left;
                out[i * 2 + 1] += (r0 + static_cast<float>(fraction) * (r1 - r0)) * right;
                stream_state.position += step;
            }
        }
        if (!any) pool.running = false;
    }
    set_callback_frames(stream, any ? count : 0);
    return any ? AAUDIO_CALLBACK_RESULT_CONTINUE : AAUDIO_CALLBACK_RESULT_STOP;
}

/// Under pool.lock: opens the output at the first play, and starts it when the mixer stopped itself.
void ensure_running(Pool &pool) {
    if (pool.output == nullptr) {
        AAudioStreamBuilder *builder = nullptr;
        if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK) return;
        AAudioStreamBuilder_setSampleRate(builder, kOutRate);
        AAudioStreamBuilder_setChannelCount(builder, 2);
        AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_FLOAT);
        AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
        AAudioStreamBuilder_setDataCallback(builder, mix, &pool);
        if (AAudioStreamBuilder_openStream(builder, &pool.output) != AAUDIO_OK) pool.output = nullptr;
        AAudioStreamBuilder_delete(builder);
    }
    if (pool.output != nullptr && !pool.running) {
        pool.running = true;
        AAudioStream_requestStart(pool.output);
    }
}

// ---- the natives ----

Pool *pool_of(JNIEnv *env, jobject thiz) {
    static jfieldID field = env->GetFieldID(env->GetObjectClass(thiz), "mNativeContext", "J");
    return reinterpret_cast<Pool *>(static_cast<intptr_t>(env->GetLongField(thiz, field)));
}

void set_pool(JNIEnv *env, jobject thiz, Pool *pool) {
    static jfieldID field = env->GetFieldID(env->GetObjectClass(thiz), "mNativeContext", "J");
    env->SetLongField(thiz, field, static_cast<jlong>(reinterpret_cast<intptr_t>(pool)));
}

jint native_setup(JNIEnv *env, jobject thiz, jint max_streams, jobject, jstring) {
    auto *pool = new Pool;
    pool->max_streams = std::clamp<int>(max_streams, 1, 32);  // StreamManager's kMaxStreams
    pool->streams.resize(static_cast<size_t>(pool->max_streams));
    env->GetJavaVM(&pool->vm);
    pool->object = env->NewGlobalRef(thiz);
    pool->post_event = env->GetMethodID(env->GetObjectClass(thiz), "postEventFromNative", "(IIILjava/lang/Object;)V");
    set_pool(env, thiz, pool);
    return 0;
}

void native_release(JNIEnv *env, jobject thiz) {
    Pool *pool = pool_of(env, thiz);
    if (pool == nullptr) return;
    set_pool(env, thiz, nullptr);
    AAudioStream *output;
    {
        std::lock_guard<std::mutex> hold(pool->lock);
        pool->released = true;
        for (Stream &stream : pool->streams) {
            stream.state = Stream::idle;
            stream.sound.reset();
        }
        output = pool->output;
        pool->output = nullptr;
    }
    if (output != nullptr) {
        AAudioStream_requestStop(output);
        AAudioStream_close(output);
    }
    // A decode thread that is still running holds its own reference to the pool state through `pending_posts`.
    while (true) {
        {
            std::lock_guard<std::mutex> hold(pool->lock);
            if (pool->pending_posts == 0) break;
        }
        usleep(1000);
    }
    env->DeleteGlobalRef(pool->object);
    delete pool;
}

jint load_fd(JNIEnv *env, jobject thiz, jobject descriptor, jlong offset, jlong length, jint) {
    Pool *pool = pool_of(env, thiz);
    if (pool == nullptr) return 0;
    jclass descriptor_class = env->GetObjectClass(descriptor);
    jfieldID field = env->GetFieldID(descriptor_class, "descriptor", "I");
    const int fd = env->GetIntField(descriptor, field);
    // The Java side closes its descriptor when _load returns, so the file is read now.
    auto file = std::make_shared<std::vector<uint8_t>>();
    bool readable = fd >= 0 && offset >= 0;
    if (readable) {
        off_t size = lseek(fd, 0, SEEK_END);
        size_t want =
            length > 0 ? static_cast<size_t>(length) : (size > offset ? static_cast<size_t>(size - offset) : 0);
        if (want > kMaxFileBytes) readable = false;
        else {
            file->resize(want);
            size_t done = 0;
            while (done < want) {
                const ssize_t n = pread(fd, file->data() + done, want - done, offset + static_cast<off_t>(done));
                if (n <= 0) break;
                done += static_cast<size_t>(n);
            }
            file->resize(done);
        }
    }
    auto sound = std::make_shared<Sound>();
    {
        std::lock_guard<std::mutex> hold(pool->lock);
        do {
            pool->next_sound = pool->next_sound == INT32_MAX ? 1 : pool->next_sound + 1;
        } while (find_sound(*pool, pool->next_sound) != nullptr);
        sound->id = pool->next_sound;
        pool->sounds.push_back(sound);
        ++pool->pending_posts;
    }
    const int id = sound->id;
    std::thread([pool, sound, file, readable]() {
        Sound decoded;
        const bool ok = readable && (decode_wav(*file, decoded) || decode_ogg(*file, decoded));
        {
            std::lock_guard<std::mutex> hold(pool->lock);
            if (ok) {
                sound->frames = std::move(decoded.frames);
                sound->rate = decoded.rate;
                sound->channels = decoded.channels;
            }
            sound->state = ok ? Sound::ready : Sound::failed;
        }
        JNIEnv *env = nullptr;
        JavaVMAttachArgs arguments = {JNI_VERSION_1_6, const_cast<char *>("SoundPool_decode"), nullptr};
        bool attached = false;
        if (pool->vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_OK) {
            attached = pool->vm->AttachCurrentThread(&env, &arguments) == JNI_OK;
        }
        if (env != nullptr && !pool->released) {
            env->CallVoidMethod(pool->object, pool->post_event, kSampleLoaded, static_cast<jint>(sound->id),
                                static_cast<jint>(ok ? 0 : kUnknownError), nullptr);
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
        if (attached) pool->vm->DetachCurrentThread();
        std::lock_guard<std::mutex> hold(pool->lock);
        --pool->pending_posts;
    }).detach();
    return id;
}

jboolean unload(JNIEnv *env, jobject thiz, jint id) {
    Pool *pool = pool_of(env, thiz);
    if (pool == nullptr) return JNI_FALSE;
    std::lock_guard<std::mutex> hold(pool->lock);
    for (auto it = pool->sounds.begin(); it != pool->sounds.end(); ++it) {
        if ((*it)->id == id) {
            pool->sounds.erase(it);
            return JNI_TRUE;
        }
    }
    return JNI_FALSE;
}

jint play(JNIEnv *env, jobject thiz, jint sound_id, jfloat left, jfloat right, jint priority, jint loop, jfloat rate,
          jint) {
    Pool *pool = pool_of(env, thiz);
    if (pool == nullptr) return 0;
    if (left != std::clamp(left, 0.0f, 1.0f) || right != std::clamp(right, 0.0f, 1.0f)) left = right = 1.0f;
    rate = std::clamp(rate, 0.125f, 8.0f);
    if (loop < -1) loop = -1;
    std::lock_guard<std::mutex> hold(pool->lock);
    std::shared_ptr<Sound> sound = find_sound(*pool, sound_id);
    if (sound == nullptr || sound->state != Sound::ready) return 0;
    Stream *chosen = nullptr;
    for (Stream &stream : pool->streams) {
        if (stream.state == Stream::idle) {
            chosen = &stream;
            break;
        }
    }
    if (chosen == nullptr) {
        for (Stream &stream : pool->streams) {
            if (stream.priority > priority) continue;
            if (chosen == nullptr || stream.priority < chosen->priority ||
                (stream.priority == chosen->priority && stream.started < chosen->started)) {
                chosen = &stream;
            }
        }
    }
    if (chosen == nullptr) return 0;
    do {
        pool->next_stream = pool->next_stream == INT32_MAX ? 1 : pool->next_stream + 1;
    } while (find_stream(*pool, pool->next_stream) != nullptr);
    *chosen = Stream();
    chosen->id = pool->next_stream;
    chosen->sound = sound;
    chosen->left = left;
    chosen->right = right;
    chosen->priority = priority;
    chosen->loop = loop;
    chosen->rate = rate;
    chosen->started = ++pool->plays;
    chosen->state = Stream::playing;
    ensure_running(*pool);
    return chosen->id;
}

void pause(JNIEnv *env, jobject thiz, jint id) {
    Pool *pool = pool_of(env, thiz);
    if (pool == nullptr) return;
    std::lock_guard<std::mutex> hold(pool->lock);
    Stream *stream = find_stream(*pool, id);
    if (stream != nullptr && stream->state == Stream::playing) stream->state = Stream::paused;
}

void resume(JNIEnv *env, jobject thiz, jint id) {
    Pool *pool = pool_of(env, thiz);
    if (pool == nullptr) return;
    std::lock_guard<std::mutex> hold(pool->lock);
    Stream *stream = find_stream(*pool, id);
    if (stream != nullptr && stream->state == Stream::paused) {
        stream->state = Stream::playing;
        stream->auto_paused = false;
        ensure_running(*pool);
    }
}

void auto_pause(JNIEnv *env, jobject thiz) {
    Pool *pool = pool_of(env, thiz);
    if (pool == nullptr) return;
    std::lock_guard<std::mutex> hold(pool->lock);
    for (Stream &stream : pool->streams) {
        if (stream.state == Stream::playing) {
            stream.state = Stream::paused;
            stream.auto_paused = true;
        }
    }
}

void auto_resume(JNIEnv *env, jobject thiz) {
    Pool *pool = pool_of(env, thiz);
    if (pool == nullptr) return;
    std::lock_guard<std::mutex> hold(pool->lock);
    for (Stream &stream : pool->streams) {
        if (stream.auto_paused) {
            if (stream.state == Stream::paused) stream.state = Stream::playing;
            stream.auto_paused = false;
        }
    }
    ensure_running(*pool);
}

void stop(JNIEnv *env, jobject thiz, jint id) {
    Pool *pool = pool_of(env, thiz);
    if (pool == nullptr) return;
    std::lock_guard<std::mutex> hold(pool->lock);
    Stream *stream = find_stream(*pool, id);
    if (stream != nullptr && stream->state != Stream::idle) {
        stream->state = Stream::idle;
        stream->sound.reset();
    }
}

void set_priority(JNIEnv *env, jobject thiz, jint id, jint priority) {
    Pool *pool = pool_of(env, thiz);
    if (pool == nullptr) return;
    std::lock_guard<std::mutex> hold(pool->lock);
    if (Stream *stream = find_stream(*pool, id)) stream->priority = priority;
}

void set_loop(JNIEnv *env, jobject thiz, jint id, jint loop) {
    Pool *pool = pool_of(env, thiz);
    if (pool == nullptr) return;
    if (loop < -1) loop = -1;
    std::lock_guard<std::mutex> hold(pool->lock);
    if (Stream *stream = find_stream(*pool, id)) stream->loop = loop;
}

void set_rate(JNIEnv *env, jobject thiz, jint id, jfloat rate) {
    Pool *pool = pool_of(env, thiz);
    if (pool == nullptr) return;
    rate = std::clamp(rate, 0.125f, 8.0f);
    std::lock_guard<std::mutex> hold(pool->lock);
    if (Stream *stream = find_stream(*pool, id)) stream->rate = rate;
}

void set_volume(JNIEnv *env, jobject thiz, jint id, jfloat left, jfloat right) {
    Pool *pool = pool_of(env, thiz);
    if (pool == nullptr) return;
    if (left != std::clamp(left, 0.0f, 1.0f) || right != std::clamp(right, 0.0f, 1.0f)) left = right = 1.0f;
    std::lock_guard<std::mutex> hold(pool->lock);
    if (Stream *stream = find_stream(*pool, id)) {
        stream->left = left;
        stream->right = right;
    }
}

void mute(JNIEnv *env, jobject thiz, jboolean muting) {
    Pool *pool = pool_of(env, thiz);
    if (pool == nullptr) return;
    std::lock_guard<std::mutex> hold(pool->lock);
    pool->muted = muting;
}

const JNINativeMethod kMethods[] = {
    {"_load", "(Ljava/io/FileDescriptor;JJI)I", reinterpret_cast<void *>(load_fd)},
    {"native_setup", "(ILjava/lang/Object;Ljava/lang/String;)I", reinterpret_cast<void *>(native_setup)},
    {"native_release", "()V", reinterpret_cast<void *>(native_release)},
    {"unload", "(I)Z", reinterpret_cast<void *>(unload)},
    {"_play", "(IFFIIFI)I", reinterpret_cast<void *>(play)},
    {"pause", "(I)V", reinterpret_cast<void *>(pause)},
    {"resume", "(I)V", reinterpret_cast<void *>(resume)},
    {"autoPause", "()V", reinterpret_cast<void *>(auto_pause)},
    {"autoResume", "()V", reinterpret_cast<void *>(auto_resume)},
    {"stop", "(I)V", reinterpret_cast<void *>(stop)},
    {"setPriority", "(II)V", reinterpret_cast<void *>(set_priority)},
    {"setLoop", "(II)V", reinterpret_cast<void *>(set_loop)},
    {"setRate", "(IF)V", reinterpret_cast<void *>(set_rate)},
    {"_setVolume", "(IFF)V", reinterpret_cast<void *>(set_volume)},
    {"_mute", "(Z)V", reinterpret_cast<void *>(mute)},
};

}  // namespace

int register_android_media_SoundPool(JNIEnv *env) {
    return RegisterMethodsOrDie(env, "android/media/SoundPool", kMethods, NELEM(kMethods));
}

}  // namespace android
