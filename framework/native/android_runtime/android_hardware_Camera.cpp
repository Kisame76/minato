// Ours in place of AOSP's core/jni/android_hardware_Camera.cpp (the Unity test APK probe's batch C): the legacy
// android.hardware.Camera on a device without cameras. Only _getNumberOfCameras is registered, which answers 0, as
// libcamera_client's Camera::getNumberOfCameras does there [verified: webview-camera-test on apk-runner-35-bare], so
// Camera.open() answers null. Its other natives stay unregistered: each needs a camera, which open(int) and
// getCameraInfo would name by an ID, and with no cameras there is none to name.
#include <jni.h>
#include <nativehelper/JNIHelp.h>

#include "core_jni_helpers.h"

namespace android {
namespace {

jint GetNumberOfCameras(JNIEnv *, jclass, jint, jint) {
    return 0;
}

const JNINativeMethod gCameraMethods[] = {
        {"_getNumberOfCameras", "(II)I", reinterpret_cast<void *>(GetNumberOfCameras)},
};

}  // namespace

int register_android_hardware_Camera(JNIEnv *env) {
    return RegisterMethodsOrDie(env, "android/hardware/Camera", gCameraMethods, NELEM(gCameraMethods));
}

}  // namespace android
