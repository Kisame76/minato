// Ours in place of AOSP's SurfaceUtils natives in core/jni (the graphics breadth's G3b, which ImageWriter's
// newInstance(Surface, int) reaches): what a Surface's window answers, over libnativewindow's ANativeWindow instead of
// libgui's Surface queries. A released Surface answers BAD_VALUE, which SurfaceUtils turns into
// IllegalArgumentException; the consumer's usage, which the queue does not tell its producers, is 0; a Surface's id is
// its window's address.
#define LOG_TAG "SurfaceUtils-JNI"

#include <android/native_window.h>
#include <errno.h>
#include <jni.h>
#include <nativehelper/JNIHelp.h>
#include <stdint.h>

#include "core_jni_helpers.h"

extern "C" ANativeWindow *minato_ANativeWindow_fromSurface(JNIEnv *env, jobject surface);

namespace android {
namespace {

constexpr jint kBadValue = -EINVAL;  // utils/Errors.h's BAD_VALUE, SurfaceUtils.BAD_VALUE

// The Surface's window with a reference of its own, or null for a released Surface.
ANativeWindow *window_of(JNIEnv *env, jobject surface) {
    return surface != nullptr ? minato_ANativeWindow_fromSurface(env, surface) : nullptr;
}

jint SurfaceUtils_detectSurfaceType(JNIEnv *env, jclass, jobject surface) {
    ANativeWindow *window = window_of(env, surface);
    if (window == nullptr) return kBadValue;
    const jint format = ANativeWindow_getFormat(window);
    ANativeWindow_release(window);
    return format;
}

jint SurfaceUtils_detectSurfaceDataspace(JNIEnv *env, jclass, jobject surface) {
    ANativeWindow *window = window_of(env, surface);
    if (window == nullptr) return kBadValue;
    const jint dataSpace = ANativeWindow_getBuffersDataSpace(window);
    ANativeWindow_release(window);
    return dataSpace;
}

jlong SurfaceUtils_detectSurfaceUsageFlags(JNIEnv *env, jclass, jobject surface) {
    ANativeWindow *window = window_of(env, surface);
    if (window == nullptr) {
        jniThrowException(env, "java/lang/UnsupportedOperationException",
                          "Could not retrieve native window from surface.");
        return 0;
    }
    ANativeWindow_release(window);
    return 0;
}

jint SurfaceUtils_detectSurfaceDimens(JNIEnv *env, jclass, jobject surface, jintArray dimens) {
    if (dimens == nullptr || env->GetArrayLength(dimens) < 2) {
        jniThrowException(env, "java/lang/IllegalArgumentException", "dimens must be an array of size 2");
        return kBadValue;
    }
    ANativeWindow *window = window_of(env, surface);
    if (window == nullptr) return kBadValue;
    const jint size[2] = {ANativeWindow_getWidth(window), ANativeWindow_getHeight(window)};
    ANativeWindow_release(window);
    env->SetIntArrayRegion(dimens, 0, 2, size);
    return 0;
}

jlong SurfaceUtils_getSurfaceId(JNIEnv *env, jclass, jobject surface) {
    ANativeWindow *window = window_of(env, surface);
    if (window == nullptr) return 0;
    ANativeWindow_release(window);
    return static_cast<jlong>(reinterpret_cast<uintptr_t>(window));
}

const JNINativeMethod gMethods[] = {
        {"nativeDetectSurfaceType", "(Landroid/view/Surface;)I", (void *)SurfaceUtils_detectSurfaceType},
        {"nativeDetectSurfaceDataspace", "(Landroid/view/Surface;)I", (void *)SurfaceUtils_detectSurfaceDataspace},
        {"nativeDetectSurfaceUsageFlags", "(Landroid/view/Surface;)J", (void *)SurfaceUtils_detectSurfaceUsageFlags},
        {"nativeDetectSurfaceDimens", "(Landroid/view/Surface;[I)I", (void *)SurfaceUtils_detectSurfaceDimens},
        {"nativeGetSurfaceId", "(Landroid/view/Surface;)J", (void *)SurfaceUtils_getSurfaceId},
};

}  // namespace

int register_android_hardware_camera2_utils_SurfaceUtils(JNIEnv *env) {
    return RegisterMethodsOrDie(env, "android/hardware/camera2/utils/SurfaceUtils", gMethods, NELEM(gMethods));
}

}  // namespace android
