// Ours in place of AOSP's media/jni/android_media_MediaCodec.cpp and android_media_MediaCodecList.cpp (ADR-0060
// e2b3): MediaCodec's and MediaCodecList's natives on libmediandk's AMediaCodec, the codec core
// (runtime/android/media_codec.cpp), instead of libstagefright's MediaCodec, following the tag's answers:
// - native_setup makes the codec by name or by type; none answers IllegalArgumentException, as Android 15 does for an
//   unknown type (media-codec-test). mNativeContext holds our context, the codec and a weak reference for callbacks.
// - A media format crosses as the tag's Map<String, Object>: Integer, Long, Float, Double, String and ByteBuffer
//   values, a rect as four "<name>-left", "-top", "-right" and "-bottom" Integers, as the tag's ConvertMessageToMap.
// - The codec's errors: AMEDIA_ERROR_INVALID_OPERATION is IllegalStateException, INVALID_PARAMETER
//   IllegalArgumentException, UNSUPPORTED IllegalArgumentException for configure's surface or crypto (e2c), any other
//   IllegalStateException; dequeues answer the core's INFO_* codes as they are.
// - getImage gives a MediaCodec.MediaImage of an output buffer's NV12 frame through its MediaImage2 description (three
//   planes, Cb and Cr interleaved: column increment 2), as the tag's getImage does for YUV buffers; an input image is
//   null (the decoders' input is a byte stream).
// - setCallback: the core's asynchronous mode, its thread's calls posted to Java with postEventFromNative
//   (EVENT_CALLBACK and CB_*), as the tag's AMessage handler posts them.
// - OnFrameRenderedListener (ADR-0060 e2c3): the core's frame rendered callback, each frame posted as
//   EVENT_FRAME_RENDERED with the tag's map ("0-media-time-us", "0-system-nano"); setVideoScalingMode's mode reaches
//   the core for a host window's frames. A codec that released itself, its window gone, answers every call with
//   IllegalStateException "Invalid to call at Released state; only valid in executing state", Android 15's
//   [verified: media-window-test].
// - MediaCodecList lists libmediandk's codecs (minato_media_codec_at) with capabilities that VideoToolbox decodes on
//   Apple's chips: H.264's Baseline, Constrained Baseline, Main, High and Constrained High to level 5.2, HEVC's Main
//   and Main 10 to level 5.1, up to 4096 x 4096, with adaptive playback; hardware-accelerated and vendor. The audio
//   decoders (e2f) and their aliases are listed as Android 15 lists c2.android.aac.decoder [verified:
//   media-codec-audio-test] and c2.android.mp3.decoder, c2.android.flac.decoder and c2.android.raw.decoder
//   [verified: media-codec-decoders-test] and c2.android.opus.decoder [verified: media-codec-opus-test]: software
//   only, their canonical names, AAC's profiles, and the limits of Android's media_codecs_sw.xml, 32 instances each.
// Left out, their natives not registered: secure input, the block model, input surfaces, tunneling, vendor parameters
// and audio presentations (UnsatisfiedLinkError, as no codec of ours has them).
#define LOG_TAG "MediaCodec_JNI"

#include <android/native_window.h>
#include <dlfcn.h>
#include <jni.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <nativehelper/JNIHelp.h>
#include <string.h>

#include <mutex>
#include <string>
#include <vector>

#include <APKRuntime/media_codec.h>
#include <log/log.h>

#include "core_jni_helpers.h"

// android_view_Surface.cpp's: a Java Surface's window, acquired for the caller (libandroid's
// ANativeWindow_fromSurface).
extern "C" ANativeWindow *minato_ANativeWindow_fromSurface(JNIEnv *env, jobject surface);

