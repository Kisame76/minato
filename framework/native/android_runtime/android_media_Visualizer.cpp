// Ours (the audio effects plan): the natives of AOSP's android.media.audiofx.Visualizer, which on Android stand on
// libaudioclient's Visualizer and the effect engine. An instance is a visualizer_state.h State: the capture size, the
// modes, the enabled state and the periodic capture, with Android 15's answers (visualizer-test). Creating one needs
// RECORD_AUDIO's grant, as the effect engine's does, here the runtime's stored microphone answer. The data is the
// output mix's: silence until the session's output is tapped. The periodic capture has a thread per instance that
// posts the waveform and the FFT to Visualizer.postEventFromNative at the rate asked for, while the instance is
// enabled.
#include <jni.h>
#include <nativehelper/JNIHelp.h>
#include <stdint.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "APKRuntime/guest_interface.h"
#include "core_jni_helpers.h"
#include "visualizer_state.h"

namespace android {
namespace {

using minato::visualizer::State;

constexpr int NATIVE_EVENT_PCM_CAPTURE = 0, NATIVE_EVENT_FFT_CAPTURE = 1;

struct Instance {
    State state;
    jobject object = nullptr;
    std::thread thread;
    bool stop = false;
};

std::mutex gLock;
std::condition_variable gChanged;
std::map<int, std::unique_ptr<Instance>> gInstances;
int gNextId = 0;
JavaVM *gVm = nullptr;

struct Fields {
    jfieldID nativeInstance;  // mNativeVisualizer: the instance's id
    jmethodID postEvent;
    jclass visualizerClass;
    jfieldID peak, rms;
} gFields;

/// The periodic capture of one instance: while it is enabled and a rate is set, every period the waveform and the FFT
/// go to the Java side, which posts them to the app's listener.
void captureLoop(int id) {
    JNIEnv *env = nullptr;
    if (gVm->AttachCurrentThread(&env, nullptr) != JNI_OK) return;
    std::unique_lock<std::mutex> hold(gLock);
    while (true) {
        auto found = gInstances.find(id);
        if (found == gInstances.end() || found->second->stop) break;
        Instance &instance = *found->second;
        if (!instance.state.enabled || !instance.state.periodic()) {
            gChanged.wait(hold);
            continue;
        }
        const auto period = std::chrono::microseconds(1000000000LL / instance.state.rate);
        gChanged.wait_for(hold, period);
        found = gInstances.find(id);
        if (found == gInstances.end() || found->second->stop) break;
        Instance &now = *found->second;
        if (!now.state.enabled || !now.state.periodic()) continue;
        std::vector<uint8_t> waveform, fft;
        const size_t size = static_cast<size_t>(now.state.capture_size);
        now.state.get_waveform(waveform, size);
        now.state.get_fft(fft, size);
        const bool wantWaveform = now.state.periodic_waveform, wantFft = now.state.periodic_fft;
        jobject object = now.object;
        hold.unlock();
        for (int what : {NATIVE_EVENT_PCM_CAPTURE, NATIVE_EVENT_FFT_CAPTURE}) {
            if ((what == NATIVE_EVENT_PCM_CAPTURE ? !wantWaveform : !wantFft)) continue;
            const std::vector<uint8_t> &data = what == NATIVE_EVENT_PCM_CAPTURE ? waveform : fft;
            jbyteArray array = env->NewByteArray(static_cast<jsize>(data.size()));
            env->SetByteArrayRegion(array, 0, static_cast<jsize>(data.size()),
                                    reinterpret_cast<const jbyte *>(data.data()));
            env->CallStaticVoidMethod(gFields.visualizerClass, gFields.postEvent, object, what,
                                      minato::visualizer::kSamplingRate, array);
            if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); }
            env->DeleteLocalRef(array);
        }
        hold.lock();
    }
    hold.unlock();
    gVm->DetachCurrentThread();
}

Instance *instanceOf(JNIEnv *env, jobject thiz) {
    auto found = gInstances.find(static_cast<int>(env->GetLongField(thiz, gFields.nativeInstance)));
    return found == gInstances.end() ? nullptr : found->second.get();
}

void init(JNIEnv *env, jclass clazz) {
    env->GetJavaVM(&gVm);
    gFields.visualizerClass = static_cast<jclass>(env->NewGlobalRef(clazz));
    gFields.nativeInstance = env->GetFieldID(clazz, "mNativeVisualizer", "J");
    gFields.postEvent = env->GetStaticMethodID(clazz, "postEventFromNative", "(Ljava/lang/Object;II[B)V");
    jclass peakRms = env->FindClass("android/media/audiofx/Visualizer$MeasurementPeakRms");
    gFields.peak = env->GetFieldID(peakRms, "mPeak", "I");
    gFields.rms = env->GetFieldID(peakRms, "mRms", "I");
}

