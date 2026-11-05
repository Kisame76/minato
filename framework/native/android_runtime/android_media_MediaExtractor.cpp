// Ours in place of AOSP's media/jni/android_media_MediaExtractor.cpp and android_media_MediaDataSource.cpp (ADR-0060
// e2d part 2b): MediaExtractor's natives on libmediandk's AMediaExtractor (runtime/android/media_extractor.cpp), which
// answers as NuMediaExtractor, following the tag's JNI:
// - A source that does not open is an IOException "Failed to instantiate extractor.", a second source too; a null path,
//   file descriptor or MediaDataSource is an IllegalArgumentException.
// - A path is DataSourceFactory's: "file://" and any other path but a URL name a file, opened as FileSource opens it;
//   http and https fail without an http service, and so do data: URIs (named limits, ADR-0060 e2d).
// - A MediaDataSource is JMediaDataSource's: reads of at most 64 KiB through one byte array, -1 the end, an exception,
//   another negative answer or more bytes than asked an error from then on; getSize below 0 unknown. Its close is
//   called when the extractor goes, and twice at once when the source does not open (Android 15's, observed: the
//   extractor service's side and the app's bridge each close it).
// - A track format crosses as MediaCodec's (the tag's ConvertMessageToMap); a track past the count, a selection the
//   extractor refuses and a buffer too small for the sample are IllegalArgumentExceptions, a seek mode outside the
//   three sync modes too; the sample getters, advance and readSampleData answer -1 or false at the end, which is where
//   NuMediaExtractor's every failure of a track leads.
// - Nothing is cached (getCachedDuration -1, hasCacheReachedEndOfStream true), no sample is encrypted, no track has
//   audio presentations or a CAS (setMediaCas is an IllegalArgumentException).
// - getMetrics is the extractor's metrics item as a PersistableBundle: _key "extractor", the extractor's name
//   (MPEG4Extractor, OggExtractor for Ogg), the file's mime, the track count and the entry point "sdk", with the app's
//   uid, as Android 15's [observed: media-extractor-test, media-extractor-ogg-test]; null before a source opens.
// - Every call on a released extractor is an IllegalStateException.
#define LOG_TAG "MediaExtractor_JNI"

#include <errno.h>
#include <fcntl.h>
#include <jni.h>
#include <media/NdkMediaDataSource.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaFormat.h>
#include <nativehelper/JNIHelp.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <mutex>

#include <log/log.h>

#include "core_jni_helpers.h"

