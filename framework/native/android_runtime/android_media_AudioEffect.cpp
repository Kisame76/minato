// Ours (the audio effects plan): the natives of AOSP's android.media.audiofx.AudioEffect, which on Android stand on
// libaudioclient's AudioEffect and the audio server's effect engine. They are the local effect service
// (audio_effect_service.h): the list of effects Android 15's emulator lists, instances per session with their
// parameters and enabled state, and control passing between them, told to the Java side's control listener as
// postEventFromNative(NATIVE_EVENT_CONTROL_STATUS) does. No sample passes through them here: what the Equalizer,
// Dynamic Bass Boost, Virtualizer and Loudness Enhancer of a session ask for goes to the runtime's mix (audio_effect),
// which plays the output streams of that session through them.
#include <jni.h>
#include <nativehelper/JNIHelp.h>
#include <stdint.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "APKRuntime/guest_interface.h"
#include "audio_effect_service.h"
#include "core_jni_helpers.h"

namespace android {
namespace {

using minato::audiofx::Entry;
using minato::audiofx::Service;

constexpr int NATIVE_EVENT_CONTROL_STATUS = 0;

std::mutex gLock;
Service gService;
std::map<int, jobject> gObjects;  // an instance's java.lang.ref.WeakReference to its AudioEffect (a global ref)

struct Fields {
    jfieldID nativeInstance;  // mNativeAudioEffect: the instance's id
    jmethodID postEvent;      // AudioEffect.postEventFromNative
    jclass effectClass;
    jclass descriptorClass;
    jclass uuidClass;
    jmethodID uuidFromString;
    jmethodID descriptorInit;
    jfieldID type, uuid, connectMode, name, implementor;
} gFields;

thread_local JNIEnv *gEnv = nullptr;  // the calling thread's, for the control listener that runs inside a call

/// The Java side's control status: instance `id` gained or lost control.
void controlChanged(void *, int id, bool hasControl) {
    JNIEnv *env = gEnv;
    if (env == nullptr) return;
    auto found = gObjects.find(id);
    if (found == gObjects.end()) return;
    env->CallStaticVoidMethod(gFields.effectClass, gFields.postEvent, found->second, NATIVE_EVENT_CONTROL_STATUS,
                              hasControl ? 1 : 0, 0, static_cast<jobject>(nullptr));
    if (env->ExceptionCheck()) env->ExceptionClear();
}

/// Tells the runtime's mix what `session`'s effects are now: the five sounding kinds, each on or off with its
/// parameters. gLock is held. A runtime without the call (an old one) answers -ENOSYS and nothing sounds.
void publish(int session) {
    const minato::audiofx::Applied applied = gService.applied(session);
    apkrt_audio_effect effect{};
    effect.session = session;
    effect.kind = APKRT_AUDIO_EFFECT_EQUALIZER;
    effect.enabled = applied.equalizer ? 1 : 0;
    for (int band = 0; band < minato::audiofx::kBands; ++band) effect.params[band] = applied.bands[band];
    syscall(APKRT_NR_AUDIO_EFFECT, reinterpret_cast<uint64_t>(&effect));
    effect = {};
    effect.session = session;
    effect.kind = APKRT_AUDIO_EFFECT_BASS_BOOST;
    effect.enabled = applied.bass_boost ? 1 : 0;
    effect.params[0] = applied.bass_strength;
    syscall(APKRT_NR_AUDIO_EFFECT, reinterpret_cast<uint64_t>(&effect));
    effect = {};
    effect.session = session;
    effect.kind = APKRT_AUDIO_EFFECT_VIRTUALIZER;
    effect.enabled = applied.virtualizer ? 1 : 0;
    effect.params[0] = applied.virtualizer_strength;
    syscall(APKRT_NR_AUDIO_EFFECT, reinterpret_cast<uint64_t>(&effect));
    effect = {};
    effect.session = session;
    effect.kind = APKRT_AUDIO_EFFECT_LOUDNESS;
    effect.enabled = applied.loudness ? 1 : 0;
    effect.params[0] = applied.loudness_millibel;
    syscall(APKRT_NR_AUDIO_EFFECT, reinterpret_cast<uint64_t>(&effect));
    effect = {};
    effect.session = session;
    effect.kind = APKRT_AUDIO_EFFECT_REVERB;
    effect.enabled = applied.reverb ? 1 : 0;
    for (int i = 0; i < 7; ++i) effect.params[i] = applied.reverb_params[i];
    syscall(APKRT_NR_AUDIO_EFFECT, reinterpret_cast<uint64_t>(&effect));
}

std::string str(JNIEnv *env, jstring value) {
    if (value == nullptr) return std::string();
    const char *chars = env->GetStringUTFChars(value, nullptr);
    std::string result(chars != nullptr ? chars : "");
    if (chars != nullptr) env->ReleaseStringUTFChars(value, chars);
    return result;
}

jobject uuidOf(JNIEnv *env, const char *text) {
    jstring string = env->NewStringUTF(text);
    jobject uuid = env->CallStaticObjectMethod(gFields.uuidClass, gFields.uuidFromString, string);
    env->DeleteLocalRef(string);
    return uuid;
}

jobject descriptorOf(JNIEnv *env, const Entry &entry) {
    jobject descriptor = env->NewObject(gFields.descriptorClass, gFields.descriptorInit);
    if (descriptor == nullptr) return nullptr;
    jobject type = uuidOf(env, entry.descriptor.type);
    jobject uuid = uuidOf(env, entry.descriptor.uuid);
    env->SetObjectField(descriptor, gFields.type, type);
    env->SetObjectField(descriptor, gFields.uuid, uuid);
    jstring mode = env->NewStringUTF(entry.descriptor.mode);
    jstring name = env->NewStringUTF(entry.descriptor.name);
    jstring implementor = env->NewStringUTF(entry.descriptor.implementor);
    env->SetObjectField(descriptor, gFields.connectMode, mode);
    env->SetObjectField(descriptor, gFields.name, name);
    env->SetObjectField(descriptor, gFields.implementor, implementor);
    for (jobject local : {type, uuid, static_cast<jobject>(mode), static_cast<jobject>(name),
                          static_cast<jobject>(implementor)}) {
        env->DeleteLocalRef(local);
    }
    return descriptor;
}

void init(JNIEnv *env, jclass clazz) {
    gFields.effectClass = static_cast<jclass>(env->NewGlobalRef(clazz));
    gFields.nativeInstance = env->GetFieldID(clazz, "mNativeAudioEffect", "J");
    gFields.postEvent = env->GetStaticMethodID(clazz, "postEventFromNative",
                                               "(Ljava/lang/Object;IIILjava/lang/Object;)V");
    jclass descriptor = env->FindClass("android/media/audiofx/AudioEffect$Descriptor");
    gFields.descriptorClass = static_cast<jclass>(env->NewGlobalRef(descriptor));
    gFields.descriptorInit = env->GetMethodID(descriptor, "<init>", "()V");
    gFields.type = env->GetFieldID(descriptor, "type", "Ljava/util/UUID;");
    gFields.uuid = env->GetFieldID(descriptor, "uuid", "Ljava/util/UUID;");
    gFields.connectMode = env->GetFieldID(descriptor, "connectMode", "Ljava/lang/String;");
    gFields.name = env->GetFieldID(descriptor, "name", "Ljava/lang/String;");
    gFields.implementor = env->GetFieldID(descriptor, "implementor", "Ljava/lang/String;");
    jclass uuid = env->FindClass("java/util/UUID");
    gFields.uuidClass = static_cast<jclass>(env->NewGlobalRef(uuid));
    gFields.uuidFromString = env->GetStaticMethodID(uuid, "fromString", "(Ljava/lang/String;)Ljava/util/UUID;");
}

int instanceOf(JNIEnv *env, jobject thiz) {
    return static_cast<int>(env->GetLongField(thiz, gFields.nativeInstance));
}

jint setup(JNIEnv *env, jobject thiz, jobject weakThis, jstring type, jstring uuid, jint priority, jint session,
           jint, jstring, jintArray id, jobjectArray desc, jobject, jboolean probe) {
    if (env->GetLongField(thiz, gFields.nativeInstance) != 0) {
        return minato::audiofx::kInvalidOperation;  // set up already
    }
    std::lock_guard<std::mutex> hold(gLock);
    gEnv = env;
    int made = 0;
    const Entry *entry = nullptr;
    if (probe) {
        // A probe only asks whether the effect could be attached: it makes no instance, so no instance in control
        // loses it and gets it back.
        const int answer = gService.probe(str(env, type), str(env, uuid), priority, session, &entry);
        if (answer != minato::audiofx::kSuccess && answer != minato::audiofx::kAlreadyExists) {
            gEnv = nullptr;
            return answer;
        }
        jint ids[1] = {0};
        env->SetIntArrayRegion(id, 0, 1, ids);
        jobject descriptor = descriptorOf(env, *entry);
        env->SetObjectArrayElement(desc, 0, descriptor);
        env->DeleteLocalRef(descriptor);
        gEnv = nullptr;
        return answer;
    }
    const int status = gService.create(str(env, type), str(env, uuid), priority, session, &made, &entry,
                                       controlChanged, nullptr);
    if (status != minato::audiofx::kSuccess && status != minato::audiofx::kAlreadyExists) {
        gEnv = nullptr;
        return status;
    }
    gObjects[made] = env->NewGlobalRef(weakThis);
    env->SetLongField(thiz, gFields.nativeInstance, static_cast<jlong>(made));
    jint ids[1] = {made};
    env->SetIntArrayRegion(id, 0, 1, ids);
    jobject descriptor = descriptorOf(env, *entry);
    env->SetObjectArrayElement(desc, 0, descriptor);
    env->DeleteLocalRef(descriptor);
    publish(session);  // a new instance may take control from one that sounds
    gEnv = nullptr;
    return status;
}

void releaseInstance(JNIEnv *env, jobject thiz) {
    const int id = instanceOf(env, thiz);
    if (id == 0) return;
    std::lock_guard<std::mutex> hold(gLock);
    gEnv = env;
    auto found = gObjects.find(id);
    if (found != gObjects.end()) {
        env->DeleteGlobalRef(found->second);
        gObjects.erase(found);
    }
    minato::audiofx::Instance *instance = gService.get(id);
    const int session = instance != nullptr ? instance->session : 0;
    gService.release(id);
    if (instance != nullptr) publish(session);  // control may pass to another instance, or the effect ends
    env->SetLongField(thiz, gFields.nativeInstance, 0);
    gEnv = nullptr;
}

void finalize(JNIEnv *env, jobject thiz) {
    releaseInstance(env, thiz);
}

void release(JNIEnv *env, jobject thiz) {
    releaseInstance(env, thiz);
}

jint setEnabled(JNIEnv *env, jobject thiz, jboolean enabled) {
    std::lock_guard<std::mutex> hold(gLock);
    const int id = instanceOf(env, thiz);
    const int status = gService.set_enabled(id, enabled);
    minato::audiofx::Instance *instance = gService.get(id);
    if (status == minato::audiofx::kSuccess && instance != nullptr) publish(instance->session);
    return status;
}

jboolean getEnabled(JNIEnv *env, jobject thiz) {
    std::lock_guard<std::mutex> hold(gLock);
    minato::audiofx::Instance *instance = gService.get(instanceOf(env, thiz));
    return instance != nullptr && instance->enabled;
}

jboolean hasControl(JNIEnv *env, jobject thiz) {
    std::lock_guard<std::mutex> hold(gLock);
    minato::audiofx::Instance *instance = gService.get(instanceOf(env, thiz));
    return instance != nullptr && instance->has_control;
}

jint setParameter(JNIEnv *env, jobject thiz, jint psize, jbyteArray param, jint vsize, jbyteArray value) {
    if (param == nullptr || value == nullptr || psize < 0 || vsize < 0) return minato::audiofx::kBadValue;
    std::vector<uint8_t> p(static_cast<size_t>(psize)), v(static_cast<size_t>(vsize));
    env->GetByteArrayRegion(param, 0, psize, reinterpret_cast<jbyte *>(p.data()));
    env->GetByteArrayRegion(value, 0, vsize, reinterpret_cast<jbyte *>(v.data()));
    std::lock_guard<std::mutex> hold(gLock);
    const int id = instanceOf(env, thiz);
    const int status = gService.set_parameter(id, p.data(), psize, v.data(), vsize);
    minato::audiofx::Instance *instance = gService.get(id);
    if (status == minato::audiofx::kSuccess && instance != nullptr) publish(instance->session);
    return status;
}

jint getParameter(JNIEnv *env, jobject thiz, jint psize, jbyteArray param, jint vsize, jbyteArray value) {
    if (param == nullptr || value == nullptr || psize < 0 || vsize < 0) return minato::audiofx::kBadValue;
    std::vector<uint8_t> p(static_cast<size_t>(psize)), v(static_cast<size_t>(vsize));
    env->GetByteArrayRegion(param, 0, psize, reinterpret_cast<jbyte *>(p.data()));
    jint status;
    {
        std::lock_guard<std::mutex> hold(gLock);
        status = gService.get_parameter(instanceOf(env, thiz), p.data(), psize, v.data(), vsize);
    }
    if (status > 0) env->SetByteArrayRegion(value, 0, status, reinterpret_cast<const jbyte *>(v.data()));
    return status;
}

jint command(JNIEnv *, jobject, jint, jint, jbyteArray, jint, jbyteArray) {
    return minato::audiofx::kInvalidOperation;  // the commands of an effect engine: none here, the parameters do
}

jobjectArray queryEffects(JNIEnv *env, jclass) {
    const auto &list = minato::audiofx::entries();
    jobjectArray result = env->NewObjectArray(static_cast<jsize>(list.size()), gFields.descriptorClass, nullptr);
    for (size_t index = 0; index < list.size(); ++index) {
        jobject descriptor = descriptorOf(env, list[index]);
        env->SetObjectArrayElement(result, static_cast<jsize>(index), descriptor);
        env->DeleteLocalRef(descriptor);
    }
    return result;
}

jobjectArray queryPreProcessing(JNIEnv *env, jclass, jint) {
    // The pre-processings (echo canceler, gain control, noise suppression) are not in the emulator's list.
    return env->NewObjectArray(0, gFields.descriptorClass, nullptr);
}

const JNINativeMethod kMethods[] = {
    {"native_init", "()V", reinterpret_cast<void *>(init)},
    {"native_setup",
     "(Ljava/lang/Object;Ljava/lang/String;Ljava/lang/String;IIILjava/lang/String;[I[Ljava/lang/Object;"
     "Landroid/os/Parcel;Z)I",
     reinterpret_cast<void *>(setup)},
    {"native_finalize", "()V", reinterpret_cast<void *>(finalize)},
    {"native_release", "()V", reinterpret_cast<void *>(release)},
    {"native_setEnabled", "(Z)I", reinterpret_cast<void *>(setEnabled)},
    {"native_getEnabled", "()Z", reinterpret_cast<void *>(getEnabled)},
    {"native_hasControl", "()Z", reinterpret_cast<void *>(hasControl)},
    {"native_setParameter", "(I[BI[B)I", reinterpret_cast<void *>(setParameter)},
    {"native_getParameter", "(I[BI[B)I", reinterpret_cast<void *>(getParameter)},
    {"native_command", "(II[BI[B)I", reinterpret_cast<void *>(command)},
    {"native_query_effects", "()[Ljava/lang/Object;", reinterpret_cast<void *>(queryEffects)},
    {"native_query_pre_processing", "(I)[Ljava/lang/Object;", reinterpret_cast<void *>(queryPreProcessing)},
};

}  // namespace

int register_android_media_AudioEffect(JNIEnv *env) {
    return RegisterMethodsOrDie(env, "android/media/audiofx/AudioEffect", kMethods, NELEM(kMethods));
}

}  // namespace android