/// Whether the runtime holds the microphone grant (0 denied or unanswered).
bool microphoneGranted() {
    return syscall(APKRT_NR_LOCATION, uint64_t{APKRT_LOCATION_PERMISSION}, uint64_t{APKRT_PERMISSION_MICROPHONE},
                   uint64_t{0}) > 0;
}

jint setup(JNIEnv *env, jobject thiz, jobject weakThis, jint, jintArray id, jobject) {
    if (!microphoneGranted()) return minato::visualizer::kError;
    std::lock_guard<std::mutex> hold(gLock);
    auto instance = std::make_unique<Instance>();
    const int made = ++gNextId;
    instance->object = env->NewGlobalRef(weakThis);  // the java.lang.ref.WeakReference holds the Visualizer weakly
    Instance *raw = instance.get();
    gInstances[made] = std::move(instance);
    raw->thread = std::thread(captureLoop, made);
    env->SetLongField(thiz, gFields.nativeInstance, static_cast<jlong>(made));
    jint ids[1] = {made};
    env->SetIntArrayRegion(id, 0, 1, ids);
    return minato::visualizer::kSuccess;
}

void releaseInstance(JNIEnv *env, jobject thiz) {
    const int id = static_cast<int>(env->GetLongField(thiz, gFields.nativeInstance));
    std::unique_ptr<Instance> instance;
    {
        std::lock_guard<std::mutex> hold(gLock);
        auto found = gInstances.find(id);
        if (found == gInstances.end()) return;
        found->second->stop = true;
        gChanged.notify_all();
        instance = std::move(found->second);
        gInstances.erase(found);
    }
    if (instance->thread.joinable()) instance->thread.join();
    if (instance->object != nullptr) env->DeleteGlobalRef(instance->object);
    env->SetLongField(thiz, gFields.nativeInstance, 0);
}

void finalize(JNIEnv *env, jobject thiz) {
    releaseInstance(env, thiz);
}

void release(JNIEnv *env, jobject thiz) {
    releaseInstance(env, thiz);
}

jint setEnabled(JNIEnv *env, jobject thiz, jboolean enabled) {
    std::lock_guard<std::mutex> hold(gLock);
    Instance *instance = instanceOf(env, thiz);
    if (instance == nullptr) return minato::visualizer::kNoInit;
    const int status = instance->state.set_enabled(enabled);
    gChanged.notify_all();
    return status;
}

jboolean getEnabled(JNIEnv *env, jobject thiz) {
    std::lock_guard<std::mutex> hold(gLock);
    Instance *instance = instanceOf(env, thiz);
    return instance != nullptr && instance->state.enabled;
}

jint setCaptureSize(JNIEnv *env, jobject thiz, jint size) {
    std::lock_guard<std::mutex> hold(gLock);
    Instance *instance = instanceOf(env, thiz);
    return instance == nullptr ? minato::visualizer::kNoInit : instance->state.set_capture_size(size);
}

jint getCaptureSize(JNIEnv *env, jobject thiz) {
    std::lock_guard<std::mutex> hold(gLock);
    Instance *instance = instanceOf(env, thiz);
    return instance == nullptr ? minato::visualizer::kNoInit : instance->state.capture_size;
}

jint setScalingMode(JNIEnv *env, jobject thiz, jint mode) {
    std::lock_guard<std::mutex> hold(gLock);
    Instance *instance = instanceOf(env, thiz);
    return instance == nullptr ? minato::visualizer::kNoInit : instance->state.set_scaling_mode(mode);
}

jint getScalingMode(JNIEnv *env, jobject thiz) {
    std::lock_guard<std::mutex> hold(gLock);
    Instance *instance = instanceOf(env, thiz);
    return instance == nullptr ? minato::visualizer::kNoInit : instance->state.scaling_mode;
}

jint setMeasurementMode(JNIEnv *env, jobject thiz, jint mode) {
    std::lock_guard<std::mutex> hold(gLock);
    Instance *instance = instanceOf(env, thiz);
    return instance == nullptr ? minato::visualizer::kNoInit : instance->state.set_measurement_mode(mode);
}

jint getMeasurementMode(JNIEnv *env, jobject thiz) {
    std::lock_guard<std::mutex> hold(gLock);
    Instance *instance = instanceOf(env, thiz);
    return instance == nullptr ? minato::visualizer::kNoInit : instance->state.measurement_mode;
}

jint getSamplingRate(JNIEnv *, jobject) {
    return minato::visualizer::kSamplingRate;
}

