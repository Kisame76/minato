// Ours in place of AOSP's core/jni/android_hardware_Camera.cpp (ADR-0073 step e): the legacy android.hardware.Camera
// on the host's cameras (guest_interface.h's camera, 4137), answering as AOSP's natives do over libcamera_client and
// cameraservice's Camera2Client [verified: android_hardware_Camera.cpp and Parameters.cpp at android-15.0.0_r1,
// camera-legacy-test on apk-runner-35]:
// - _getNumberOfCameras is the host's count (0 without host cameras, as on a device without them). _getCameraInfo
//   gives a camera's legacy facing (camera2's back is CAMERA_FACING_BACK 0) and orientation from the host's facts,
//   and canDisableShutterSound true (ro.camera.sound.forced is 0); an ID past the cameras throws
//   RuntimeException("Unknown camera ID") with AOSP's log line, facts that do not hold "Fail to get camera info".
// - native_setup opens the host's camera; without the guest's camera grant (ADR-0073 decision 8) or for an ID past the
//   cameras it answers -EACCES, as Camera::connect's failure, so that open throws RuntimeException("Fail to connect to
//   camera service"). A second open of a camera that is open takes it from the first Camera, as cameraservice evicts
//   the older client of the same app: the first one's calls then fail as a disconnected camera's
//   (getParameters "getParameters failed (empty parameters)", setParameters "setParameters failed", lock "lock
//   failed", unlock "unlock failed", reconnect IOException "reconnect failed"), and its release only lets it go
//   [verified: camera-legacy-test]. Its parameters are Parameters::initialize's (camera_parameters.h) from the
//   camera's facts, with Minato's video sizes up to 1920 x 1080 (a named limit: no encoders yet); native_release
//   closes it, also when the finalizer comes after release.
// - native_getParameters gives the parameters; native_setParameters takes what Parameters::set takes (the lists'
//   values) and keeps the app's string, as CameraParameters2 does, else throws RuntimeException("setParameters
//   failed"). lock, unlock and reconnect succeed. Each throws RuntimeException("Camera is being used after
//   Camera.release() was called") after a release, as get_native_camera does.
// Camera's other natives (preview, focus, pictures, zoom, faces) stay unregistered until the next parts of step e.
#include <errno.h>
#include <jni.h>
#include <log/log.h>
#include <nativehelper/JNIHelp.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

#include <APKRuntime/apkrt.h>
#include <APKRuntime/guest_interface.h>

#include "camera_parameters.h"
#include "core_jni_helpers.h"

