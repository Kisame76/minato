// Ours in place of AOSP's core/jni/android_view_TextureView.cpp (ADR-0058 d)): TextureView's natives on the queue
// window of its SurfaceTexture instead of a libgui Surface. mNativeWindow is that window with a reference of the
// TextureView's own; lockCanvas and unlockCanvasAndPost go through ANativeWindow_lock and ANativeWindow_unlockAndPost
// with hwui's ACanvas, as AOSP's go through the window's perform and graphics::Canvas. One more native is Minato's,
// for the layer of a TextureView in a window drawn in software (framework/patches/textureview-software-layer.patch):
// nMinatoLatch copies the SurfaceTexture's newest frame into a Bitmap on the CPU, where hwui's DeferredLayerUpdater
// binds it to a texture on the GPU, a named mechanism difference (a copy per frame).
#define LOG_TAG "TextureView"

#include <android/data_space.h>
#include <android/graphics/bitmap.h>
#include <android/graphics/canvas.h>
#include <android/hardware_buffer.h>
#include <android/native_window.h>
#include <jni.h>
#include <nativehelper/JNIHelp.h>
#include <stdint.h>
#include <string.h>

#include <log/log.h>

#include "core_jni_helpers.h"
#include "minato_gles.h"

namespace android {
namespace {

const char *const kClassPathName = "android/view/TextureView";

struct {
    jfieldID left;
    jfieldID top;
    jfieldID right;
    jfieldID bottom;
    jmethodID set;
} gRectClassInfo;

struct {
    jfieldID nativeWindow;
} gTextureViewClassInfo;

struct {
    jclass bitmap;  // global
    jmethodID createBitmap;
    jobject argb8888;  // global
    jobject rgb565;    // global
} gBitmapClassInfo;

void TextureView_createNativeWindow(JNIEnv *env, jobject textureView, jobject surface) {
    ANativeWindow *window = minato_surface_texture_window(env, surface);
    env->SetLongField(textureView, gTextureViewClassInfo.nativeWindow, reinterpret_cast<jlong>(window));
}

void TextureView_destroyNativeWindow(JNIEnv *env, jobject textureView) {
    auto *window = reinterpret_cast<ANativeWindow *>(env->GetLongField(textureView,
                                                                       gTextureViewClassInfo.nativeWindow));
    if (window != nullptr) {
        ANativeWindow_release(window);
        env->SetLongField(textureView, gTextureViewClassInfo.nativeWindow, 0);
    }
}

jboolean TextureView_lockCanvas(JNIEnv *env, jclass, jlong nativeWindow, jobject canvasObj, jobject dirtyRect) {
    auto *window = reinterpret_cast<ANativeWindow *>(nativeWindow);
    if (window == nullptr) return JNI_FALSE;
    // As AOSP's: without a dirty rectangle, all of the buffer.
    ARect rect = {0, 0, 0x3FFF, 0x3FFF};
    if (dirtyRect != nullptr) {
        rect.left = env->GetIntField(dirtyRect, gRectClassInfo.left);
        rect.top = env->GetIntField(dirtyRect, gRectClassInfo.top);
        rect.right = env->GetIntField(dirtyRect, gRectClassInfo.right);
        rect.bottom = env->GetIntField(dirtyRect, gRectClassInfo.bottom);
    }
    ANativeWindow_Buffer buffer;
    if (ANativeWindow_lock(window, &buffer, &rect) != 0) return JNI_FALSE;

    graphics::Canvas canvas(env, canvasObj);
    canvas.setBuffer(&buffer, ANativeWindow_getBuffersDataSpace(window));
    canvas.clipRect({rect.left, rect.top, rect.right, rect.bottom});
    if (dirtyRect != nullptr) {
        env->CallVoidMethod(dirtyRect, gRectClassInfo.set, rect.left, rect.top, rect.right, rect.bottom);
    }
    return JNI_TRUE;
}

void TextureView_unlockCanvasAndPost(JNIEnv *env, jclass, jlong nativeWindow, jobject canvasObj) {
    graphics::Canvas canvas(env, canvasObj);
    canvas.setBuffer(nullptr, ADATASPACE_UNKNOWN);
    auto *window = reinterpret_cast<ANativeWindow *>(nativeWindow);
    if (window != nullptr) ANativeWindow_unlockAndPost(window);
}

// The Bitmap config that holds a buffer of `format` as it is, or null for a format the copy does not take.
jobject config_of(uint32_t format) {
    switch (format) {
        case AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM:
        case AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM:
            return gBitmapClassInfo.argb8888;
        case AHARDWAREBUFFER_FORMAT_R5G6B5_UNORM:
            return gBitmapClassInfo.rgb565;
        default:
            return nullptr;
    }
}

// Copies `buffer`'s rows into `pixels` with `stride` bytes per row: RGBX's fourth byte becomes an opaque alpha.
bool copy_rows(AHardwareBuffer *buffer, const AHardwareBuffer_Desc &desc, uint8_t *pixels, uint32_t stride) {
    void *data = nullptr;
    if (AHardwareBuffer_lock(buffer, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, nullptr, &data) != 0) return false;
    const uint32_t bytes = desc.format == AHARDWAREBUFFER_FORMAT_R5G6B5_UNORM ? 2 : 4;
    const auto *source = static_cast<const uint8_t *>(data);
    for (uint32_t row = 0; row < desc.height; row++) {
        uint8_t *line = pixels + row * stride;
        memcpy(line, source + row * desc.stride * bytes, desc.width * bytes);
        if (desc.format == AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM) {
            for (uint32_t x = 0; x < desc.width; x++) line[x * 4 + 3] = 0xff;
        }
    }
    AHardwareBuffer_unlock(buffer, nullptr);
    return true;
}

// The SurfaceTexture's current frame in a Bitmap: `frame` again, refilled with a new frame when its size and config
// fit, else a new Bitmap; `frame` unchanged without a new frame; null without any. A frame of a format the copy does
// not take (not RGBA_8888, RGBX_8888 or RGB_565) leaves `frame` as it is, logged once.
jobject TextureView_minatoLatch(JNIEnv *env, jclass, jobject surfaceTexture, jobject frame) {
    bool fresh = false;
    AHardwareBuffer *buffer = minato_surface_texture_latch(env, surfaceTexture, &fresh);
    if (buffer == nullptr) return frame;
    if (!fresh && frame != nullptr) {
        AHardwareBuffer_release(buffer);
        return frame;
    }
    AHardwareBuffer_Desc desc;
    AHardwareBuffer_describe(buffer, &desc);
    jobject config = config_of(desc.format);
    if (config == nullptr) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            ALOGW("a TextureView in a window drawn in software shows no frame of format %u", desc.format);
        }
        AHardwareBuffer_release(buffer);
        return frame;
    }
    jobject target = frame;
    if (target != nullptr) {
        const AndroidBitmapInfo info = ABitmap_getInfoFromJava(env, target);
        const int32_t wanted = desc.format == AHARDWAREBUFFER_FORMAT_R5G6B5_UNORM ? ANDROID_BITMAP_FORMAT_RGB_565
                                                                                  : ANDROID_BITMAP_FORMAT_RGBA_8888;
        if (info.width != desc.width || info.height != desc.height || info.format != wanted) target = nullptr;
    }
    if (target == nullptr) {
        target = env->CallStaticObjectMethod(gBitmapClassInfo.bitmap, gBitmapClassInfo.createBitmap,
                                             static_cast<jint>(desc.width), static_cast<jint>(desc.height), config);
        if (env->ExceptionCheck() || target == nullptr) {
            // No Bitmap for the frame (out of memory): the last one stays, as no allocation fails in hwui's draw.
            ALOGE("no Bitmap of %u x %u for a TextureView's frame", desc.width, desc.height);
            env->ExceptionDescribe();
            env->ExceptionClear();
            AHardwareBuffer_release(buffer);
            return frame;
        }
    }
    graphics::Bitmap bitmap(env, target);
    if (bitmap.isValid() && bitmap.getPixels() != nullptr) {
        if (copy_rows(buffer, desc, static_cast<uint8_t *>(bitmap.getPixels()), bitmap.getInfo().stride)) {
            bitmap.notifyPixelsChanged();
        } else {
            ALOGE("a TextureView's frame could not be locked for its copy");
        }
    }
    AHardwareBuffer_release(buffer);
    return target;
}

