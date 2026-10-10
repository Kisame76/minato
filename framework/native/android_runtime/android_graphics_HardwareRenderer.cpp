// Ours in place of hwui's jni/android_graphics_HardwareRenderer.cpp, for PixelCopy (the Unity test APK probe: Unity's pause
// path): only HardwareRenderer.nCopySurfaceInto, which PixelCopy's requests reach through copySurfaceInto. hwui's
// Readback takes the surface's last queued buffer, which Minato's surfaces, presented by ANGLE or MoltenVK on a
// CAMetalLayer, do not keep, so every copy answers as Readback does for a surface without a queued frame: its warning
// and CopyRequest.onCopyFinished(ERROR_SOURCE_NO_DATA). Android calls it on hwui's RenderThread after the call
// returns; ours calls it before, on the caller's thread, and PixelCopy still hands the result to the caller's Handler
// or Executor. HardwareRenderer's other natives stay unregistered (M4 step 10, hwui's GPU pipeline).
#include <jni.h>
#include <log/log.h>
#include <nativehelper/JNIHelp.h>

#include "core_jni_helpers.h"

namespace android {
namespace {

// PixelCopy.ERROR_SOURCE_NO_DATA, hwui's CopyResult::SourceEmpty.
constexpr jint kSourceEmpty = 3;

jmethodID gOnCopyFinished;

void CopySurfaceInto(JNIEnv *env, jclass, jobject, jint, jint, jint, jint, jobject copyRequest) {
    __android_log_print(ANDROID_LOG_WARN, "HWUI",
                        "Surface doesn't have any previously queued frames, nothing to readback from");
    env->CallVoidMethod(copyRequest, gOnCopyFinished, kSourceEmpty);
}

const JNINativeMethod gHardwareRendererMethods[] = {
        {"nCopySurfaceInto", "(Landroid/view/Surface;IIIILandroid/graphics/HardwareRenderer$CopyRequest;)V",
         reinterpret_cast<void *>(CopySurfaceInto)},
};

}  // namespace

int register_android_graphics_HardwareRenderer(JNIEnv *env) {
    jclass copyRequest = FindClassOrDie(env, "android/graphics/HardwareRenderer$CopyRequest");
    gOnCopyFinished = GetMethodIDOrDie(env, copyRequest, "onCopyFinished", "(I)V");
    return RegisterMethodsOrDie(env, "android/graphics/HardwareRenderer", gHardwareRendererMethods,
                                NELEM(gHardwareRendererMethods));
}

}  // namespace android
