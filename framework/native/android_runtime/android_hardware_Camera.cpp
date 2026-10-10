// Ours in place of AOSP's core/jni/android_hardware_Camera.cpp (the Unity test APK probe's batch C): the legacy
// android.hardware.Camera on a device without cameras, answering as AOSP's natives do over libcamera_client there
// [verified: android_hardware_Camera.cpp at android-15.0.0_r1, webview-camera-test on apk-runner-35-bare]:
// _getNumberOfCameras answers 0, so Camera.open() answers null; _getCameraInfo throws RuntimeException("Unknown camera
// ID") for every ID, with AOSP's log line; native_setup answers -EACCES, as Camera::connect gives no camera, so
// open(int) throws RuntimeException("Fail to connect to camera service"); native_release, which the finalizer of that
// half-made Camera calls, has no camera to release. Camera's other natives stay unregistered: each needs an open
// camera.
#include <errno.h>
#include <jni.h>
#include <log/log.h>
#include <nativehelper/JNIHelp.h>

#include "core_jni_helpers.h"

namespace android {
namespace {

jint GetNumberOfCameras(JNIEnv *, jclass, jint, jint) {
    return 0;
}

void GetCameraInfo(JNIEnv *env, jclass, jint cameraId, jint, jint, jint, jobject) {
    __android_log_print(ANDROID_LOG_ERROR, "Camera-JNI", "%s: Unknown camera ID %d",
                        "android_hardware_Camera_getCameraInfo", cameraId);
    jniThrowRuntimeException(env, "Unknown camera ID");
}

jint Setup(JNIEnv *, jobject, jobject, jint, jstring, jint, jboolean, jint, jint) {
    return -EACCES;
}

void Release(JNIEnv *, jobject) {}

const JNINativeMethod gCameraMethods[] = {
        {"_getNumberOfCameras", "(II)I", reinterpret_cast<void *>(GetNumberOfCameras)},
        {"_getCameraInfo", "(IIIILandroid/hardware/Camera$CameraInfo;)V", reinterpret_cast<void *>(GetCameraInfo)},
        {"native_setup", "(Ljava/lang/Object;ILjava/lang/String;IZII)I", reinterpret_cast<void *>(Setup)},
        {"native_release", "()V", reinterpret_cast<void *>(Release)},
};

}  // namespace

int register_android_hardware_Camera(JNIEnv *env) {
    return RegisterMethodsOrDie(env, "android/hardware/Camera", gCameraMethods, NELEM(gCameraMethods));
}

}  // namespace android