namespace android {

// android_media_MediaCodec.cpp's: a format as the tag's Map<String, Object>.
jobject media_format_to_map(JNIEnv *env, AMediaFormat *format);

namespace {

constexpr jint kBufferSize = 64 * 1024;  // JMediaDataSource's kBufferSize

struct {
    jfieldID context;  // MediaExtractor.mNativeContext
    jclass arrayListClass;  // global
    jmethodID arrayListConstructor;
    jmethodID bufferArray, bufferPosition, bufferLimit;
    jclass bundleClass;  // global, PersistableBundle
    jmethodID bundleConstructor, putString, putInt;
} gFields;

// A Java MediaDataSource as an AMediaDataSource's userdata.
struct JavaSource {
    JavaVM *vm = nullptr;
    jobject source = nullptr;  // global
    jbyteArray bytes = nullptr;  // global, kBufferSize
    jmethodID readAt = nullptr, getSize = nullptr, close = nullptr;
    std::mutex lock;
    bool failed = false;  // an exception, a bad answer or close: every read fails from then on
    bool sized = false;
    int64_t size = 0;
};

// A Java MediaExtractor's native side.
struct Extractor {
    AMediaExtractor *extractor = nullptr;
    JavaSource *source = nullptr;
};

JNIEnv *env_of(JavaVM *vm) {
    JNIEnv *env = nullptr;
    vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
    return env;
}

ssize_t java_read(void *userdata, off64_t offset, void *buffer, size_t size) {
    auto *source = static_cast<JavaSource *>(userdata);
    std::lock_guard<std::mutex> guard(source->lock);
    if (source->failed) return -1;
    if (size > static_cast<size_t>(kBufferSize)) size = kBufferSize;
    JNIEnv *env = env_of(source->vm);
    const jint read = env->CallIntMethod(source->source, source->readAt, static_cast<jlong>(offset), source->bytes,
                                         0, static_cast<jint>(size));
    if (env->ExceptionCheck()) {
        ALOGW("An exception occurred in readAt()");
        env->ExceptionClear();
        source->failed = true;
        return -1;
    }
    if (read == -1) return 0;  // the end
    if (read < 0 || static_cast<size_t>(read) > size) {
        source->failed = true;
        return -1;
    }
    env->GetByteArrayRegion(source->bytes, 0, read, static_cast<jbyte *>(buffer));
    return read;
}

ssize_t java_size(void *userdata) {
    auto *source = static_cast<JavaSource *>(userdata);
    std::lock_guard<std::mutex> guard(source->lock);
    if (source->failed) return -1;
    if (!source->sized) {
        JNIEnv *env = env_of(source->vm);
        const jlong size = env->CallLongMethod(source->source, source->getSize);
        if (env->ExceptionCheck()) {
            ALOGW("An exception occurred in getSize()");
            env->ExceptionClear();
            source->failed = true;
            return -1;
        }
        source->size = size < 0 ? -1 : size;
        source->sized = true;
    }
    return static_cast<ssize_t>(source->size);
}

void java_close(void *userdata) {
    auto *source = static_cast<JavaSource *>(userdata);
    std::lock_guard<std::mutex> guard(source->lock);
    JNIEnv *env = env_of(source->vm);
    env->CallVoidMethod(source->source, source->close);
    source->failed = true;  // closed is as failed
}

void free_source(JNIEnv *env, JavaSource *source) {
    if (source == nullptr) return;
    env->DeleteGlobalRef(source->source);
    env->DeleteGlobalRef(source->bytes);
    delete source;
}

Extractor *extractor_of(JNIEnv *env, jobject thiz) {
    return reinterpret_cast<Extractor *>(env->GetLongField(thiz, gFields.context));
}

/// The extractor, or NULL with an IllegalStateException for a released one.
AMediaExtractor *live(JNIEnv *env, jobject thiz) {
    Extractor *extractor = extractor_of(env, thiz);
    if (extractor == nullptr) {
        jniThrowException(env, "java/lang/IllegalStateException", nullptr);
        return nullptr;
    }
    return extractor->extractor;
}

void throw_failed(JNIEnv *env) {
    jniThrowException(env, "java/io/IOException", "Failed to instantiate extractor.");
}

void MediaExtractor_init(JNIEnv *env, jclass clazz) {
    gFields.context = GetFieldIDOrDie(env, clazz, "mNativeContext", "J");
}

void MediaExtractor_setup(JNIEnv *env, jobject thiz) {
    auto *extractor = new Extractor;
    extractor->extractor = AMediaExtractor_new();
    env->SetLongField(thiz, gFields.context, reinterpret_cast<jlong>(extractor));
}

void MediaExtractor_release(JNIEnv *env, jobject thiz) {
    Extractor *extractor = extractor_of(env, thiz);
    env->SetLongField(thiz, gFields.context, 0);
    if (extractor == nullptr) return;
    AMediaExtractor_delete(extractor->extractor);  // closes a source it took
    free_source(env, extractor->source);
    delete extractor;
}

void MediaExtractor_setDataSource(JNIEnv *env, jobject thiz, jobject, jstring path_object, jobjectArray,
                                  jobjectArray) {
    AMediaExtractor *extractor = live(env, thiz);
    if (extractor == nullptr) return;
    if (path_object == nullptr) {
        jniThrowException(env, "java/lang/IllegalArgumentException", nullptr);
        return;
    }
    const char *path = env->GetStringUTFChars(path_object, nullptr);
    if (path == nullptr) return;
    // DataSourceFactory::CreateFromURI: without an http service, only a file opens.
    const char *file = path;
    if (strncasecmp(path, "file://", 7) == 0) {
        file = path + 7;
    } else if (strncasecmp(path, "http://", 7) == 0 || strncasecmp(path, "https://", 8) == 0 ||
               strncasecmp(path, "data:", 5) == 0) {
        file = nullptr;
    }
    media_status_t status = AMEDIA_ERROR_UNKNOWN;
    const int fd = file == nullptr ? -1 : open(file, O_RDONLY | O_LARGEFILE | O_CLOEXEC);
    if (fd >= 0) {
        const off64_t length = lseek64(fd, 0, SEEK_END);
        if (length >= 0) status = AMediaExtractor_setDataSourceFd(extractor, fd, 0, length);
        close(fd);  // the extractor keeps its own
    }
    env->ReleaseStringUTFChars(path_object, path);
    if (status != AMEDIA_OK) throw_failed(env);
}

void MediaExtractor_setDataSourceFd(JNIEnv *env, jobject thiz, jobject descriptor, jlong offset, jlong length) {
    AMediaExtractor *extractor = live(env, thiz);
    if (extractor == nullptr) return;
    if (descriptor == nullptr) {
        jniThrowException(env, "java/lang/IllegalArgumentException", nullptr);
        return;
    }
    const int fd = jniGetFDFromFileDescriptor(env, descriptor);
    if (AMediaExtractor_setDataSourceFd(extractor, fd, offset, length) != AMEDIA_OK) throw_failed(env);
}

void MediaExtractor_setDataSourceCallback(JNIEnv *env, jobject thiz, jobject callback) {
    Extractor *extractor = extractor_of(env, thiz);
    if (extractor == nullptr) {
        jniThrowException(env, "java/lang/IllegalStateException", nullptr);
        return;
    }
    if (callback == nullptr) {
        jniThrowException(env, "java/lang/IllegalArgumentException", nullptr);
        return;
    }
    auto *source = new JavaSource;
    env->GetJavaVM(&source->vm);
    source->source = env->NewGlobalRef(callback);
    jclass type = env->GetObjectClass(callback);
    source->readAt = env->GetMethodID(type, "readAt", "(J[BII)I");
    source->getSize = env->GetMethodID(type, "getSize", "()J");
    source->close = env->GetMethodID(type, "close", "()V");
    env->DeleteLocalRef(type);
    jbyteArray bytes = env->NewByteArray(kBufferSize);
    source->bytes = static_cast<jbyteArray>(env->NewGlobalRef(bytes));
    env->DeleteLocalRef(bytes);
    AMediaDataSource *data_source = AMediaDataSource_new();
    AMediaDataSource_setUserdata(data_source, source);
    AMediaDataSource_setReadAt(data_source, java_read);
    AMediaDataSource_setGetSize(data_source, java_size);
    AMediaDataSource_setClose(data_source, java_close);
    const media_status_t status = AMediaExtractor_setDataSourceCustom(extractor->extractor, data_source);
    AMediaDataSource_delete(data_source);  // the extractor keeps its functions
    if (status != AMEDIA_OK) {
        // The tag's bridge goes before the exception: the service's side and the app's each close the source.
        java_close(source);
        java_close(source);
        free_source(env, source);
        throw_failed(env);
        return;
    }
    free_source(env, extractor->source);
    extractor->source = source;
}

void MediaExtractor_setMediaCas(JNIEnv *env, jobject thiz, jobject) {
    if (live(env, thiz) == nullptr) return;
    jniThrowException(env, "java/lang/IllegalArgumentException", "Failed to set MediaCas on extractor.");
}

jint MediaExtractor_getTrackCount(JNIEnv *env, jobject thiz) {
    AMediaExtractor *extractor = live(env, thiz);
    return extractor == nullptr ? -1 : static_cast<jint>(AMediaExtractor_getTrackCount(extractor));
}

jobject format_map(JNIEnv *env, AMediaFormat *format) {
    jobject map = media_format_to_map(env, format);
    AMediaFormat_delete(format);
    return map;
}

jobject MediaExtractor_getTrackFormatNative(JNIEnv *env, jobject thiz, jint index) {
    AMediaExtractor *extractor = live(env, thiz);
    if (extractor == nullptr) return nullptr;
    if (index < 0 || static_cast<size_t>(index) >= AMediaExtractor_getTrackCount(extractor)) {
        jniThrowException(env, "java/lang/IllegalArgumentException", nullptr);  // -ERANGE
        return nullptr;
    }
    // A format the conversion refuses has no MIME type (libmediandk's track of a broken avcC, hvcC or esds):
    // getTrackFormat's error, IllegalArgumentException.
    AMediaFormat *format = AMediaExtractor_getTrackFormat(extractor, static_cast<size_t>(index));
    const char *mime = nullptr;
    if (format != nullptr && !AMediaFormat_getString(format, "mime", &mime)) {
        AMediaFormat_delete(format);
        jniThrowException(env, "java/lang/IllegalArgumentException", nullptr);
        return nullptr;
    }
    return format_map(env, format);
}

jobject MediaExtractor_getFileFormatNative(JNIEnv *env, jobject thiz) {
    AMediaExtractor *extractor = live(env, thiz);
    return extractor == nullptr ? nullptr : format_map(env, AMediaExtractor_getFileFormat(extractor));
}

void MediaExtractor_selectTrack(JNIEnv *env, jobject thiz, jint index) {
    AMediaExtractor *extractor = live(env, thiz);
    if (extractor == nullptr) return;
    if (index < 0 || AMediaExtractor_selectTrack(extractor, static_cast<size_t>(index)) != AMEDIA_OK) {
        jniThrowException(env, "java/lang/IllegalArgumentException", nullptr);
    }
}

void MediaExtractor_unselectTrack(JNIEnv *env, jobject thiz, jint index) {
    AMediaExtractor *extractor = live(env, thiz);
    if (extractor == nullptr) return;
    if (index < 0 || AMediaExtractor_unselectTrack(extractor, static_cast<size_t>(index)) != AMEDIA_OK) {
        jniThrowException(env, "java/lang/IllegalArgumentException", nullptr);
    }
}

void MediaExtractor_seekTo(JNIEnv *env, jobject thiz, jlong time_us, jint mode) {
    AMediaExtractor *extractor = live(env, thiz);
    if (extractor == nullptr) return;
    // MediaSource::ReadOptions' SEEK_PREVIOUS_SYNC to SEEK_CLOSEST_SYNC, AMediaExtractor's numbers too.
    if (mode < AMEDIAEXTRACTOR_SEEK_PREVIOUS_SYNC || mode > AMEDIAEXTRACTOR_SEEK_CLOSEST_SYNC) {
        jniThrowException(env, "java/lang/IllegalArgumentException", nullptr);
        return;
    }
    AMediaExtractor_seekTo(extractor, time_us, static_cast<SeekMode>(mode));
}

jboolean MediaExtractor_advance(JNIEnv *env, jobject thiz) {
    AMediaExtractor *extractor = live(env, thiz);
    return extractor != nullptr && AMediaExtractor_advance(extractor) ? JNI_TRUE : JNI_FALSE;
}

jint MediaExtractor_readSampleData(JNIEnv *env, jobject thiz, jobject buffer, jint offset) {
    AMediaExtractor *extractor = live(env, thiz);
    if (extractor == nullptr) return -1;
    // The tag's: a direct buffer's memory, else the whole backing array; the sample at `offset`.
    auto *data = static_cast<uint8_t *>(env->GetDirectBufferAddress(buffer));
    jbyteArray array = nullptr;
    size_t capacity = 0;
    if (data != nullptr) {
        capacity = static_cast<size_t>(env->GetDirectBufferCapacity(buffer));
    } else {
        array = static_cast<jbyteArray>(env->CallObjectMethod(buffer, gFields.bufferArray));
        if (env->ExceptionCheck() || array == nullptr) {
            env->ExceptionClear();
            jniThrowException(env, "java/lang/IllegalArgumentException", nullptr);  // INVALID_OPERATION
            return -1;
        }
        data = reinterpret_cast<uint8_t *>(env->GetByteArrayElements(array, nullptr));
        capacity = static_cast<size_t>(env->GetArrayLength(array));
    }
    jint answer = -1;
    bool refused = false;
    const ssize_t size = data == nullptr ? -1 : AMediaExtractor_getSampleSize(extractor);
    if (data == nullptr || offset < 0 || capacity < static_cast<size_t>(offset)) {
        refused = true;  // -ENOMEM, -ERANGE
    } else if (size >= 0) {
        if (static_cast<size_t>(size) > capacity - static_cast<size_t>(offset)) {
            refused = true;  // -ENOMEM: the buffer is too small for the sample
        } else {
            answer = static_cast<jint>(AMediaExtractor_readSampleData(extractor, data + offset, capacity - offset));
        }
    }
    if (array != nullptr && data != nullptr) env->ReleaseByteArrayElements(array, reinterpret_cast<jbyte *>(data), 0);
    if (array != nullptr) env->DeleteLocalRef(array);
    if (refused) {
        jniThrowException(env, "java/lang/IllegalArgumentException", nullptr);
        return -1;
    }
    if (answer < 0) return -1;  // the end
    env->DeleteLocalRef(env->CallObjectMethod(buffer, gFields.bufferLimit, offset + answer));
    env->DeleteLocalRef(env->CallObjectMethod(buffer, gFields.bufferPosition, offset));
    return answer;
}

jint MediaExtractor_getSampleTrackIndex(JNIEnv *env, jobject thiz) {
    AMediaExtractor *extractor = live(env, thiz);
    return extractor == nullptr ? -1 : AMediaExtractor_getSampleTrackIndex(extractor);
}

jlong MediaExtractor_getSampleTime(JNIEnv *env, jobject thiz) {
    AMediaExtractor *extractor = live(env, thiz);
    return extractor == nullptr ? -1 : AMediaExtractor_getSampleTime(extractor);
}

jlong MediaExtractor_getSampleSize(JNIEnv *env, jobject thiz) {
    AMediaExtractor *extractor = live(env, thiz);
    return extractor == nullptr ? -1 : AMediaExtractor_getSampleSize(extractor);
}

jint MediaExtractor_getSampleFlags(JNIEnv *env, jobject thiz) {
    AMediaExtractor *extractor = live(env, thiz);
    return extractor == nullptr ? -1 : static_cast<jint>(AMediaExtractor_getSampleFlags(extractor));
}

jboolean MediaExtractor_getSampleCryptoInfo(JNIEnv *env, jobject thiz, jobject) {
    live(env, thiz);
    return JNI_FALSE;  // no sample is encrypted
}

jlong MediaExtractor_getCachedDuration(JNIEnv *env, jobject thiz) {
    live(env, thiz);
    return -1;  // NuMediaExtractor's for a source that does not cache
}

jboolean MediaExtractor_hasCacheReachedEndOfStream(JNIEnv *env, jobject thiz) {
    live(env, thiz);
    return JNI_TRUE;
}

void put_string(JNIEnv *env, jobject bundle, const char *key, const char *value) {
    jstring name = env->NewStringUTF(key);
    jstring text = env->NewStringUTF(value);
    env->CallVoidMethod(bundle, gFields.putString, name, text);
    env->DeleteLocalRef(name);
    env->DeleteLocalRef(text);
}

void put_int(JNIEnv *env, jobject bundle, const char *key, jint value) {
    jstring name = env->NewStringUTF(key);
    env->CallVoidMethod(bundle, gFields.putInt, name, value);
    env->DeleteLocalRef(name);
}

jobject MediaExtractor_getMetrics(JNIEnv *env, jobject thiz) {
    AMediaExtractor *extractor = live(env, thiz);
    if (extractor == nullptr) return nullptr;
    // The file's format has a mime once a source opened (MPEG4Extractor's metrics item exists from then on).
    AMediaFormat *file = AMediaExtractor_getFileFormat(extractor);
    const char *mime = nullptr;
    const bool opened = file != nullptr && AMediaFormat_getString(file, AMEDIAFORMAT_KEY_MIME, &mime);
    jobject bundle = nullptr;
    if (opened) {
        bundle = env->NewObject(gFields.bundleClass, gFields.bundleConstructor);
        put_string(env, bundle, "_key", "extractor");
        put_int(env, bundle, "_uid", static_cast<jint>(getuid()));
        put_string(env, bundle, "android.media.mediaextractor.entry", "sdk");
        // The extractor's name: OggExtractor for an Ogg file, else MPEG4Extractor.
        const bool ogg = strcmp(mime, "audio/ogg") == 0;
        put_string(env, bundle, "android.media.mediaextractor.fmt", ogg ? "OggExtractor" : "MPEG4Extractor");
        put_string(env, bundle, "android.media.mediaextractor.mime", mime);
        put_int(env, bundle, "android.media.mediaextractor.ntrk",
                static_cast<jint>(AMediaExtractor_getTrackCount(extractor)));
    }
    AMediaFormat_delete(file);
    return bundle;
}

void MediaExtractor_setLogSessionId(JNIEnv *env, jobject thiz, jstring) {
    live(env, thiz);
}

jobject MediaExtractor_getAudioPresentations(JNIEnv *env, jobject thiz, jint) {
    jobject list = env->NewObject(gFields.arrayListClass, gFields.arrayListConstructor);
    live(env, thiz);
    return list;  // MPEG4Extractor has none (ERROR_UNSUPPORTED)
}

const JNINativeMethod gMethods[] = {
        {"release", "()V", (void *)MediaExtractor_release},
        {"getTrackCount", "()I", (void *)MediaExtractor_getTrackCount},
        {"getFileFormatNative", "()Ljava/util/Map;", (void *)MediaExtractor_getFileFormatNative},
        {"getTrackFormatNative", "(I)Ljava/util/Map;", (void *)MediaExtractor_getTrackFormatNative},
        {"selectTrack", "(I)V", (void *)MediaExtractor_selectTrack},
        {"unselectTrack", "(I)V", (void *)MediaExtractor_unselectTrack},
        {"seekTo", "(JI)V", (void *)MediaExtractor_seekTo},
        {"advance", "()Z", (void *)MediaExtractor_advance},
        {"readSampleData", "(Ljava/nio/ByteBuffer;I)I", (void *)MediaExtractor_readSampleData},
        {"getSampleTrackIndex", "()I", (void *)MediaExtractor_getSampleTrackIndex},
        {"getSampleTime", "()J", (void *)MediaExtractor_getSampleTime},
        {"getSampleSize", "()J", (void *)MediaExtractor_getSampleSize},
        {"getSampleFlags", "()I", (void *)MediaExtractor_getSampleFlags},
        {"getSampleCryptoInfo", "(Landroid/media/MediaCodec$CryptoInfo;)Z", (void *)MediaExtractor_getSampleCryptoInfo},
        {"native_init", "()V", (void *)MediaExtractor_init},
        {"native_setup", "()V", (void *)MediaExtractor_setup},
        {"native_finalize", "()V", (void *)MediaExtractor_release},
        {"nativeSetDataSource", "(Landroid/os/IBinder;Ljava/lang/String;[Ljava/lang/String;[Ljava/lang/String;)V",
         (void *)MediaExtractor_setDataSource},
        {"setDataSource", "(Ljava/io/FileDescriptor;JJ)V", (void *)MediaExtractor_setDataSourceFd},
        {"setDataSource", "(Landroid/media/MediaDataSource;)V", (void *)MediaExtractor_setDataSourceCallback},
        {"nativeSetMediaCas", "(Landroid/os/IHwBinder;)V", (void *)MediaExtractor_setMediaCas},
        {"getCachedDuration", "()J", (void *)MediaExtractor_getCachedDuration},
        {"hasCacheReachedEndOfStream", "()Z", (void *)MediaExtractor_hasCacheReachedEndOfStream},
        {"native_getMetrics", "()Landroid/os/PersistableBundle;", (void *)MediaExtractor_getMetrics},
        {"native_setLogSessionId", "(Ljava/lang/String;)V", (void *)MediaExtractor_setLogSessionId},
        {"native_getAudioPresentations", "(I)Ljava/util/List;", (void *)MediaExtractor_getAudioPresentations},
};

}  // namespace

int register_android_media_MediaExtractor(JNIEnv *env) {
    gFields.arrayListClass = MakeGlobalRefOrDie(env, FindClassOrDie(env, "java/util/ArrayList"));
    gFields.arrayListConstructor = GetMethodIDOrDie(env, gFields.arrayListClass, "<init>", "()V");
    jclass buffer = FindClassOrDie(env, "java/nio/ByteBuffer");
    gFields.bufferArray = GetMethodIDOrDie(env, buffer, "array", "()[B");
    gFields.bufferPosition = GetMethodIDOrDie(env, buffer, "position", "(I)Ljava/nio/Buffer;");
    gFields.bufferLimit = GetMethodIDOrDie(env, buffer, "limit", "(I)Ljava/nio/Buffer;");
    gFields.bundleClass = MakeGlobalRefOrDie(env, FindClassOrDie(env, "android/os/PersistableBundle"));
    gFields.bundleConstructor = GetMethodIDOrDie(env, gFields.bundleClass, "<init>", "()V");
    gFields.putString =
            GetMethodIDOrDie(env, gFields.bundleClass, "putString", "(Ljava/lang/String;Ljava/lang/String;)V");
    gFields.putInt = GetMethodIDOrDie(env, gFields.bundleClass, "putInt", "(Ljava/lang/String;I)V");
    return RegisterMethodsOrDie(env, "android/media/MediaExtractor", gMethods, NELEM(gMethods));
}

}  // namespace android
