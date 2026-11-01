// Ours, not AOSP's core/jni/AndroidRuntime.cpp (ADR-0039 decision 10, b3.3). AOSP's file is zygote's runtime: start,
// startVm and gRegJNI's registrations of all of core/jni, which pull in libbinder and more. The core/jni files we
// take call only its statics getJNIEnv (android_content_res_ApkAssets.cpp's LoaderAssetsProvider) and
// registerNativeMethods (the GLES bindings, ADR-0041 a)), so this file defines only AndroidRuntime's statics
// getJavaVM, getJNIEnv and registerNativeMethods, with the VM app_process passes in through
// register_framework_natives (register_framework_natives.h). That stands in for AOSP's private startReg: it registers
// the natives of framework.jar's classes in gRegJNI's order, libhwui's graphics classes among them (ADR-0043 test 2),
// through hwui's jni_runtime.h as AOSP's does; AOSP's also hooks libutils' thread creation into the VM, which
// nothing of ours uses yet.
#include "register_framework_natives.h"

#include <android/graphics/jni_runtime.h>
#include <android_runtime/AndroidRuntime.h>

#include <jni.h>
#include <log/log.h>
#include <nativehelper/JNIHelp.h>

// The GLES bindings (ADR-0041 a)): AOSP's core/jni files, which define these outside namespace android, as AOSP's
// AndroidRuntime.cpp declares them.
extern int register_com_google_android_gles_jni_EGLImpl(JNIEnv *env);
extern int register_com_google_android_gles_jni_GLImpl(JNIEnv *env);
extern int register_android_opengl_jni_EGL14(JNIEnv *env);
extern int register_android_opengl_jni_EGL15(JNIEnv *env);
extern int register_android_opengl_jni_EGLExt(JNIEnv *env);
extern int register_android_opengl_jni_GLES10(JNIEnv *env);
extern int register_android_opengl_jni_GLES10Ext(JNIEnv *env);
extern int register_android_opengl_jni_GLES11(JNIEnv *env);
extern int register_android_opengl_jni_GLES11Ext(JNIEnv *env);
extern int register_android_opengl_jni_GLES20(JNIEnv *env);
extern int register_android_opengl_jni_GLES30(JNIEnv *env);
extern int register_android_opengl_jni_GLES31(JNIEnv *env);
extern int register_android_opengl_jni_GLES31Ext(JNIEnv *env);
extern int register_android_opengl_jni_GLES32(JNIEnv *env);