namespace android {
namespace {

constexpr int kEventCallback = 1, kEventFrameRendered = 3;
constexpr int kInputAvailable = 1, kOutputAvailable = 2, kError = 3, kOutputFormatChange = 4;
constexpr int kTypeYuv = 1;

struct {
    jfieldID context;  // MediaCodec.mNativeContext
    jmethodID postEvent;
    jclass bufferInfoClass;  // global
    jmethodID bufferInfoConstructor, bufferInfoSet;
    jclass hashMapClass, integerClass, longClass, floatClass, doubleClass, stringClass, byteBufferClass;  // global
    jmethodID hashMapConstructor, mapPut, integerValueOf, longValueOf, floatValueOf, doubleValueOf, byteBufferWrap;
    jmethodID integerValue, longValue, floatValue, doubleValue, bufferDuplicate, bufferRemaining, bufferGet;
    jmethodID bufferOrder, nativeOrder;
    jclass byteOrderClass;  // global
    jclass mediaImageClass, rectClass;  // global
    jmethodID mediaImageConstructor, rectConstructor;
    jclass capabilitiesClass, profileLevelClass;  // global
    jmethodID capabilitiesConstructor, profileLevelConstructor;
    jfieldID profileField, levelField;
} gFields;

// libmediandk's private functions, looked up once.
struct Private {
    decltype(&minato_AMediaFormat_count) count = nullptr;
    decltype(&minato_AMediaFormat_entry) entry = nullptr;
    decltype(&minato_media_codec_count) codecs = nullptr;
    decltype(&minato_media_codec_at) codecAt = nullptr;
    decltype(&minato_AMediaCodec_isReleased) isReleased = nullptr;
    decltype(&minato_AMediaCodec_setVideoScalingMode) setScaling = nullptr;
};

const Private &private_functions() {
    static const Private functions = [] {
        Private found;
        if (void *library = dlopen("libmediandk.so", RTLD_NOW)) {
            found.count = reinterpret_cast<decltype(found.count)>(dlsym(library, "minato_AMediaFormat_count"));
            found.entry = reinterpret_cast<decltype(found.entry)>(dlsym(library, "minato_AMediaFormat_entry"));
            found.codecs = reinterpret_cast<decltype(found.codecs)>(dlsym(library, "minato_media_codec_count"));
            found.codecAt = reinterpret_cast<decltype(found.codecAt)>(dlsym(library, "minato_media_codec_at"));
            found.isReleased =
                reinterpret_cast<decltype(found.isReleased)>(dlsym(library, "minato_AMediaCodec_isReleased"));
            found.setScaling = reinterpret_cast<decltype(found.setScaling)>(
                dlsym(library, "minato_AMediaCodec_setVideoScalingMode"));
        }
        return found;
    }();
    return functions;
}

// A Java MediaCodec's native side.
struct Codec {
    AMediaCodec *codec = nullptr;
    std::string name;
    JavaVM *vm = nullptr;
    jobject weak = nullptr;  // the MediaCodec, for callbacks
    bool asynchronous = false;
    int64_t times[8] = {};  // each output buffer's time, for its image
};

Codec *codec_of(JNIEnv *env, jobject thiz) {
    return reinterpret_cast<Codec *>(env->GetLongField(thiz, gFields.context));
}

/// The codec, or NULL with IllegalStateException for a released one, by the app or by itself.
AMediaCodec *live(JNIEnv *env, jobject thiz) {
    Codec *codec = codec_of(env, thiz);
    if (codec == nullptr || codec->codec == nullptr) {
        jniThrowException(env, "java/lang/IllegalStateException", nullptr);
        return nullptr;
    }
    const Private &functions = private_functions();
    if (functions.isReleased != nullptr && functions.isReleased(codec->codec) != 0) {
        jniThrowException(env, "java/lang/IllegalStateException",
                          "Invalid to call at Released state; only valid in executing state");
        return nullptr;
    }
    return codec->codec;
}

/// Throws the Java exception of a status not AMEDIA_OK; true when it threw.
bool throw_status(JNIEnv *env, media_status_t status, const char *what) {
    if (status == AMEDIA_OK) return false;
    const char *type = status == AMEDIA_ERROR_INVALID_PARAMETER || status == AMEDIA_ERROR_UNSUPPORTED
                               ? "java/lang/IllegalArgumentException"
                               : "java/lang/IllegalStateException";
    jniThrowExceptionFmt(env, type, "%s failed: %d", what, static_cast<int>(status));
    return true;
}

jobject boxed(JNIEnv *env, jclass type, jmethodID valueOf, ...) {
    va_list arguments;
    va_start(arguments, valueOf);
    jobject value = env->CallStaticObjectMethodV(type, valueOf, arguments);
    va_end(arguments);
    return value;
}

/// The format as the tag's Map<String, Object>.
jobject map_of(JNIEnv *env, AMediaFormat *format) {
    jobject map = env->NewObject(gFields.hashMapClass, gFields.hashMapConstructor);
    if (map == nullptr || format == nullptr) return map;
    const Private &functions = private_functions();
    if (functions.count == nullptr || functions.entry == nullptr) return map;
    const size_t count = functions.count(format);
    for (size_t index = 0; index < count; index++) {
        const char *name = nullptr;
        int kind = 0;
        if (functions.entry(format, index, &name, &kind) != 0) continue;
        jobject value = nullptr;
        switch (kind) {
            case MINATO_MEDIA_FORMAT_INT32: {
                int32_t number = 0;
                AMediaFormat_getInt32(format, name, &number);
                value = boxed(env, gFields.integerClass, gFields.integerValueOf, static_cast<jint>(number));
                break;
            }
            case MINATO_MEDIA_FORMAT_INT64:
            case MINATO_MEDIA_FORMAT_SIZE: {
                int64_t number = 0;
                size_t size = 0;
                if (kind == MINATO_MEDIA_FORMAT_SIZE && AMediaFormat_getSize(format, name, &size)) {
                    number = static_cast<int64_t>(size);
                } else {
                    AMediaFormat_getInt64(format, name, &number);
                }
                value = boxed(env, gFields.longClass, gFields.longValueOf, static_cast<jlong>(number));
                break;
            }
            case MINATO_MEDIA_FORMAT_FLOAT: {
                float number = 0;
                AMediaFormat_getFloat(format, name, &number);
                value = boxed(env, gFields.floatClass, gFields.floatValueOf, static_cast<jdouble>(number));
                break;
            }
            case MINATO_MEDIA_FORMAT_DOUBLE: {
                double number = 0;
                AMediaFormat_getDouble(format, name, &number);
                value = boxed(env, gFields.doubleClass, gFields.doubleValueOf, number);
                break;
            }
            case MINATO_MEDIA_FORMAT_STRING: {
                const char *text = nullptr;
                AMediaFormat_getString(format, name, &text);
                value = env->NewStringUTF(text != nullptr ? text : "");
                break;
            }
            case MINATO_MEDIA_FORMAT_BUFFER: {
                void *data = nullptr;
                size_t size = 0;
                AMediaFormat_getBuffer(format, name, &data, &size);
                jbyteArray bytes = env->NewByteArray(static_cast<jsize>(size));
                if (bytes == nullptr) return map;
                env->SetByteArrayRegion(bytes, 0, static_cast<jsize>(size), static_cast<const jbyte *>(data));
                value = env->CallStaticObjectMethod(gFields.byteBufferClass, gFields.byteBufferWrap, bytes);
                env->DeleteLocalRef(bytes);
                break;
            }
            case MINATO_MEDIA_FORMAT_RECT: {
                int32_t sides[4] = {0, 0, 0, 0};
                AMediaFormat_getRect(format, name, &sides[0], &sides[1], &sides[2], &sides[3]);
                const char *suffixes[4] = {"-left", "-top", "-right", "-bottom"};
                for (int side = 0; side < 4; side++) {
                    jstring key = env->NewStringUTF((std::string(name) + suffixes[side]).c_str());
                    jobject number = boxed(env, gFields.integerClass, gFields.integerValueOf, sides[side]);
                    env->CallObjectMethod(map, gFields.mapPut, key, number);
                    env->DeleteLocalRef(key);
                    env->DeleteLocalRef(number);
                }
                continue;
            }
            default: continue;
        }
        if (value == nullptr) continue;
        jstring key = env->NewStringUTF(name);
        env->CallObjectMethod(map, gFields.mapPut, key, value);
        env->DeleteLocalRef(key);
        env->DeleteLocalRef(value);
    }
    return map;
}

/// An AMediaFormat of the keys and values configure and setParameters take; NULL with an exception thrown.
AMediaFormat *format_of(JNIEnv *env, jobjectArray keys, jobjectArray values) {
    AMediaFormat *format = AMediaFormat_new();
    if (format == nullptr || keys == nullptr || values == nullptr) return format;
    const jsize count = env->GetArrayLength(keys);
    if (env->GetArrayLength(values) != count) {
        AMediaFormat_delete(format);
        jniThrowException(env, "java/lang/IllegalArgumentException", nullptr);
        return nullptr;
    }
    for (jsize index = 0; index < count; index++) {
        auto key = static_cast<jstring>(env->GetObjectArrayElement(keys, index));
        jobject value = env->GetObjectArrayElement(values, index);
        if (key == nullptr) continue;
        const char *name = env->GetStringUTFChars(key, nullptr);
        if (value == nullptr) {
            // nothing to set
        } else if (env->IsInstanceOf(value, gFields.integerClass)) {
            AMediaFormat_setInt32(format, name, env->CallIntMethod(value, gFields.integerValue));
        } else if (env->IsInstanceOf(value, gFields.longClass)) {
            AMediaFormat_setInt64(format, name, env->CallLongMethod(value, gFields.longValue));
        } else if (env->IsInstanceOf(value, gFields.floatClass)) {
            AMediaFormat_setFloat(format, name, env->CallFloatMethod(value, gFields.floatValue));
        } else if (env->IsInstanceOf(value, gFields.doubleClass)) {
            AMediaFormat_setDouble(format, name, env->CallDoubleMethod(value, gFields.doubleValue));
        } else if (env->IsInstanceOf(value, gFields.stringClass)) {
            const char *text = env->GetStringUTFChars(static_cast<jstring>(value), nullptr);
            AMediaFormat_setString(format, name, text);
            env->ReleaseStringUTFChars(static_cast<jstring>(value), text);
        } else if (env->IsInstanceOf(value, gFields.byteBufferClass)) {
            // Its remaining bytes, read through a duplicate, so that the app's position stays.
            jobject copy = env->CallObjectMethod(value, gFields.bufferDuplicate);
            const jint size = copy != nullptr ? env->CallIntMethod(copy, gFields.bufferRemaining) : 0;
            jbyteArray bytes = env->NewByteArray(size);
            if (bytes != nullptr && copy != nullptr) {
                env->DeleteLocalRef(env->CallObjectMethod(copy, gFields.bufferGet, bytes));
                std::vector<jbyte> data(static_cast<size_t>(size));
                env->GetByteArrayRegion(bytes, 0, size, data.data());
                AMediaFormat_setBuffer(format, name, data.data(), data.size());
            }
            if (bytes != nullptr) env->DeleteLocalRef(bytes);
            if (copy != nullptr) env->DeleteLocalRef(copy);
        }
        env->ReleaseStringUTFChars(key, name);
        env->DeleteLocalRef(key);
        if (value != nullptr) env->DeleteLocalRef(value);
        if (env->ExceptionCheck()) {
            AMediaFormat_delete(format);
            return nullptr;
        }
    }
    return format;
}

jobject buffer_info(JNIEnv *env, const AMediaCodecBufferInfo &info) {
    jobject object = env->NewObject(gFields.bufferInfoClass, gFields.bufferInfoConstructor);
    if (object != nullptr) {
        env->CallVoidMethod(object, gFields.bufferInfoSet, info.offset, info.size, info.presentationTimeUs,
                            static_cast<jint>(info.flags));
    }
    return object;
}

// The core's asynchronous calls, posted to the Java MediaCodec on the calling thread, attached to the VM for it.
void post(Codec *codec, int kind, int index, jobject (*make)(JNIEnv *, void *), void *argument) {
    JNIEnv *env = nullptr;
    bool attached = false;
    if (codec->vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_OK) {
        if (codec->vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return;
        attached = true;
    }
    jobject target = env->NewLocalRef(codec->weak);
    if (target != nullptr) {
        jobject object = make != nullptr ? make(env, argument) : nullptr;
        env->CallVoidMethod(target, gFields.postEvent, kEventCallback, kind, index, object);
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (object != nullptr) env->DeleteLocalRef(object);
        env->DeleteLocalRef(target);
    }
    if (attached) codec->vm->DetachCurrentThread();
}

void on_input(AMediaCodec *, void *userdata, int32_t index) {
    post(static_cast<Codec *>(userdata), kInputAvailable, index, nullptr, nullptr);
}

void on_output(AMediaCodec *, void *userdata, int32_t index, AMediaCodecBufferInfo *info) {
    if (index >= 0 && index < 8) static_cast<Codec *>(userdata)->times[index] = info->presentationTimeUs;
    post(static_cast<Codec *>(userdata), kOutputAvailable, index,
         [](JNIEnv *env, void *argument) { return buffer_info(env, *static_cast<AMediaCodecBufferInfo *>(argument)); },
         info);
}

void on_format(AMediaCodec *, void *userdata, AMediaFormat *format) {
    post(static_cast<Codec *>(userdata), kOutputFormatChange, 0,
         [](JNIEnv *env, void *argument) { return map_of(env, static_cast<AMediaFormat *>(argument)); }, format);
}

void on_error(AMediaCodec *, void *userdata, media_status_t, int32_t, const char *) {
    post(static_cast<Codec *>(userdata), kError, 0, nullptr, nullptr);
}

// MediaCodec's natives.

void MediaCodec_init(JNIEnv *env, jclass clazz) {
    gFields.context = GetFieldIDOrDie(env, clazz, "mNativeContext", "J");
    gFields.postEvent = GetMethodIDOrDie(env, clazz, "postEventFromNative", "(IIILjava/lang/Object;)V");
}

void MediaCodec_setup(JNIEnv *env, jobject thiz, jstring name, jboolean nameIsType, jboolean encoder, jint, jint) {
    if (name == nullptr) {
        jniThrowException(env, "java/lang/NullPointerException", nullptr);
        return;
    }
    const char *text = env->GetStringUTFChars(name, nullptr);
    AMediaCodec *made = encoder ? (nameIsType ? AMediaCodec_createEncoderByType(text) : nullptr)
                                : (nameIsType ? AMediaCodec_createDecoderByType(text)
                                              : AMediaCodec_createCodecByName(text));
    if (made == nullptr) {
        jniThrowExceptionFmt(env, "java/lang/IllegalArgumentException", "Failed to initialize %s, error %#x", text,
                             static_cast<unsigned>(-2));
        env->ReleaseStringUTFChars(name, text);
        return;
    }
    env->ReleaseStringUTFChars(name, text);
    auto *codec = new Codec;
    codec->codec = made;
    char *made_name = nullptr;
    if (AMediaCodec_getName(made, &made_name) == AMEDIA_OK) {
        codec->name = made_name;
        AMediaCodec_releaseName(made, made_name);
    }
    env->GetJavaVM(&codec->vm);
    codec->weak = env->NewWeakGlobalRef(thiz);
    env->SetLongField(thiz, gFields.context, reinterpret_cast<jlong>(codec));
}

void MediaCodec_release(JNIEnv *env, jobject thiz) {
    Codec *codec = codec_of(env, thiz);
    if (codec == nullptr) return;
    env->SetLongField(thiz, gFields.context, 0);
    if (codec->codec != nullptr) AMediaCodec_delete(codec->codec);
    if (codec->weak != nullptr) env->DeleteWeakGlobalRef(codec->weak);
    delete codec;
}

void MediaCodec_reset(JNIEnv *env, jobject thiz) {
    if (AMediaCodec *codec = live(env, thiz)) AMediaCodec_stop(codec);
}

void MediaCodec_configure(JNIEnv *env, jobject thiz, jobjectArray keys, jobjectArray values, jobject surface,
                          jobject crypto, jobject descrambler, jint flags) {
    AMediaCodec *codec = live(env, thiz);
    if (codec == nullptr) return;
    if (crypto != nullptr || descrambler != nullptr) {
        jniThrowException(env, "java/lang/IllegalArgumentException", "not supported yet: crypto");
        return;  // no crypto or descrambler
    }
    ANativeWindow *window = surface != nullptr ? minato_ANativeWindow_fromSurface(env, surface) : nullptr;
    if (surface != nullptr && window == nullptr) {
        jniThrowException(env, "java/lang/IllegalArgumentException", "The surface has been released");
        return;
    }
    AMediaFormat *format = format_of(env, keys, values);
    if (format == nullptr) {
        if (window != nullptr) ANativeWindow_release(window);
        return;
    }
    const media_status_t status = AMediaCodec_configure(codec, format, window, nullptr, static_cast<uint32_t>(flags));
    AMediaFormat_delete(format);
    if (window != nullptr) ANativeWindow_release(window);  // the codec holds its own
    if (status == AMEDIA_ERROR_UNKNOWN && window != nullptr) {
        // The surface has another producer (a codec, an EGL window surface, a Vulkan surface): Android's connect
        // fails with BAD_VALUE, which its natives throw as IllegalArgumentException [verified: media-window-test].
        jniThrowException(env, "java/lang/IllegalArgumentException", "configure failed: the surface is connected");
        return;
    }
    throw_status(env, status, "configure");
}

void MediaCodec_setCallback(JNIEnv *env, jobject thiz, jobject callback) {
    Codec *native = codec_of(env, thiz);
    AMediaCodec *codec = live(env, thiz);
    if (codec == nullptr) return;
    AMediaCodecOnAsyncNotifyCallback callbacks{};
    if (callback != nullptr) callbacks = {on_input, on_output, on_format, on_error};
    native->asynchronous = callback != nullptr;
    throw_status(env, AMediaCodec_setAsyncNotifyCallback(codec, callbacks, native), "setCallback");
}

void MediaCodec_start(JNIEnv *env, jobject thiz) {
    if (AMediaCodec *codec = live(env, thiz)) throw_status(env, AMediaCodec_start(codec), "start");
}

void MediaCodec_stop(JNIEnv *env, jobject thiz) {
    if (AMediaCodec *codec = live(env, thiz)) throw_status(env, AMediaCodec_stop(codec), "stop");
}

void MediaCodec_flush(JNIEnv *env, jobject thiz) {
    if (AMediaCodec *codec = live(env, thiz)) throw_status(env, AMediaCodec_flush(codec), "flush");
}

void MediaCodec_queueInputBuffer(JNIEnv *env, jobject thiz, jint index, jint offset, jint size, jlong timeUs,
                                 jint flags) {
    AMediaCodec *codec = live(env, thiz);
    if (codec == nullptr) return;
    if (index < 0 || offset < 0 || size < 0) {
        jniThrowException(env, "java/lang/IllegalArgumentException", nullptr);
        return;
    }
    throw_status(env,
                 AMediaCodec_queueInputBuffer(codec, static_cast<size_t>(index), offset, static_cast<size_t>(size),
                                              static_cast<uint64_t>(timeUs), static_cast<uint32_t>(flags)),
                 "queueInputBuffer");
}

/// A dequeue's answer: an index, an INFO_* code, or an exception for an error.
jint dequeued(JNIEnv *env, ssize_t answer, const char *what) {
    if (answer >= 0 || answer == AMEDIACODEC_INFO_TRY_AGAIN_LATER ||
        answer == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED || answer == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
        return static_cast<jint>(answer);
    }
    throw_status(env, static_cast<media_status_t>(answer), what);
    return 0;
}

jint MediaCodec_dequeueInputBuffer(JNIEnv *env, jobject thiz, jlong timeoutUs) {
    AMediaCodec *codec = live(env, thiz);
    if (codec == nullptr) return 0;
    return dequeued(env, AMediaCodec_dequeueInputBuffer(codec, timeoutUs), "dequeueInputBuffer");
}

jint MediaCodec_dequeueOutputBuffer(JNIEnv *env, jobject thiz, jobject info, jlong timeoutUs) {
    AMediaCodec *codec = live(env, thiz);
    if (codec == nullptr) return 0;
    AMediaCodecBufferInfo answer{};
    const ssize_t index = AMediaCodec_dequeueOutputBuffer(codec, &answer, timeoutUs);
    if (index >= 0 && index < 8) codec_of(env, thiz)->times[index] = answer.presentationTimeUs;
    if (index >= 0 && info != nullptr) {
        env->CallVoidMethod(info, gFields.bufferInfoSet, answer.offset, answer.size, answer.presentationTimeUs,
                            static_cast<jint>(answer.flags));
    }
    return dequeued(env, index, "dequeueOutputBuffer");
}

void MediaCodec_releaseOutputBuffer(JNIEnv *env, jobject thiz, jint index, jboolean render, jboolean update_time,
                                    jlong timestamp_ns) {
    AMediaCodec *codec = live(env, thiz);
    if (codec == nullptr) return;
    if (index < 0) {
        jniThrowException(env, "java/lang/IllegalArgumentException", nullptr);
        return;
    }
    const size_t buffer = static_cast<size_t>(index);
    throw_status(env,
                 render && update_time ? AMediaCodec_releaseOutputBufferAtTime(codec, buffer, timestamp_ns)
                                       : AMediaCodec_releaseOutputBuffer(codec, buffer, render),
                 "releaseOutputBuffer");
}

void MediaCodec_signalEndOfInputStream(JNIEnv *env, jobject thiz) {
    if (AMediaCodec *codec = live(env, thiz)) {
        throw_status(env, AMediaCodec_signalEndOfInputStream(codec), "signalEndOfInputStream");
    }
}

/// A direct ByteBuffer of `size` bytes at `data`, in the platform's byte order; NULL for no data.
jobject direct_buffer(JNIEnv *env, uint8_t *data, size_t size) {
    if (data == nullptr) return nullptr;
    jobject buffer = env->NewDirectByteBuffer(data, static_cast<jlong>(size));
    if (buffer == nullptr) return nullptr;
    jobject order = env->CallStaticObjectMethod(gFields.byteOrderClass, gFields.nativeOrder);
    env->DeleteLocalRef(env->CallObjectMethod(buffer, gFields.bufferOrder, order));
    env->DeleteLocalRef(order);
    return buffer;
}

jobject MediaCodec_getBuffer(JNIEnv *env, jobject thiz, jboolean input, jint index) {
    AMediaCodec *codec = live(env, thiz);
    if (codec == nullptr || index < 0) return nullptr;
    size_t size = 0;
    uint8_t *data = input ? AMediaCodec_getInputBuffer(codec, static_cast<size_t>(index), &size)
                          : AMediaCodec_getOutputBuffer(codec, static_cast<size_t>(index), &size);
    return direct_buffer(env, data, size);
}

jobjectArray MediaCodec_getBuffers(JNIEnv *env, jobject thiz, jboolean input) {
    Codec *native = codec_of(env, thiz);
    AMediaCodec *codec = live(env, thiz);
    if (codec == nullptr) return nullptr;
    if (native->asynchronous) {
        jniThrowException(env, "java/lang/IllegalStateException", nullptr);  // the tag's: none in async mode
        return nullptr;
    }
    const jsize count = input ? 4 : 8;
    jobjectArray buffers = env->NewObjectArray(count, gFields.byteBufferClass, nullptr);
    for (jsize index = 0; buffers != nullptr && index < count; index++) {
        jobject buffer = MediaCodec_getBuffer(env, thiz, input, index);
        if (buffer != nullptr) {
            env->SetObjectArrayElement(buffers, index, buffer);
            env->DeleteLocalRef(buffer);
        }
    }
    return buffers;
}

jobject MediaCodec_getImage(JNIEnv *env, jobject thiz, jboolean input, jint index) {
    AMediaCodec *codec = live(env, thiz);
    if (codec == nullptr || input || index < 0) return nullptr;
    size_t size = 0;
    uint8_t *data = AMediaCodec_getOutputBuffer(codec, static_cast<size_t>(index), &size);
    AMediaFormat *format = AMediaCodec_getOutputFormat(codec);
    int32_t width = 0, height = 0, stride = 0, slice = 0, left = 0, top = 0, right = -1, bottom = -1;
    AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_WIDTH, &width);
    AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_HEIGHT, &height);
    AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_STRIDE, &stride);
    AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_SLICE_HEIGHT, &slice);
    if (!AMediaFormat_getRect(format, AMEDIAFORMAT_KEY_DISPLAY_CROP, &left, &top, &right, &bottom)) {
        right = width - 1, bottom = height - 1;
    }
    AMediaFormat_delete(format);
    const size_t frame = static_cast<size_t>(stride) * static_cast<size_t>(slice + (slice + 1) / 2);
    if (data == nullptr || width <= 0 || height <= 0 || stride < width || slice < height || size < frame) {
        return nullptr;
    }
    // MediaImage2 (media/hardware/VideoAPI.h): type, planes, width, height, bit depth, allocated bit depth, then four
    // planes of offset, column increment, row increment, horizontal and vertical subsampling, 104 bytes.
    const int32_t chroma = stride * slice;
    const int32_t description[26] = {kTypeYuv, 3, width, height, 8, 8,
                                     0, 1, stride, 1, 1,
                                     chroma, 2, stride, 2, 2,
                                     chroma + 1, 2, stride, 2, 2,
                                     0, 0, 0, 0, 0};
    jbyteArray infoBytes = env->NewByteArray(sizeof description);
    if (infoBytes == nullptr) return nullptr;
    env->SetByteArrayRegion(infoBytes, 0, sizeof description, reinterpret_cast<const jbyte *>(description));
    jobject info = env->CallStaticObjectMethod(gFields.byteBufferClass, gFields.byteBufferWrap, infoBytes);
    jobject order = env->CallStaticObjectMethod(gFields.byteOrderClass, gFields.nativeOrder);
    env->DeleteLocalRef(env->CallObjectMethod(info, gFields.bufferOrder, order));
    jobject buffer = direct_buffer(env, data, frame);
    // MediaImage's time is in nanoseconds, the buffer's in microseconds, as the tag's getImage converts it.
    const jlong timestamp = index < 8 ? minato_media_time_ns(codec_of(env, thiz)->times[index]) : 0;
    jobject crop = env->NewObject(gFields.rectClass, gFields.rectConstructor, left, top, right + 1, bottom + 1);
    jobject image = env->NewObject(gFields.mediaImageClass, gFields.mediaImageConstructor, buffer, info, JNI_TRUE,
                                   timestamp, 0, 0, crop);
    env->DeleteLocalRef(infoBytes);
    env->DeleteLocalRef(order);
    return image;
}

