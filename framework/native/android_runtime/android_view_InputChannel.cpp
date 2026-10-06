// Ours in place of AOSP's core/jni/android_view_InputChannel.cpp (ADR-0045 step 7b2): InputChannel's natives on the
// session's input channel (minato_input_channel.h), where AOSP's stand on libinput's InputChannel. A window's channel
// comes from the session, not from the window manager's addToDisplay: at APKRT_EVENT_INPUT_QUEUE_CREATED,
// ActivityThread's native connects the channel (input_connect) and makes an InputChannel of its descriptor with
// android_view_InputChannel_createJavaObject, as AOSP's of the same name, for the window's ViewRootImpl. Ours is a
// channel's receiving end only, so what only system_server and Binder use answers as follows:
//   - nativeOpenInputChannelPair, nativeReadFromParcel and nativeWriteToParcel throw UnsupportedOperationException:
//     no pair (no dispatcher writes one) and no parcel across processes;
//   - nativeGetToken answers null, as for a channel no dispatcher registered;
//   - nativeDup gives a new channel on the same descriptor, which stays open while either holds it;
//   - nativeDispose drops the channel's hold on its descriptor, which closes with its last owner, as AOSP's
//     NativeInputChannel::dispose resets its shared InputChannel; the finalizer frees the rest.
#define LOG_TAG "InputChannel-JNI"

#include <jni.h>
#include <log/log.h>
#include <nativehelper/JNIHelp.h>
#include <stdint.h>
#include <unistd.h>

#include "core_jni_helpers.h"
#include "minato_input_channel.h"

namespace android {

InputDescriptor::~InputDescriptor() {
    close(mFd);
}

namespace {

struct {
    jclass clazz;
    jfieldID mPtr;
    jmethodID ctor;
    jmethodID setNativeInputChannel;
} gInputChannelClassInfo;

NativeInputChannel *Channel(jlong ptr) {
    return reinterpret_cast<NativeInputChannel *>(ptr);
}

void Destroy(void *channel) {
    delete static_cast<NativeInputChannel *>(channel);
}

void ThrowUnsupported(JNIEnv *env, const char *what) {
    jniThrowExceptionFmt(env, "java/lang/UnsupportedOperationException",
                         "%s: Minato's input channels are the session's, without pairs or parcels (ADR-0045 step 7b2)",
                         what);
}

jlongArray OpenInputChannelPair(JNIEnv *env, jclass, jstring) {
    ThrowUnsupported(env, "openInputChannelPair");
    return nullptr;
}

jlong GetFinalizer(JNIEnv *, jclass) {
    return static_cast<jlong>(reinterpret_cast<uintptr_t>(&Destroy));
}

void Dispose(JNIEnv *, jobject, jlong ptr) {
    if (NativeInputChannel *channel = Channel(ptr)) {
        channel->descriptor.reset();
    }
}

jlong ReadFromParcel(JNIEnv *env, jobject, jobject) {
    ThrowUnsupported(env, "readFromParcel");
    return 0;
}

void WriteToParcel(JNIEnv *env, jobject, jobject, jlong) {
    ThrowUnsupported(env, "writeToParcel");
}

jlong Dup(JNIEnv *env, jobject, jlong ptr) {
    NativeInputChannel *channel = Channel(ptr);
    if (channel == nullptr) {
        jniThrowRuntimeException(env, "InputChannel has no valid NativeInputChannel");
        return 0;
    }
    if (channel->descriptor == nullptr) {
        jniThrowRuntimeException(env, "NativeInputChannel has no corresponding InputChannel");
        return 0;
    }
    return reinterpret_cast<jlong>(new NativeInputChannel{channel->descriptor, channel->name});
}

jobject GetToken(JNIEnv *, jobject, jlong) {
    return nullptr;
}

jstring GetName(JNIEnv *env, jobject, jlong ptr) {
    NativeInputChannel *channel = Channel(ptr);
    if (channel == nullptr || channel->descriptor == nullptr) return nullptr;
    return env->NewStringUTF(channel->name.c_str());
}

const JNINativeMethod gInputChannelMethods[] = {
        {"nativeOpenInputChannelPair", "(Ljava/lang/String;)[J", reinterpret_cast<void *>(OpenInputChannelPair)},
        {"nativeGetFinalizer", "()J", reinterpret_cast<void *>(GetFinalizer)},
        {"nativeDispose", "(J)V", reinterpret_cast<void *>(Dispose)},
        {"nativeReadFromParcel", "(Landroid/os/Parcel;)J", reinterpret_cast<void *>(ReadFromParcel)},
        {"nativeWriteToParcel", "(Landroid/os/Parcel;J)V", reinterpret_cast<void *>(WriteToParcel)},
        {"nativeDup", "(J)J", reinterpret_cast<void *>(Dup)},
        {"nativeGetToken", "(J)Landroid/os/IBinder;", reinterpret_cast<void *>(GetToken)},
        {"nativeGetName", "(J)Ljava/lang/String;", reinterpret_cast<void *>(GetName)},
};

}  // namespace

std::shared_ptr<InputDescriptor> android_view_InputChannel_getDescriptor(JNIEnv *env, jobject inputChannelObj) {
    NativeInputChannel *channel = Channel(env->GetLongField(inputChannelObj, gInputChannelClassInfo.mPtr));
    return channel != nullptr ? channel->descriptor : nullptr;
}

std::string android_view_InputChannel_getName(JNIEnv *env, jobject inputChannelObj) {
    NativeInputChannel *channel = Channel(env->GetLongField(inputChannelObj, gInputChannelClassInfo.mPtr));
    return channel != nullptr ? channel->name : "uninitialized";
}

jobject android_view_InputChannel_createJavaObject(JNIEnv *env, int fd, const std::string &name) {
    auto *channel = new NativeInputChannel{std::make_shared<InputDescriptor>(fd), name};
    jobject object = env->NewObject(gInputChannelClassInfo.clazz, gInputChannelClassInfo.ctor);
    if (object == nullptr) {
        ALOGE("Failed to create a Java InputChannel for channel %s.", name.c_str());
        delete channel;
        return nullptr;
    }
    env->CallVoidMethod(object, gInputChannelClassInfo.setNativeInputChannel, reinterpret_cast<jlong>(channel));
    if (env->ExceptionCheck()) {
        ALOGE("Failed to set native ptr to the Java InputChannel for channel %s.", name.c_str());
        delete channel;
        return nullptr;
    }
    return object;
}

// The natives come first, as AOSP's: looking up a method initializes InputChannel, whose static initializer calls
// nativeGetFinalizer.
int register_android_view_InputChannel(JNIEnv *env) {
    const int result =
            RegisterMethodsOrDie(env, "android/view/InputChannel", gInputChannelMethods, NELEM(gInputChannelMethods));
    jclass clazz = FindClassOrDie(env, "android/view/InputChannel");
    gInputChannelClassInfo.clazz = MakeGlobalRefOrDie(env, clazz);
    gInputChannelClassInfo.mPtr = GetFieldIDOrDie(env, clazz, "mPtr", "J");
    gInputChannelClassInfo.ctor = GetMethodIDOrDie(env, clazz, "<init>", "()V");
    gInputChannelClassInfo.setNativeInputChannel = GetMethodIDOrDie(env, clazz, "setNativeInputChannel", "(J)V");
    return result;
}

}  // namespace android
