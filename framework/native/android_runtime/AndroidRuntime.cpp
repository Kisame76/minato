// Ours, not AOSP's core/jni/AndroidRuntime.cpp (ADR-0039 decision 10, b3.3). AOSP's file is zygote's runtime: start,
// startVm and gRegJNI's registrations of all of core/jni, which pull in libbinder and more. The core/jni files we
// take call only its static getJNIEnv (android_content_res_ApkAssets.cpp's LoaderAssetsProvider), so this file
// defines only AndroidRuntime's statics getJavaVM and getJNIEnv, with the VM app_process passes in through
// register_framework_natives (register_framework_natives.h). That stands in for AOSP's private startReg: it registers
// the natives of framework.jar's classes in gRegJNI's order, libhwui's graphics classes among them (ADR-0043 test 2),
// through hwui's jni_runtime.h as AOSP's does; AOSP's also hooks libutils' thread creation into the VM, which
// nothing of ours uses yet.
#include "register_framework_natives.h"

#include <android/graphics/jni_runtime.h>
#include <android_runtime/AndroidRuntime.h>

#include <jni.h>
#include <log/log.h>

namespace android {

extern int register_android_os_SystemClock(JNIEnv *env);
extern int register_android_util_Log(JNIEnv *env);
extern int register_android_content_AssetManager(JNIEnv *env);
extern int register_android_content_StringBlock(JNIEnv *env);
extern int register_android_content_XmlBlock(JNIEnv *env);
extern int register_android_content_res_ApkAssets(JNIEnv *env);
extern int register_android_os_SystemProperties(JNIEnv *env);
extern int register_android_view_DisplayEventReceiver(JNIEnv *env);
extern int register_android_hardware_display_DisplayService(JNIEnv *env);
extern int register_android_os_MessageQueue(JNIEnv *env);
extern int register_android_os_Trace(JNIEnv *env);
extern int register_android_hardware_SensorManager(JNIEnv *env);
extern int register_android_app_ActivityThread(JNIEnv *env);
extern int register_android_animation_PropertyValuesHolder(JNIEnv *env);
extern int register_android_util_EventLog(JNIEnv *env);
extern int register_android_app_Activity(JNIEnv *env);
extern int register_android_view_KeyEvent(JNIEnv *env);

namespace {
JavaVM *gJavaVM = nullptr;
}  // namespace

JavaVM *AndroidRuntime::getJavaVM() {
    return gJavaVM;
}

// As AOSP's: the calling thread's JNIEnv, or null when it is not attached to the VM.
JNIEnv *AndroidRuntime::getJNIEnv() {
    JavaVM *vm = getJavaVM();
    LOG_ALWAYS_FATAL_IF(vm == nullptr, "AndroidRuntime::getJNIEnv before register_framework_natives");
    JNIEnv *env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_4) != JNI_OK) {
        return nullptr;
    }
    return env;
}

int register_framework_natives(JNIEnv *env) {
    if (env->GetJavaVM(&gJavaVM) != JNI_OK) {
        return -1;
    }
    // AndroidRuntime's constructor calls it before the VM starts, and it registers nothing. Android's sets up Skia;
    // the host flavor's, layoutlib's, also pins hwui's pipeline to SkiaCpu for the process, which
    // Properties::overrideRenderPipelineType never lets change: the GPU pipeline's step changes this call.
    init_android_graphics();
    // Each registration's FindClass returns local references; the frame releases them, as AOSP's startReg does.
    env->PushLocalFrame(200);
    for (int (*registration)(JNIEnv *) :
         {register_android_os_SystemClock, register_android_util_Log, register_android_content_AssetManager,
          register_android_content_StringBlock, register_android_content_XmlBlock,
          register_android_content_res_ApkAssets, register_android_os_SystemProperties,
          register_android_view_DisplayEventReceiver, register_android_hardware_display_DisplayService,
          register_android_graphics_classes, register_android_os_MessageQueue, register_android_os_Trace,
          register_android_hardware_SensorManager,
          register_android_app_ActivityThread, register_android_animation_PropertyValuesHolder,
          register_android_util_EventLog, register_android_app_Activity, register_android_view_KeyEvent}) {
        if (registration(env) < 0) {
            env->PopLocalFrame(nullptr);
            return -1;
        }
    }
    env->PopLocalFrame(nullptr);
    return 0;
}

}  // namespace android