jobject MediaCodec_getFormatNative(JNIEnv *env, jobject thiz, jboolean input) {
    AMediaCodec *codec = live(env, thiz);
    if (codec == nullptr) return nullptr;
    AMediaFormat *format = input ? AMediaCodec_getInputFormat(codec) : AMediaCodec_getOutputFormat(codec);
    jobject map = map_of(env, format);
    AMediaFormat_delete(format);
    return map;
}

jobject MediaCodec_getOutputFormatNative(JNIEnv *env, jobject thiz, jint) {
    return MediaCodec_getFormatNative(env, thiz, JNI_FALSE);
}

jstring MediaCodec_getCanonicalName(JNIEnv *env, jobject thiz) {
    Codec *codec = codec_of(env, thiz);
    if (codec == nullptr || codec->codec == nullptr) {
        jniThrowException(env, "java/lang/IllegalStateException", nullptr);
        return nullptr;
    }
    // An alias's codec is its canonical one's (MediaCodecList's minato_media_codec_info).
    const Private &functions = private_functions();
    const size_t count = functions.codecs != nullptr ? functions.codecs() : 0;
    for (size_t index = 0; index < count; index++) {
        const minato_media_codec_info *info = functions.codecAt(index);
        if (info != nullptr && codec->name == info->name) return env->NewStringUTF(info->canonical);
    }
    return env->NewStringUTF(codec->name.c_str());
}