namespace android {
namespace {

constexpr jint kFacingBack = 0, kFacingFront = 1;  // Camera.CameraInfo's

struct {
    jfieldID context;  // Camera.mNativeContext
    jfieldID facing, orientation, can_disable_shutter_sound;
} gFields;

std::mutex gLock;  // around the contexts, as AOSP's sLock

long camera_call(uint64_t op, apkrt_camera *call) {
    long result;
    do {
        result = syscall(APKRT_NR_CAMERA, op, call);
    } while (result == -1 && errno == EINTR);
    return result == -1 ? -errno : result;
}

uint32_t camera_count() {
    apkrt_camera call{};
    return camera_call(APKRT_CAMERA_COUNT, &call) == 0 ? call.count : 0;
}

/// The host's facts of camera `index` read as a camera's; false where it has none that hold.
bool camera_facts(uint32_t index, minato::camera::Facts *facts) {
    std::vector<unsigned char> bytes(APKRT_CAMERA_MAX_FACTS);
    apkrt_camera call{};
    call.camera = index;
    call.data = reinterpret_cast<uintptr_t>(bytes.data());
    call.size = bytes.size();
    if (camera_call(APKRT_CAMERA_FACTS, &call) != 0) return false;
    return minato::camera::read_facts(bytes.data(), static_cast<size_t>(call.size), facts);
}

/// An open camera of a Java Camera.
struct Context {
    uint64_t id = 0;
    uint32_t index = 0;
    bool evicted = false;  // a second open took the camera: the host's camera is closed
    minato::camera::Facts facts;
    std::string initial, parameters;  // Parameters::initialize's; the ones in force
};

std::vector<Context *> gOpen;  // the contexts that hold their camera, with gLock held

void close_camera(Context *context) {
    apkrt_camera call{};
    call.id = context->id;
    camera_call(APKRT_CAMERA_CLOSE, &call);
}

/// The open camera of `thiz` with gLock held by the caller, or NULL with AOSP's RuntimeException after a release.
Context *live(JNIEnv *env, jobject thiz) {
    auto *context = reinterpret_cast<Context *>(env->GetLongField(thiz, gFields.context));
    if (context == nullptr) {
        jniThrowRuntimeException(env, "Camera is being used after Camera.release() was called");
    }
    return context;
}

jint GetNumberOfCameras(JNIEnv *, jclass, jint, jint) {
    return static_cast<jint>(camera_count());
}

void GetCameraInfo(JNIEnv *env, jclass, jint cameraId, jint, jint, jint, jobject info) {
    if (cameraId < 0 || static_cast<uint32_t>(cameraId) >= camera_count()) {
        __android_log_print(ANDROID_LOG_ERROR, "Camera-JNI", "%s: Unknown camera ID %d",
                            "android_hardware_Camera_getCameraInfo", cameraId);
        jniThrowRuntimeException(env, "Unknown camera ID");
        return;
    }
    minato::camera::Facts facts;
    if (!camera_facts(static_cast<uint32_t>(cameraId), &facts)) {
        jniThrowRuntimeException(env, "Fail to get camera info");
        return;
    }
    env->SetIntField(info, gFields.facing, facts.facing == 1 ? kFacingBack : kFacingFront);
    env->SetIntField(info, gFields.orientation, facts.orientation);
    env->SetBooleanField(info, gFields.can_disable_shutter_sound, JNI_TRUE);
}

jint Setup(JNIEnv *env, jobject thiz, jobject, jint cameraId, jstring, jint, jboolean, jint, jint) {
    if (cameraId < 0 || static_cast<uint32_t>(cameraId) >= camera_count()) return -EACCES;
    auto *context = new Context;
    if (!camera_facts(static_cast<uint32_t>(cameraId), &context->facts)) {
        delete context;
        return -EACCES;
    }
    context->initial = minato::camera::legacy_parameters(context->facts.info, minato::camera::kVideoBounds);
    context->parameters = context->initial;
    context->index = static_cast<uint32_t>(cameraId);
    const std::lock_guard<std::mutex> hold(gLock);
    if (context->initial.empty()) {
        delete context;
        return -EACCES;
    }
    // cameraservice evicts the older client of the same app.
    for (Context *older : gOpen) {
        if (older->index != context->index) continue;
        close_camera(older);
        older->evicted = true;
    }
    gOpen.erase(std::remove_if(gOpen.begin(), gOpen.end(), [](Context *each) { return each->evicted; }), gOpen.end());
    apkrt_camera call{};
    call.camera = context->index;
    if (camera_call(APKRT_CAMERA_OPEN, &call) != 0) {
        delete context;
        return -EACCES;  // Camera::connect's failure
    }
    context->id = call.id;
    gOpen.push_back(context);
    env->SetLongField(thiz, gFields.context, reinterpret_cast<jlong>(context));
    return 0;
}

void Release(JNIEnv *env, jobject thiz) {
    const std::lock_guard<std::mutex> hold(gLock);
    auto *context = reinterpret_cast<Context *>(env->GetLongField(thiz, gFields.context));
    env->SetLongField(thiz, gFields.context, 0);
    if (context == nullptr) return;  // released before, or never opened
    if (!context->evicted) close_camera(context);
    gOpen.erase(std::remove(gOpen.begin(), gOpen.end(), context), gOpen.end());
    delete context;
}

jstring GetParameters(JNIEnv *env, jobject thiz) {
    const std::lock_guard<std::mutex> hold(gLock);
    Context *context = live(env, thiz);
    if (context == nullptr) return nullptr;
    if (context->evicted) {
        jniThrowRuntimeException(env, "getParameters failed (empty parameters)");
        return nullptr;
    }
    return env->NewStringUTF(context->parameters.c_str());
}

void SetParameters(JNIEnv *env, jobject thiz, jstring params) {
    std::string asked;
    if (params != nullptr) {
        const char *chars = env->GetStringUTFChars(params, nullptr);
        if (chars == nullptr) return;
        asked = chars;
        env->ReleaseStringUTFChars(params, chars);
    }
    const std::lock_guard<std::mutex> hold(gLock);
    Context *context = live(env, thiz);
    if (context == nullptr) return;
    if (context->evicted || !minato::camera::accept_parameters(context->initial, asked)) {
        jniThrowRuntimeException(env, "setParameters failed");
        return;
    }
    context->parameters = asked;
}

/// lock, unlock and reconnect: the camera stays the app's alone, so each succeeds while the Camera holds it.
void Keep(JNIEnv *env, jobject thiz, const char *exception, const char *message) {
    const std::lock_guard<std::mutex> hold(gLock);
    Context *context = live(env, thiz);
    if (context != nullptr && context->evicted) jniThrowException(env, exception, message);
}

void Lock(JNIEnv *env, jobject thiz) { Keep(env, thiz, "java/lang/RuntimeException", "lock failed"); }
void Unlock(JNIEnv *env, jobject thiz) { Keep(env, thiz, "java/lang/RuntimeException", "unlock failed"); }
void Reconnect(JNIEnv *env, jobject thiz) { Keep(env, thiz, "java/io/IOException", "reconnect failed"); }

const JNINativeMethod gCameraMethods[] = {
        {"_getNumberOfCameras", "(II)I", reinterpret_cast<void *>(GetNumberOfCameras)},
        {"_getCameraInfo", "(IIIILandroid/hardware/Camera$CameraInfo;)V", reinterpret_cast<void *>(GetCameraInfo)},
        {"native_setup", "(Ljava/lang/Object;ILjava/lang/String;IZII)I", reinterpret_cast<void *>(Setup)},
        {"native_release", "()V", reinterpret_cast<void *>(Release)},
        {"native_getParameters", "()Ljava/lang/String;", reinterpret_cast<void *>(GetParameters)},
        {"native_setParameters", "(Ljava/lang/String;)V", reinterpret_cast<void *>(SetParameters)},
        {"lock", "()V", reinterpret_cast<void *>(Lock)},
        {"unlock", "()V", reinterpret_cast<void *>(Unlock)},
        {"reconnect", "()V", reinterpret_cast<void *>(Reconnect)},
};

}  // namespace

int register_android_hardware_Camera(JNIEnv *env) {
    jclass camera = FindClassOrDie(env, "android/hardware/Camera");
    gFields.context = GetFieldIDOrDie(env, camera, "mNativeContext", "J");
    jclass info = FindClassOrDie(env, "android/hardware/Camera$CameraInfo");
    gFields.facing = GetFieldIDOrDie(env, info, "facing", "I");
    gFields.orientation = GetFieldIDOrDie(env, info, "orientation", "I");
    gFields.can_disable_shutter_sound = GetFieldIDOrDie(env, info, "canDisableShutterSound", "Z");
    return RegisterMethodsOrDie(env, "android/hardware/Camera", gCameraMethods, NELEM(gCameraMethods));
}

}  // namespace android