const JNINativeMethod gMethods[] = {
        {"nCreateNativeWindow", "(Landroid/graphics/SurfaceTexture;)V", (void *)TextureView_createNativeWindow},
        {"nDestroyNativeWindow", "()V", (void *)TextureView_destroyNativeWindow},
        {"nLockCanvas", "(JLandroid/graphics/Canvas;Landroid/graphics/Rect;)Z", (void *)TextureView_lockCanvas},
        {"nUnlockCanvasAndPost", "(JLandroid/graphics/Canvas;)V", (void *)TextureView_unlockCanvasAndPost},
        {"nMinatoLatch", "(Landroid/graphics/SurfaceTexture;Landroid/graphics/Bitmap;)Landroid/graphics/Bitmap;",
         (void *)TextureView_minatoLatch},
};

}  // namespace

int register_android_view_TextureView(JNIEnv *env) {
    jclass clazz = FindClassOrDie(env, "android/graphics/Rect");
    gRectClassInfo.set = GetMethodIDOrDie(env, clazz, "set", "(IIII)V");
    gRectClassInfo.left = GetFieldIDOrDie(env, clazz, "left", "I");
    gRectClassInfo.top = GetFieldIDOrDie(env, clazz, "top", "I");
    gRectClassInfo.right = GetFieldIDOrDie(env, clazz, "right", "I");
    gRectClassInfo.bottom = GetFieldIDOrDie(env, clazz, "bottom", "I");

    clazz = FindClassOrDie(env, "android/graphics/Bitmap");
    gBitmapClassInfo.bitmap = MakeGlobalRefOrDie(env, clazz);
    gBitmapClassInfo.createBitmap =
            GetStaticMethodIDOrDie(env, clazz, "createBitmap",
                                   "(IILandroid/graphics/Bitmap$Config;)Landroid/graphics/Bitmap;");
    jclass config = FindClassOrDie(env, "android/graphics/Bitmap$Config");
    gBitmapClassInfo.argb8888 = MakeGlobalRefOrDie(
            env, env->GetStaticObjectField(config, GetStaticFieldIDOrDie(env, config, "ARGB_8888",
                                                                         "Landroid/graphics/Bitmap$Config;")));
    gBitmapClassInfo.rgb565 = MakeGlobalRefOrDie(
            env, env->GetStaticObjectField(config, GetStaticFieldIDOrDie(env, config, "RGB_565",
                                                                         "Landroid/graphics/Bitmap$Config;")));

    clazz = FindClassOrDie(env, kClassPathName);
    gTextureViewClassInfo.nativeWindow = GetFieldIDOrDie(env, clazz, "mNativeWindow", "J");
    return RegisterMethodsOrDie(env, kClassPathName, gMethods, NELEM(gMethods));
}

}  // namespace android