void MediaCodec_setParameters(JNIEnv *env, jobject thiz, jobjectArray keys, jobjectArray values) {
    AMediaCodec *codec = live(env, thiz);
    if (codec == nullptr) return;
    AMediaFormat *format = format_of(env, keys, values);
    if (format == nullptr) return;
    throw_status(env, AMediaCodec_setParameters(codec, format), "setParameters");
    AMediaFormat_delete(format);
}

jobject MediaCodec_getOwnCodecInfo(JNIEnv *, jobject) {
    return nullptr;  // MediaCodec then asks MediaCodecList.getInfoFor, as the tag's falls back to
}

void MediaCodec_setVideoScalingMode(JNIEnv *env, jobject thiz, jint mode) {
    AMediaCodec *codec = live(env, thiz);
    if (codec == nullptr) return;
    if (mode != 1 && mode != 2) {  // VIDEO_SCALING_MODE_SCALE_TO_FIT, SCALE_TO_FIT_WITH_CROPPING
        jniThrowExceptionFmt(env, "java/lang/IllegalArgumentException", "Unrecognized mode: %d", mode);
        return;
    }
    const Private &functions = private_functions();
    if (functions.setScaling != nullptr) {
        throw_status(env, static_cast<media_status_t>(functions.setScaling(codec, mode)), "setVideoScalingMode");
    }
}