jint fillArray(JNIEnv *env, jobject thiz, jbyteArray array, bool fft) {
    std::vector<uint8_t> data;
    int status;
    {
        std::lock_guard<std::mutex> hold(gLock);
        Instance *instance = instanceOf(env, thiz);
        if (instance == nullptr) return minato::visualizer::kNoInit;
        const size_t room = static_cast<size_t>(env->GetArrayLength(array));
        status = fft ? instance->state.get_fft(data, room) : instance->state.get_waveform(data, room);
    }
    if (status == minato::visualizer::kSuccess) {
        env->SetByteArrayRegion(array, 0, static_cast<jsize>(data.size()),
                                reinterpret_cast<const jbyte *>(data.data()));
    }
    return status;
}

jint getWaveForm(JNIEnv *env, jobject thiz, jbyteArray array) {
    return fillArray(env, thiz, array, false);
}

jint getFft(JNIEnv *env, jobject thiz, jbyteArray array) {
    return fillArray(env, thiz, array, true);
}

jint getPeakRms(JNIEnv *env, jobject thiz, jobject measurement) {
    int peak = 0, rms = 0, status;
    {
        std::lock_guard<std::mutex> hold(gLock);
        Instance *instance = instanceOf(env, thiz);
        if (instance == nullptr) return minato::visualizer::kNoInit;
        status = instance->state.get_peak_rms(&peak, &rms);
    }
    if (status == minato::visualizer::kSuccess) {
        env->SetIntField(measurement, gFields.peak, peak);
        env->SetIntField(measurement, gFields.rms, rms);
    }
    return status;
}

jint setPeriodicCapture(JNIEnv *env, jobject thiz, jint rate, jboolean waveform, jboolean fft) {
    std::lock_guard<std::mutex> hold(gLock);
    Instance *instance = instanceOf(env, thiz);
    if (instance == nullptr) return minato::visualizer::kNoInit;
    const int status = instance->state.set_periodic_capture(rate, waveform, fft);
    gChanged.notify_all();
    return status;
}

jintArray getCaptureSizeRange(JNIEnv *env, jclass) {
    jintArray result = env->NewIntArray(2);
    jint range[2] = {minato::visualizer::kMinCaptureSize, minato::visualizer::kMaxCaptureSize};
    env->SetIntArrayRegion(result, 0, 2, range);
    return result;
}

jint getMaxCaptureRate(JNIEnv *, jclass) {
    return minato::visualizer::kMaxCaptureRate;
}

const JNINativeMethod kMethods[] = {
    {"native_init", "()V", reinterpret_cast<void *>(init)},
    {"native_setup", "(Ljava/lang/Object;I[ILandroid/os/Parcel;)I", reinterpret_cast<void *>(setup)},
    {"native_finalize", "()V", reinterpret_cast<void *>(finalize)},
    {"native_release", "()V", reinterpret_cast<void *>(release)},
    {"native_setEnabled", "(Z)I", reinterpret_cast<void *>(setEnabled)},
    {"native_getEnabled", "()Z", reinterpret_cast<void *>(getEnabled)},
    {"native_setCaptureSize", "(I)I", reinterpret_cast<void *>(setCaptureSize)},
    {"native_getCaptureSize", "()I", reinterpret_cast<void *>(getCaptureSize)},
    {"native_setScalingMode", "(I)I", reinterpret_cast<void *>(setScalingMode)},
    {"native_getScalingMode", "()I", reinterpret_cast<void *>(getScalingMode)},
    {"native_setMeasurementMode", "(I)I", reinterpret_cast<void *>(setMeasurementMode)},
    {"native_getMeasurementMode", "()I", reinterpret_cast<void *>(getMeasurementMode)},
    {"native_getSamplingRate", "()I", reinterpret_cast<void *>(getSamplingRate)},
    {"native_getWaveForm", "([B)I", reinterpret_cast<void *>(getWaveForm)},
    {"native_getFft", "([B)I", reinterpret_cast<void *>(getFft)},
    {"native_getPeakRms", "(Landroid/media/audiofx/Visualizer$MeasurementPeakRms;)I",
     reinterpret_cast<void *>(getPeakRms)},
    {"native_setPeriodicCapture", "(IZZ)I", reinterpret_cast<void *>(setPeriodicCapture)},
    {"getCaptureSizeRange", "()[I", reinterpret_cast<void *>(getCaptureSizeRange)},
    {"getMaxCaptureRate", "()I", reinterpret_cast<void *>(getMaxCaptureRate)},
};

}  // namespace

int register_android_media_Visualizer(JNIEnv *env) {
    return RegisterMethodsOrDie(env, "android/media/audiofx/Visualizer", kMethods, NELEM(kMethods));
}

}  // namespace android
