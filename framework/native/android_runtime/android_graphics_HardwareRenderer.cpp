// Ours in place of hwui's jni/android_graphics_HardwareRenderer.cpp, for PixelCopy (the Unity test APK probe and the
// text selection's loupe: android.widget.Magnifier draws its loupe from a PixelCopy of the surface): only
// HardwareRenderer.nCopySurfaceInto, which PixelCopy's requests reach through copySurfaceInto. Android reads the
// surface's last queued buffer; Minato's windows that GL or Vulkan present on a CAMetalLayer keep none, but a window
// drawn by the CPU keeps its last posted software buffer (ADR-0043 decision 3 and its amendment). So nCopySurfaceInto
// finds the window of the Surface, reads a rectangle of that buffer through APKRT_NR_WINDOW_READBACK into the
// CopyRequest's destination bitmap (RGBA_8888), scaled to the bitmap's size, and finishes the request with SUCCESS;
// without a software buffer (never posted, trimmed, a GL or Vulkan window) the readback answers -ENODATA and the
// request finishes with ERROR_SOURCE_NO_DATA, as Readback does for a surface without a queued frame. Android calls it
// on hwui's RenderThread after the call returns; ours calls it before, on the caller's thread, and PixelCopy still
// hands the result to the caller's Handler or Executor. HardwareRenderer's other natives stay unregistered (M4 step
// 10, hwui's GPU pipeline).
#include <android/bitmap.h>
#include <android/native_window.h>
#include <dlfcn.h>
#include <jni.h>
#include <log/log.h>
#include <nativehelper/JNIHelp.h>
#include <unistd.h>

#include <cstdint>

#include "APKRuntime/guest_interface.h"
#include "core_jni_helpers.h"

// android_view_Surface.cpp's: the ANativeWindow of a Java Surface (libandroid's ANativeWindow_fromSurface).
extern "C" ANativeWindow *minato_ANativeWindow_fromSurface(JNIEnv *env, jobject surface);

namespace android {
namespace {

// PixelCopy's results (android.view.PixelCopy): SUCCESS and ERROR_SOURCE_NO_DATA (hwui's CopyResult::SourceEmpty).
constexpr jint kSuccess = 0;
constexpr jint kSourceEmpty = 3;
// PixelFormat.RGBA_8888, the only destination the readback writes.
constexpr std::uint32_t kFormatRgba8888 = 1;

using GetWindowInfo = int (*)(ANativeWindow *window, struct minato_window_info *info);

// minato_ANativeWindow_getInfo gives the host window's ID. It is a private export of the guest's libnativewindow,
// loaded at process start, but it is not in the NDK stub libnativewindow that libandroid_runtime links against, so a
// direct call does not resolve at link time; resolve it by name once instead. AndroidBitmap_* come from libjnigraphics
// (-ljnigraphics), a public NDK library, so they link directly.
GetWindowInfo window_info_fn() {
    static const auto fn = reinterpret_cast<GetWindowInfo>(dlsym(RTLD_DEFAULT, "minato_ANativeWindow_getInfo"));
    return fn;
}

jmethodID gOnCopyFinished;
jfieldID gDestinationBitmap;

void CopySurfaceInto(JNIEnv *env, jclass, jobject surface, jint left, jint top, jint right, jint bottom,
                     jobject copyRequest) {
    jint result = kSourceEmpty;
    ANativeWindow *window = surface != nullptr ? minato_ANativeWindow_fromSurface(env, surface) : nullptr;
    jobject bitmap = env->GetObjectField(copyRequest, gDestinationBitmap);
    const GetWindowInfo get_info = window_info_fn();
    AndroidBitmapInfo info;
    if (window != nullptr && bitmap != nullptr && get_info != nullptr &&
        AndroidBitmap_getInfo(env, bitmap, &info) == ANDROID_BITMAP_RESULT_SUCCESS &&
        info.format == ANDROID_BITMAP_FORMAT_RGBA_8888) {
        void *pixels = nullptr;
        if (AndroidBitmap_lockPixels(env, bitmap, &pixels) == ANDROID_BITMAP_RESULT_SUCCESS) {
            struct minato_window_info window_info = {};
            if (get_info(window, &window_info) == 0) {
                const struct apkrt_window_readback_request request{
                        left, top, right, bottom, info.width, info.height, info.stride, kFormatRgba8888,
                        reinterpret_cast<std::uint64_t>(pixels)};
                if (syscall(APKRT_NR_WINDOW_READBACK, window_info.id,
                            reinterpret_cast<std::uintptr_t>(&request)) == 0) {
                    result = kSuccess;
                }
            }
            AndroidBitmap_unlockPixels(env, bitmap);
        }
    }
    if (window != nullptr) {
        ANativeWindow_release(window);
    }
    if (result != kSuccess) {
        __android_log_print(ANDROID_LOG_WARN, "HWUI",
                            "Surface doesn't have any previously queued frames, nothing to readback from");
    }
    env->CallVoidMethod(copyRequest, gOnCopyFinished, result);
}

const JNINativeMethod gHardwareRendererMethods[] = {
        {"nCopySurfaceInto", "(Landroid/view/Surface;IIIILandroid/graphics/HardwareRenderer$CopyRequest;)V",
         reinterpret_cast<void *>(CopySurfaceInto)},
};

}  // namespace

int register_android_graphics_HardwareRenderer(JNIEnv *env) {
    jclass copyRequest = FindClassOrDie(env, "android/graphics/HardwareRenderer$CopyRequest");
    gOnCopyFinished = GetMethodIDOrDie(env, copyRequest, "onCopyFinished", "(I)V");
    gDestinationBitmap = GetFieldIDOrDie(env, copyRequest, "mDestinationBitmap", "Landroid/graphics/Bitmap;");
    return RegisterMethodsOrDie(env, "android/graphics/HardwareRenderer", gHardwareRendererMethods,
                                NELEM(gHardwareRendererMethods));
}

}  // namespace android