// A frame the core rendered, posted to the Java MediaCodec as EVENT_FRAME_RENDERED with the tag's map, on the
// thread that released it, attached to the VM for it. It reads the Codec after the core let its lock go, as the
// asynchronous callbacks did before e2c3: a release of the MediaCodec on another thread meanwhile is the app's race.
void on_rendered(AMediaCodec *, void *userdata, int64_t media_time_us, int64_t system_nano) {
    auto *codec = static_cast<Codec *>(userdata);
    JNIEnv *env = nullptr;
    bool attached = false;
    if (codec->vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_OK) {
        if (codec->vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return;
        attached = true;
    }
    jobject target = env->NewLocalRef(codec->weak);
    jobject map = target != nullptr ? env->NewObject(gFields.hashMapClass, gFields.hashMapConstructor) : nullptr;
    bool filled = map != nullptr;
    if (map != nullptr) {
        const std::pair<const char *, int64_t> entries[] = {{"0-media-time-us", media_time_us},
                                                            {"0-system-nano", system_nano}};
        for (const auto &[key, value] : entries) {  // each call checked: without memory the frame goes unreported
            jstring name = env->NewStringUTF(key);
            jobject boxed_value = name != nullptr ? env->CallStaticObjectMethod(gFields.longClass, gFields.longValueOf,
                                                                                static_cast<jlong>(value))
                                                  : nullptr;
            jobject old = boxed_value != nullptr && !env->ExceptionCheck()
                              ? env->CallObjectMethod(map, gFields.mapPut, name, boxed_value)
                              : nullptr;
            filled = filled && boxed_value != nullptr && !env->ExceptionCheck();
            if (old != nullptr) env->DeleteLocalRef(old);
            if (boxed_value != nullptr) env->DeleteLocalRef(boxed_value);
            if (name != nullptr) env->DeleteLocalRef(name);
            if (!filled) break;
        }
        if (filled) env->CallVoidMethod(target, gFields.postEvent, kEventFrameRendered, 0, 0, map);
        env->DeleteLocalRef(map);
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (target != nullptr) env->DeleteLocalRef(target);
    if (attached) codec->vm->DetachCurrentThread();
}

void MediaCodec_enableOnFrameRenderedListener(JNIEnv *env, jobject thiz, jboolean enable) {
    AMediaCodec *codec = live(env, thiz);
    if (codec == nullptr) return;
    throw_status(env,
                 AMediaCodec_setOnFrameRenderedCallback(codec, enable ? on_rendered : nullptr,
                                                        enable ? codec_of(env, thiz) : nullptr),
                 "enableOnFrameRenderedListener");
}

void MediaCodec_setSurface(JNIEnv *env, jobject thiz, jobject surface) {
    AMediaCodec *codec = live(env, thiz);
    if (codec == nullptr) return;
    ANativeWindow *window = surface != nullptr ? minato_ANativeWindow_fromSurface(env, surface) : nullptr;
    if (window == nullptr) {
        jniThrowException(env, "java/lang/IllegalArgumentException", "The surface has been released");
        return;
    }
    const media_status_t status = AMediaCodec_setOutputSurface(codec, window);
    ANativeWindow_release(window);
    throw_status(env, status, "setSurface");
}

jobject MediaCodec_getMetrics(JNIEnv *, jobject) {
    return nullptr;  // as the tag's without a metrics item
}

const JNINativeMethod gMediaCodecMethods[] = {
        {"native_init", "()V", (void *)MediaCodec_init},
        {"native_setup", "(Ljava/lang/String;ZZII)V", (void *)MediaCodec_setup},
        {"native_finalize", "()V", (void *)MediaCodec_release},
        {"native_release", "()V", (void *)MediaCodec_release},
        {"native_reset", "()V", (void *)MediaCodec_reset},
        {"native_configure",
         "([Ljava/lang/String;[Ljava/lang/Object;Landroid/view/Surface;Landroid/media/MediaCrypto;"
         "Landroid/os/IHwBinder;I)V",
         (void *)MediaCodec_configure},
        {"native_setCallback", "(Landroid/media/MediaCodec$Callback;)V", (void *)MediaCodec_setCallback},
        {"native_start", "()V", (void *)MediaCodec_start},
        {"native_stop", "()V", (void *)MediaCodec_stop},
        {"native_flush", "()V", (void *)MediaCodec_flush},
        {"native_queueInputBuffer", "(IIIJI)V", (void *)MediaCodec_queueInputBuffer},
        {"native_dequeueInputBuffer", "(J)I", (void *)MediaCodec_dequeueInputBuffer},
        {"native_dequeueOutputBuffer", "(Landroid/media/MediaCodec$BufferInfo;J)I",
         (void *)MediaCodec_dequeueOutputBuffer},
        {"releaseOutputBuffer", "(IZZJ)V", (void *)MediaCodec_releaseOutputBuffer},
        {"signalEndOfInputStream", "()V", (void *)MediaCodec_signalEndOfInputStream},
        {"getFormatNative", "(Z)Ljava/util/Map;", (void *)MediaCodec_getFormatNative},
        {"getOutputFormatNative", "(I)Ljava/util/Map;", (void *)MediaCodec_getOutputFormatNative},
        {"getBuffers", "(Z)[Ljava/nio/ByteBuffer;", (void *)MediaCodec_getBuffers},
        {"getBuffer", "(ZI)Ljava/nio/ByteBuffer;", (void *)MediaCodec_getBuffer},
        {"getImage", "(ZI)Landroid/media/Image;", (void *)MediaCodec_getImage},
        {"getCanonicalName", "()Ljava/lang/String;", (void *)MediaCodec_getCanonicalName},
        {"setParameters", "([Ljava/lang/String;[Ljava/lang/Object;)V", (void *)MediaCodec_setParameters},
        {"getOwnCodecInfo", "()Landroid/media/MediaCodecInfo;", (void *)MediaCodec_getOwnCodecInfo},
        {"setVideoScalingMode", "(I)V", (void *)MediaCodec_setVideoScalingMode},
        {"native_enableOnFrameRenderedListener", "(Z)V", (void *)MediaCodec_enableOnFrameRenderedListener},
        {"native_setSurface", "(Landroid/view/Surface;)V", (void *)MediaCodec_setSurface},
        {"native_getMetrics", "()Landroid/os/PersistableBundle;", (void *)MediaCodec_getMetrics},
};

// MediaCodecList's natives.

const minato_media_codec_info *codec_at(JNIEnv *env, jint index) {
    const Private &functions = private_functions();
    const minato_media_codec_info *info =
            functions.codecAt != nullptr && index >= 0 ? functions.codecAt(static_cast<size_t>(index)) : nullptr;
    if (info == nullptr) jniThrowException(env, "java/lang/IllegalArgumentException", nullptr);
    return info;
}

void MediaCodecList_init(JNIEnv *, jclass) {
}

jint MediaCodecList_getCodecCount(JNIEnv *, jclass) {
    const Private &functions = private_functions();
    return functions.codecs != nullptr ? static_cast<jint>(functions.codecs()) : 0;
}

jstring MediaCodecList_getCodecName(JNIEnv *env, jclass, jint index) {
    const minato_media_codec_info *info = codec_at(env, index);
    return info != nullptr ? env->NewStringUTF(info->name) : nullptr;
}

jint MediaCodecList_getAttributes(JNIEnv *env, jclass, jint index) {
    const minato_media_codec_info *info = codec_at(env, index);
    if (info == nullptr) return 0;
    // MediaCodecInfo's FLAG_IS_ENCODER 1, FLAG_IS_VENDOR 2, FLAG_IS_SOFTWARE_ONLY 4, FLAG_IS_HARDWARE_ACCELERATED 8.
    return (info->encoder ? 1 : 0) | (info->vendor ? 2 : 0) | (!info->vendor && !info->hardware ? 4 : 0) |
           (info->hardware ? 8 : 0);
}

jstring MediaCodecList_getCanonicalName(JNIEnv *env, jclass, jint index) {
    const minato_media_codec_info *info = codec_at(env, index);
    return info != nullptr ? env->NewStringUTF(info->canonical) : nullptr;
}

jobjectArray MediaCodecList_getSupportedTypes(JNIEnv *env, jclass, jint index) {
    const minato_media_codec_info *info = codec_at(env, index);
    if (info == nullptr) return nullptr;
    jobjectArray types = env->NewObjectArray(1, gFields.stringClass, nullptr);
    if (types != nullptr) {
        jstring type = env->NewStringUTF(info->type);
        env->SetObjectArrayElement(types, 0, type);
        env->DeleteLocalRef(type);
    }
    return types;
}

void put_string(JNIEnv *env, jobject map, const char *key, const char *value) {
    jstring k = env->NewStringUTF(key), v = env->NewStringUTF(value);
    env->DeleteLocalRef(env->CallObjectMethod(map, gFields.mapPut, k, v));
    env->DeleteLocalRef(k);
    env->DeleteLocalRef(v);
}

void put_integer(JNIEnv *env, jobject map, const char *key, jint value) {
    jstring k = env->NewStringUTF(key);
    jobject v = boxed(env, gFields.integerClass, gFields.integerValueOf, value);
    env->DeleteLocalRef(env->CallObjectMethod(map, gFields.mapPut, k, v));
    env->DeleteLocalRef(k);
    env->DeleteLocalRef(v);
}

/// An audio decoder's capabilities: AAC's profiles as C2SoftAacDec lists them (LEVEL_UNUSED, 0), none for the
/// others, no color formats, and the details MediaCodecInfo's AudioCapabilities reads, media_codecs_sw.xml's limits.
jobject audio_capabilities(JNIEnv *env, const minato_media_codec_info *info) {
    struct Limits {
        const char *type, *bitrates, *channels, *rates;
    };
    static const Limits limits[] = {
            {"audio/mp4a-latm", "8000-510000", "8", "7350,8000,11025,12000,16000,22050,24000,32000,44100,48000"},
            {"audio/mpeg", "8000-320000", "2", "8000,11025,12000,16000,22050,24000,32000,44100,48000"},
            {"audio/flac", "1-21000000", "8", "1-655350"},
            {"audio/raw", "1-10000000", "12", "8000-192000"},
            {"audio/opus", "6000-510000", "8", "8000,12000,16000,24000,48000"},
            {"audio/vorbis", "32000-500000", "8", "8000-96000"},
    };
    const Limits *limit = &limits[0];
    for (const Limits &each : limits) {
        if (strcmp(each.type, info->type) == 0) limit = &each;
    }
    // CodecProfileLevel's AACObjectLC 2, HE 5, LD 23, ERLC... as C2SoftAacDec lists them: LC, HE, HE_PS 29, LD,
    // ELD 39, ER_SCALABLE 20, XHE 42.
    const jint aac[] = {2, 5, 29, 23, 39, 20, 42};
    const jsize count = limit == &limits[0] ? sizeof aac / sizeof *aac : 0;
    jobjectArray profiles = env->NewObjectArray(count, gFields.profileLevelClass, nullptr);
    for (jsize n = 0; profiles != nullptr && n < count; n++) {
        jobject level = env->NewObject(gFields.profileLevelClass, gFields.profileLevelConstructor);
        env->SetIntField(level, gFields.profileField, aac[n]);
        env->SetIntField(level, gFields.levelField, 0);
        env->SetObjectArrayElement(profiles, n, level);
        env->DeleteLocalRef(level);
    }
    jintArray colors = env->NewIntArray(0);
    jobject defaults = env->NewObject(gFields.hashMapClass, gFields.hashMapConstructor);
    put_string(env, defaults, "mime", info->type);
    jobject details = env->NewObject(gFields.hashMapClass, gFields.hashMapConstructor);
    put_string(env, details, "mime", info->type);
    put_string(env, details, "bitrate-range", limit->bitrates);
    put_string(env, details, "max-channel-count", limit->channels);
    put_string(env, details, "sample-rate-ranges", limit->rates);
    put_string(env, details, "max-concurrent-instances", "32");
    jobject capabilities = env->NewObject(gFields.capabilitiesClass, gFields.capabilitiesConstructor, profiles,
                                          colors, static_cast<jboolean>(info->encoder != 0), defaults, details);
    env->DeleteLocalRef(profiles);
    env->DeleteLocalRef(colors);
    env->DeleteLocalRef(defaults);
    env->DeleteLocalRef(details);
    return capabilities;
}

jobject MediaCodecList_getCodecCapabilities(JNIEnv *env, jclass, jint index, jstring type) {
    const minato_media_codec_info *info = codec_at(env, index);
    if (info == nullptr || type == nullptr) return nullptr;
    if (strncmp(info->type, "audio/", 6) == 0) return audio_capabilities(env, info);
    const bool hevc = strcmp(info->type, "video/hevc") == 0;
    // CodecProfileLevel's values (MediaCodecInfo.java): AVC's Baseline 1, Constrained Baseline 0x10000, Main 2,
    // High 8, Constrained High 0x80000 at AVCLevel52 0x10000; HEVC's Main 1 and Main10 2 at HEVCMainTierLevel51
    // 0x10000.
    const jint avc[][2] = {{0x10000, 0x10000}, {1, 0x10000}, {2, 0x10000}, {0x80000, 0x10000}, {8, 0x10000}};
    const jint hevcLevels[][2] = {{1, 0x10000}, {2, 0x10000}};
    const jint(*levels)[2] = hevc ? hevcLevels : avc;
    const jsize count = hevc ? 2 : 5;
    jobjectArray profiles = env->NewObjectArray(count, gFields.profileLevelClass, nullptr);
    for (jsize n = 0; profiles != nullptr && n < count; n++) {
        jobject level = env->NewObject(gFields.profileLevelClass, gFields.profileLevelConstructor);
        env->SetIntField(level, gFields.profileField, levels[n][0]);
        env->SetIntField(level, gFields.levelField, levels[n][1]);
        env->SetObjectArrayElement(profiles, n, level);
        env->DeleteLocalRef(level);
    }
    // COLOR_FormatYUV420Flexible, COLOR_FormatYUV420SemiPlanar (our buffers' NV12), COLOR_FormatSurface.
    const jint formats[] = {0x7f420888, 21, 0x7f000789};
    jintArray colors = env->NewIntArray(3);
    if (colors != nullptr) env->SetIntArrayRegion(colors, 0, 3, formats);
    jobject defaults = env->NewObject(gFields.hashMapClass, gFields.hashMapConstructor);
    put_string(env, defaults, "mime", info->type);
    jobject details = env->NewObject(gFields.hashMapClass, gFields.hashMapConstructor);
    put_string(env, details, "mime", info->type);
    put_string(env, details, "size-range", "16x16-4096x4096");
    put_string(env, details, "alignment", "2x2");
    put_string(env, details, "block-size", "16x16");
    put_string(env, details, "block-count-range", "1-65536");
    put_string(env, details, "blocks-per-second-range", "1-3932160");
    put_string(env, details, "frame-rate-range", "1-240");
    put_string(env, details, "bitrate-range", "1-240000000");
    put_integer(env, details, "feature-adaptive-playback", 0);
    jobject capabilities = env->NewObject(gFields.capabilitiesClass, gFields.capabilitiesConstructor, profiles,
                                          colors, static_cast<jboolean>(info->encoder != 0), defaults, details);
    env->DeleteLocalRef(profiles);
    env->DeleteLocalRef(colors);
    env->DeleteLocalRef(defaults);
    env->DeleteLocalRef(details);
    return capabilities;
}

jobject MediaCodecList_getGlobalSettings(JNIEnv *env, jclass) {
    return env->NewObject(gFields.hashMapClass, gFields.hashMapConstructor);
}

jint MediaCodecList_findCodecByName(JNIEnv *env, jclass, jstring name) {
    if (name == nullptr) return -1;
    const char *text = env->GetStringUTFChars(name, nullptr);
    const Private &functions = private_functions();
    jint found = -1;
    const size_t count = functions.codecs != nullptr ? functions.codecs() : 0;
    for (size_t index = 0; index < count && found < 0; index++) {
        const minato_media_codec_info *info = functions.codecAt(index);
        if (info != nullptr && strcmp(info->name, text) == 0) found = static_cast<jint>(index);
    }
    env->ReleaseStringUTFChars(name, text);
    return found;
}

const JNINativeMethod gMediaCodecListMethods[] = {
        {"native_init", "()V", (void *)MediaCodecList_init},
        {"native_getCodecCount", "()I", (void *)MediaCodecList_getCodecCount},
        {"getCodecName", "(I)Ljava/lang/String;", (void *)MediaCodecList_getCodecName},
        {"getCanonicalName", "(I)Ljava/lang/String;", (void *)MediaCodecList_getCanonicalName},
        {"getAttributes", "(I)I", (void *)MediaCodecList_getAttributes},
        {"getSupportedTypes", "(I)[Ljava/lang/String;", (void *)MediaCodecList_getSupportedTypes},
        {"getCodecCapabilities", "(ILjava/lang/String;)Landroid/media/MediaCodecInfo$CodecCapabilities;",
         (void *)MediaCodecList_getCodecCapabilities},
        {"native_getGlobalSettings", "()Ljava/util/Map;", (void *)MediaCodecList_getGlobalSettings},
        {"findCodecByName", "(Ljava/lang/String;)I", (void *)MediaCodecList_findCodecByName},
};

jclass global_class(JNIEnv *env, const char *name) {
    return MakeGlobalRefOrDie(env, FindClassOrDie(env, name));
}

}  // namespace

// The format as the tag's Map<String, Object>, for MediaExtractor's natives (android_media_MediaExtractor.cpp).
jobject media_format_to_map(JNIEnv *env, AMediaFormat *format) {
    return map_of(env, format);
}

int register_android_media_MediaCodec(JNIEnv *env) {
    gFields.bufferInfoClass = global_class(env, "android/media/MediaCodec$BufferInfo");
    gFields.bufferInfoConstructor = GetMethodIDOrDie(env, gFields.bufferInfoClass, "<init>", "()V");
    gFields.bufferInfoSet = GetMethodIDOrDie(env, gFields.bufferInfoClass, "set", "(IIJI)V");
    gFields.hashMapClass = global_class(env, "java/util/HashMap");
    gFields.hashMapConstructor = GetMethodIDOrDie(env, gFields.hashMapClass, "<init>", "()V");
    gFields.mapPut = GetMethodIDOrDie(env, gFields.hashMapClass, "put",
                                      "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;");
    gFields.integerClass = global_class(env, "java/lang/Integer");
    gFields.integerValueOf = GetStaticMethodIDOrDie(env, gFields.integerClass, "valueOf", "(I)Ljava/lang/Integer;");
    gFields.integerValue = GetMethodIDOrDie(env, gFields.integerClass, "intValue", "()I");
    gFields.longClass = global_class(env, "java/lang/Long");
    gFields.longValueOf = GetStaticMethodIDOrDie(env, gFields.longClass, "valueOf", "(J)Ljava/lang/Long;");
    gFields.longValue = GetMethodIDOrDie(env, gFields.longClass, "longValue", "()J");
    gFields.floatClass = global_class(env, "java/lang/Float");
    gFields.floatValueOf = GetStaticMethodIDOrDie(env, gFields.floatClass, "valueOf", "(F)Ljava/lang/Float;");
    gFields.floatValue = GetMethodIDOrDie(env, gFields.floatClass, "floatValue", "()F");
    gFields.doubleClass = global_class(env, "java/lang/Double");
    gFields.doubleValueOf = GetStaticMethodIDOrDie(env, gFields.doubleClass, "valueOf", "(D)Ljava/lang/Double;");
    gFields.doubleValue = GetMethodIDOrDie(env, gFields.doubleClass, "doubleValue", "()D");
    gFields.stringClass = global_class(env, "java/lang/String");
    gFields.byteBufferClass = global_class(env, "java/nio/ByteBuffer");
    gFields.byteBufferWrap = GetStaticMethodIDOrDie(env, gFields.byteBufferClass, "wrap", "([B)Ljava/nio/ByteBuffer;");
    gFields.bufferDuplicate = GetMethodIDOrDie(env, gFields.byteBufferClass, "duplicate", "()Ljava/nio/ByteBuffer;");
    gFields.bufferRemaining = GetMethodIDOrDie(env, gFields.byteBufferClass, "remaining", "()I");
    gFields.bufferGet = GetMethodIDOrDie(env, gFields.byteBufferClass, "get", "([B)Ljava/nio/ByteBuffer;");
    gFields.byteOrderClass = global_class(env, "java/nio/ByteOrder");
    gFields.nativeOrder = GetStaticMethodIDOrDie(env, gFields.byteOrderClass, "nativeOrder", "()Ljava/nio/ByteOrder;");
    gFields.bufferOrder =
            GetMethodIDOrDie(env, gFields.byteBufferClass, "order", "(Ljava/nio/ByteOrder;)Ljava/nio/ByteBuffer;");
    gFields.mediaImageClass = global_class(env, "android/media/MediaCodec$MediaImage");
    gFields.mediaImageConstructor =
            GetMethodIDOrDie(env, gFields.mediaImageClass, "<init>",
                             "(Ljava/nio/ByteBuffer;Ljava/nio/ByteBuffer;ZJIILandroid/graphics/Rect;)V");
    gFields.rectClass = global_class(env, "android/graphics/Rect");
    gFields.rectConstructor = GetMethodIDOrDie(env, gFields.rectClass, "<init>", "(IIII)V");
    gFields.capabilitiesClass = global_class(env, "android/media/MediaCodecInfo$CodecCapabilities");
    gFields.capabilitiesConstructor =
            GetMethodIDOrDie(env, gFields.capabilitiesClass, "<init>",
                             "([Landroid/media/MediaCodecInfo$CodecProfileLevel;[IZLjava/util/Map;Ljava/util/Map;)V");
    gFields.profileLevelClass = global_class(env, "android/media/MediaCodecInfo$CodecProfileLevel");
    gFields.profileLevelConstructor = GetMethodIDOrDie(env, gFields.profileLevelClass, "<init>", "()V");
    gFields.profileField = GetFieldIDOrDie(env, gFields.profileLevelClass, "profile", "I");
    gFields.levelField = GetFieldIDOrDie(env, gFields.profileLevelClass, "level", "I");
    RegisterMethodsOrDie(env, "android/media/MediaCodecList", gMediaCodecListMethods, NELEM(gMediaCodecListMethods));
    return RegisterMethodsOrDie(env, "android/media/MediaCodec", gMediaCodecMethods, NELEM(gMediaCodecMethods));
}

}  // namespace android