namespace android {

extern int register_android_os_SystemClock(JNIEnv *env);
extern int register_android_util_Log(JNIEnv *env);
extern int register_android_content_AssetManager(JNIEnv *env);
extern int register_android_content_StringBlock(JNIEnv *env);
extern int register_android_content_XmlBlock(JNIEnv *env);
extern int register_android_content_res_ApkAssets(JNIEnv *env);
extern int register_android_os_SystemProperties(JNIEnv *env);
extern int register_android_os_Parcel(JNIEnv *env);
extern int register_android_view_DisplayEventReceiver(JNIEnv *env);
extern int register_android_view_Surface(JNIEnv *env);
extern int register_android_view_TextureView(JNIEnv *env);
extern int register_android_hardware_SyncFence(JNIEnv *env);
extern int register_android_media_ImageReader(JNIEnv *env);
extern int register_android_media_PublicFormatUtils(JNIEnv *env);
extern int register_android_media_ImageWriter(JNIEnv *env);
extern int register_android_media_MediaCodec(JNIEnv *env);
extern int register_android_media_MediaExtractor(JNIEnv *env);
extern int register_android_media_MediaPlayer(JNIEnv *env);
extern int register_android_hardware_camera2_utils_SurfaceUtils(JNIEnv *env);
extern int register_android_hardware_display_DisplayService(JNIEnv *env);
extern int register_android_os_MessageQueue(JNIEnv *env);
extern int register_android_os_Trace(JNIEnv *env);
extern int register_com_android_internal_minato_webkit_WebViewChannel(JNIEnv *env);
extern int register_android_os_Debug(JNIEnv *env);
extern int register_android_os_Process(JNIEnv *env);
extern int register_com_android_internal_util_VirtualRefBasePtr(JNIEnv *env);
extern int register_android_hardware_SensorManager(JNIEnv *env);
extern int register_android_hardware_HardwareBuffer(JNIEnv *env);
extern int register_android_hardware_Camera(JNIEnv *env);
// AudioTrack's (gap 13): ours on libaaudio.
extern int register_android_media_AudioTrack(JNIEnv *env);
extern int register_android_media_SoundPool(JNIEnv *env);
extern int register_android_graphics_HardwareRenderer(JNIEnv *env);
extern int register_android_graphics_SurfaceTexture(JNIEnv *env);
extern int register_android_app_ActivityThread(JNIEnv *env);
extern int register_android_animation_PropertyValuesHolder(JNIEnv *env);
extern int register_android_util_EventLog(JNIEnv *env);
extern int register_android_app_Activity(JNIEnv *env);
extern int register_android_app_NativeActivity(JNIEnv *env);
extern int register_android_view_InputQueue(JNIEnv *env);
// KeyEvent's and KeyCharacterMap's (ADR-0045 step 7c1): AOSP's core/jni files over libinput; the input service's, ours,
// for the virtual keyboard's map.
extern int register_android_view_KeyEvent(JNIEnv *env);
extern int register_android_view_KeyCharacterMap(JNIEnv *env);
extern int register_android_hardware_input_InputService(JNIEnv *env);
// MotionEvent's and VelocityTracker's (ADR-0045 step 7b1): AOSP's core/jni files over libinput.
extern int register_android_view_MotionEvent(JNIEnv *env);
extern int register_android_view_VelocityTracker(JNIEnv *env);
// InputChannel's and InputEventReceiver's (ADR-0045 step 7b2): ours on the session's input channel.
extern int register_android_view_InputChannel(JNIEnv *env);
extern int register_android_view_InputEventReceiver(JNIEnv *env);
// The GLES bindings (ADR-0041 a)): AOSP's core/jni files; util.cpp's classes are in namespace android.
extern int register_android_opengl_classes(JNIEnv *env);
// SQLite's (the compatibility plan's gap 7): AOSP's core/jni files over libsqlite.so and libandroidfw's CursorWindow.
extern int register_android_database_CursorWindow(JNIEnv *env);
extern int register_android_database_SQLiteConnection(JNIEnv *env);
extern int register_android_database_SQLiteGlobal(JNIEnv *env);
extern int register_android_database_SQLiteDebug(JNIEnv *env);
extern int register_android_database_SQLiteRawStatement(JNIEnv *env);

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

// As AOSP's: libnativehelper's registration, which aborts when the class or a method is missing.
int AndroidRuntime::registerNativeMethods(JNIEnv *env, const char *className, const JNINativeMethod *gMethods,
                                          int numMethods) {
    return jniRegisterNativeMethods(env, className, gMethods, numMethods);
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
          register_android_content_res_ApkAssets, register_android_os_SystemProperties, register_android_os_Parcel,
          register_android_view_DisplayEventReceiver, register_android_view_Surface,
          register_android_hardware_display_DisplayService,
          register_com_google_android_gles_jni_EGLImpl, register_com_google_android_gles_jni_GLImpl,
          register_android_opengl_jni_EGL14, register_android_opengl_jni_EGL15, register_android_opengl_jni_EGLExt,
          register_android_opengl_jni_GLES10, register_android_opengl_jni_GLES10Ext, register_android_opengl_jni_GLES11,
          register_android_opengl_jni_GLES11Ext, register_android_opengl_jni_GLES20, register_android_opengl_jni_GLES30,
          register_android_opengl_jni_GLES31, register_android_opengl_jni_GLES31Ext, register_android_opengl_jni_GLES32,
          register_android_graphics_classes, register_android_database_CursorWindow,
          register_android_database_SQLiteConnection, register_android_database_SQLiteGlobal,
          register_android_database_SQLiteDebug, register_android_database_SQLiteRawStatement,
          register_android_os_MessageQueue, register_android_os_Trace, register_android_os_Debug,
          register_android_os_Process,
          register_com_android_internal_util_VirtualRefBasePtr,
          register_android_hardware_SensorManager, register_android_hardware_HardwareBuffer,
          register_android_hardware_Camera,
          register_android_media_AudioTrack, register_android_media_SoundPool,
          register_android_graphics_HardwareRenderer, register_android_graphics_SurfaceTexture,
          register_android_view_TextureView, register_android_hardware_SyncFence,
          register_android_media_ImageReader, register_android_media_PublicFormatUtils,
          register_android_media_ImageWriter, register_android_media_MediaCodec, register_android_media_MediaExtractor,
          register_android_media_MediaPlayer,
          register_android_hardware_camera2_utils_SurfaceUtils,
          register_android_opengl_classes,
          register_android_app_ActivityThread, register_android_animation_PropertyValuesHolder,
          register_android_util_EventLog, register_android_app_Activity, register_android_app_NativeActivity,
          register_android_view_KeyEvent,
          register_android_view_KeyCharacterMap, register_android_hardware_input_InputService,
          register_android_view_MotionEvent, register_android_view_VelocityTracker,
          register_android_view_InputChannel, register_android_view_InputEventReceiver,
          register_android_view_InputQueue, register_com_android_internal_minato_webkit_WebViewChannel}) {
        if (registration(env) < 0) {
            env->PopLocalFrame(nullptr);
            return -1;
        }
    }
    env->PopLocalFrame(nullptr);
    return 0;
}

}  // namespace android
