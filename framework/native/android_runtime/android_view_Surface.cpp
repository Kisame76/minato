// Ours (ADR-0043 c2): Surface's natives, written after AOSP's core/jni/android_view_Surface.cpp at android-15.0.0_r1
// function by function, on libnativewindow's window instead of libgui's Surface (ADR-0041 decision 3). mNativeObject is
// an ANativeWindow*: the Java Surface holds one reference of it, and a lock one more until Java releases it after the
// post, as libgui's sp<Surface> counts them. lockCanvas and unlockCanvasAndPost go through ANativeWindow_lock and
// ANativeWindow_unlockAndPost with hwui's ACanvas, as AOSP's go through Surface::lock and graphics::Canvas. The natives
// that need libgui, Binder or hardware buffers (SurfaceControl, BLASTBufferQueue, Parcel,
// attachAndQueueBuffer) and those our window has nothing for (frame numbers, shared buffer mode, auto refresh, a
// scoped disconnect) log Unwritten's line once and answer what means that nothing happened. Registered at gRegJNI's
// place, with WindowSurface's natives: a Surface on a host window by its ID, for ViewRootImpl's window and a
// SurfaceView's layer. A Surface of a SurfaceTexture holds its queue window (ADR-0058 c)). libnativewindow's private functions (APKRT_PRIVATE) are looked up through dlopen, as the NDK's
// stub that the library links against lacks them. For libandroid's ANativeWindow_fromSurface and
// ANativeWindow_toSurface, which Android's libandroid builds on libandroid_runtime's
// android_view_Surface_getNativeWindow and android_view_Surface_createFromSurface, it exports
// minato_ANativeWindow_fromSurface and minato_ANativeWindow_toSurface.
#define LOG_TAG "Surface"

#include <android/data_space.h>
#include <android/graphics/canvas.h>
#include <android/native_window.h>
#include <dlfcn.h>
#include <errno.h>
#include <jni.h>
#include <nativehelper/JNIHelp.h>

#include <APKRuntime/guest_interface.h>
#include <log/log.h>

#include "core_jni_helpers.h"
#include "minato_gles.h"

namespace android {

ANativeWindow *android_SurfaceTexture_getProducer(JNIEnv *env, jobject surfaceTexture);

namespace {

const char *const kIllegalArgumentException = "java/lang/IllegalArgumentException";
const char *const kOutOfResourcesException = "android/view/Surface$OutOfResourcesException";

// libgui's scaling modes (system/window.h): FREEZE, SCALE_TO_WINDOW, SCALE_CROP and NO_SCALE_CROP, 0 to 3.
constexpr jint kScalingModes = 4;

struct {
    jclass clazz;
    jfieldID mNativeObject;
    jfieldID mLock;
    jmethodID ctor;
} gSurfaceClassInfo;

struct {
    jfieldID left;
    jfieldID top;
    jfieldID right;
    jfieldID bottom;
} gRectClassInfo;

// libnativewindow's private functions, looked up once.
struct Private {
    decltype(&minato_ANativeWindow_create) create = nullptr;
    decltype(&minato_ANativeWindow_getInfo) getInfo = nullptr;
};

const Private &private_functions() {
    static const Private functions = [] {
        Private found;
        if (void *library = dlopen("libnativewindow.so", RTLD_NOW)) {
            found.create = reinterpret_cast<decltype(found.create)>(dlsym(library, "minato_ANativeWindow_create"));
            found.getInfo = reinterpret_cast<decltype(found.getInfo)>(dlsym(library, "minato_ANativeWindow_getInfo"));
        }
        LOG_ALWAYS_FATAL_IF(found.create == nullptr || found.getInfo == nullptr,
                            "libnativewindow.so lacks minato_ANativeWindow_create or minato_ANativeWindow_getInfo");
        return found;
    }();
    return functions;
}

ANativeWindow *window_of(jlong nativeObject) {
    return reinterpret_cast<ANativeWindow *>(nativeObject);
}

// Surface::isValid: a window that lives, which libnativewindow tells from anything else.
bool is_valid(ANativeWindow *window) {
    minato_window_info info;
    return window != nullptr && private_functions().getInfo(window, &info) == 0;
}

// The SurfaceTexture's producer, its queue window (android_graphics_SurfaceTexture.cpp), with a reference of the
// Surface's own; the tag's OutOfResourcesException for a SurfaceTexture without one.
jlong nativeCreateFromSurfaceTexture(JNIEnv *env, jclass, jobject surfaceTexture) {
    ANativeWindow *window = android_SurfaceTexture_getProducer(env, surfaceTexture);
    if (window == nullptr) {
        jniThrowException(env, kOutOfResourcesException,
                          "SurfaceTexture has already been released");
        return 0;
    }
    ANativeWindow_acquire(window);
    return reinterpret_cast<jlong>(window);
}

void nativeRelease(JNIEnv *, jclass, jlong nativeObject) {
    ANativeWindow_release(window_of(nativeObject));
}

jboolean nativeIsValid(JNIEnv *, jclass, jlong nativeObject) {
    return is_valid(window_of(nativeObject)) ? JNI_TRUE : JNI_FALSE;
}

// No queue lies between the app and the host, which takes each post at once.
jboolean nativeIsConsumerRunningBehind(JNIEnv *env, jclass, jlong nativeObject) {
    if (!is_valid(window_of(nativeObject))) {
        jniThrowException(env, kIllegalArgumentException, nullptr);
    }
    return JNI_FALSE;
}

jlong nativeLockCanvas(JNIEnv *env, jclass, jlong nativeObject, jobject canvasObj, jobject dirtyRectObj) {
    ANativeWindow *window = window_of(nativeObject);
    if (!is_valid(window)) {
        jniThrowException(env, kIllegalArgumentException, nullptr);
        return 0;
    }
    if (!ACanvas_isSupportedPixelFormat(ANativeWindow_getFormat(window))) {
        // native_window_set_buffers_format: the format alone, the geometry's size kept.
        minato_window_info info;
        private_functions().getInfo(window, &info);
        ANativeWindow_setBuffersGeometry(window, info.buffer_width, info.buffer_height, WINDOW_FORMAT_RGBA_8888);
    }

    ARect dirtyRect = {};
    ARect *dirtyRectPtr = nullptr;
    if (dirtyRectObj) {
        dirtyRect.left = env->GetIntField(dirtyRectObj, gRectClassInfo.left);
        dirtyRect.top = env->GetIntField(dirtyRectObj, gRectClassInfo.top);
        dirtyRect.right = env->GetIntField(dirtyRectObj, gRectClassInfo.right);
        dirtyRect.bottom = env->GetIntField(dirtyRectObj, gRectClassInfo.bottom);
        dirtyRectPtr = &dirtyRect;
    }

    ANativeWindow_Buffer buffer;
    const int32_t err = ANativeWindow_lock(window, &buffer, dirtyRectPtr);
    if (err < 0) {
        jniThrowException(env, err == -ENOMEM ? kOutOfResourcesException : kIllegalArgumentException, nullptr);
        return 0;
    }

    graphics::Canvas canvas(env, canvasObj);
    canvas.setBuffer(&buffer, ANativeWindow_getBuffersDataSpace(window));

    if (dirtyRectPtr) {
        canvas.clipRect({dirtyRect.left, dirtyRect.top, dirtyRect.right, dirtyRect.bottom});
    }

    if (dirtyRectObj) {
        env->SetIntField(dirtyRectObj, gRectClassInfo.left, dirtyRect.left);
        env->SetIntField(dirtyRectObj, gRectClassInfo.top, dirtyRect.top);
        env->SetIntField(dirtyRectObj, gRectClassInfo.right, dirtyRect.right);
        env->SetIntField(dirtyRectObj, gRectClassInfo.bottom, dirtyRect.bottom);
    }

    // Another reference, which Java passes to nativeUnlockCanvasAndPost in place of mNativeObject, since that could be
    // replaced while the surface is locked, and releases after the post.
    ANativeWindow_acquire(window);
    return nativeObject;
}

void nativeUnlockCanvasAndPost(JNIEnv *env, jclass, jlong nativeObject, jobject canvasObj) {
    ANativeWindow *window = window_of(nativeObject);
    if (!is_valid(window)) {
        return;
    }

    // Detach the canvas from the window's buffer.
    graphics::Canvas canvas(env, canvasObj);
    canvas.setBuffer(nullptr, ADATASPACE_UNKNOWN);

    if (ANativeWindow_unlockAndPost(window) < 0) {
        jniThrowException(env, kIllegalArgumentException, nullptr);
    }
}

void nativeAllocateBuffers(JNIEnv *, jclass, jlong nativeObject) {
    ANativeWindow *window = window_of(nativeObject);
    if (!is_valid(window)) {
        return;
    }
    ANativeWindow_tryAllocateBuffers(window);
}

jlong nativeCreateFromSurfaceControl(JNIEnv *env, jclass, jlong) {
    minato_unwritten(env, "android.view.Surface.createFrom(SurfaceControl)");
    return 0;
}

// The Surface keeps the window it has.
jlong nativeGetFromSurfaceControl(JNIEnv *env, jclass, jlong nativeObject, jlong) {
    minato_unwritten(env, "android.view.Surface.copyFrom(SurfaceControl)");
    return nativeObject;
}

jlong nativeGetFromBlastBufferQueue(JNIEnv *env, jclass, jlong nativeObject, jlong) {
    minato_unwritten(env, "android.view.Surface.copyFrom(BLASTBufferQueue)");
    return nativeObject;
}

// No Binder carries a Surface to another process, and within the process the Parcel carries no window.
jlong nativeReadFromParcel(JNIEnv *env, jclass, jlong nativeObject, jobject) {
    minato_unwritten(env, "android.view.Surface.readFromParcel(Parcel)");
    return nativeObject;
}

void nativeWriteToParcel(JNIEnv *env, jclass, jlong, jobject) {
    minato_unwritten(env, "android.view.Surface.writeToParcel(Parcel, int)");
}

jint nativeGetWidth(JNIEnv *, jclass, jlong nativeObject) {
    return ANativeWindow_getWidth(window_of(nativeObject));
}

jint nativeGetHeight(JNIEnv *, jclass, jlong nativeObject) {
    return ANativeWindow_getHeight(window_of(nativeObject));
}

jlong nativeGetNextFrameNumber(JNIEnv *env, jclass, jlong) {
    minato_unwritten(env, "android.view.Surface.getNextFrameNumber()");
    return 1;
}

// Surface::setScalingMode's check; the host scales a buffer of another size to the layer in every mode.
jint nativeSetScalingMode(JNIEnv *, jclass, jlong, jint scalingMode) {
    return scalingMode >= 0 && scalingMode < kScalingModes ? 0 : -EINVAL;
}

jint nativeForceScopedDisconnect(JNIEnv *env, jclass, jlong) {
    minato_unwritten(env, "android.view.Surface.forceScopedDisconnect()");
    return 0;
}

jint nativeAttachAndQueueBufferWithColorSpace(JNIEnv *env, jclass, jlong, jobject, jint) {
    minato_unwritten(env, "android.view.Surface.attachAndQueueBufferWithColorSpace(HardwareBuffer, ColorSpace)");
    return -EINVAL;
}

jint nativeSetSharedBufferModeEnabled(JNIEnv *env, jclass, jlong, jboolean) {
    minato_unwritten(env, "android.view.Surface.setSharedBufferModeEnabled(boolean)");
    return 0;
}

jint nativeSetAutoRefreshEnabled(JNIEnv *env, jclass, jlong, jboolean) {
    minato_unwritten(env, "android.view.Surface.setAutoRefreshEnabled(boolean)");
    return 0;
}

// Surface's compatibility is ANativeWindow's ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_*, the same values.
jint nativeSetFrameRate(JNIEnv *, jclass, jlong nativeObject, jfloat frameRate, jint compatibility,
                        jint changeFrameRateStrategy) {
    return ANativeWindow_setFrameRateWithChangeStrategy(window_of(nativeObject), frameRate,
                                                        static_cast<int8_t>(compatibility),
                                                        static_cast<int8_t>(changeFrameRateStrategy));
}

// Java's destroy releases the Surface's reference next; the host's window is the host's.
void nativeDestroy(JNIEnv *, jclass, jlong) {}

jobject windowSurfaceCreate(JNIEnv *env, jclass, jlong window, jint width, jint height) {
    ANativeWindow *created = private_functions().create(static_cast<uint64_t>(window), width, height);
    if (created == nullptr) {
        jniThrowException(env, kOutOfResourcesException, nullptr);
        return nullptr;
    }
    jobject surface = env->NewObject(gSurfaceClassInfo.clazz, gSurfaceClassInfo.ctor, reinterpret_cast<jlong>(created));
    if (surface == nullptr) {
        ANativeWindow_release(created);  // the constructor's exception stays pending
    }
    return surface;
}

jint windowSurfaceSetBuffersGeometry(JNIEnv *env, jclass, jobject surface, jint width, jint height, jint format) {
    ANativeWindow *window = minato_surface_window(env, surface);
    if (window == nullptr) {
        jniThrowException(env, kIllegalArgumentException, "the Surface is released");
        return -EINVAL;
    }
    const int32_t result = ANativeWindow_setBuffersGeometry(window, width, height, format);
    ANativeWindow_release(window);
    return result;
}

const JNINativeMethod gSurfaceMethods[] = {
        {"nativeCreateFromSurfaceTexture", "(Landroid/graphics/SurfaceTexture;)J",
         (void *)nativeCreateFromSurfaceTexture},
        {"nativeRelease", "(J)V", (void *)nativeRelease},
        {"nativeIsValid", "(J)Z", (void *)nativeIsValid},
        {"nativeIsConsumerRunningBehind", "(J)Z", (void *)nativeIsConsumerRunningBehind},
        {"nativeLockCanvas", "(JLandroid/graphics/Canvas;Landroid/graphics/Rect;)J", (void *)nativeLockCanvas},
        {"nativeUnlockCanvasAndPost", "(JLandroid/graphics/Canvas;)V", (void *)nativeUnlockCanvasAndPost},
        {"nativeAllocateBuffers", "(J)V", (void *)nativeAllocateBuffers},
        {"nativeCreateFromSurfaceControl", "(J)J", (void *)nativeCreateFromSurfaceControl},
        {"nativeGetFromSurfaceControl", "(JJ)J", (void *)nativeGetFromSurfaceControl},
        {"nativeReadFromParcel", "(JLandroid/os/Parcel;)J", (void *)nativeReadFromParcel},
        {"nativeWriteToParcel", "(JLandroid/os/Parcel;)V", (void *)nativeWriteToParcel},
        {"nativeGetWidth", "(J)I", (void *)nativeGetWidth},
        {"nativeGetHeight", "(J)I", (void *)nativeGetHeight},
        {"nativeGetNextFrameNumber", "(J)J", (void *)nativeGetNextFrameNumber},
        {"nativeSetScalingMode", "(JI)I", (void *)nativeSetScalingMode},
        {"nativeForceScopedDisconnect", "(J)I", (void *)nativeForceScopedDisconnect},
        {"nativeAttachAndQueueBufferWithColorSpace", "(JLandroid/hardware/HardwareBuffer;I)I",
         (void *)nativeAttachAndQueueBufferWithColorSpace},
        {"nativeSetSharedBufferModeEnabled", "(JZ)I", (void *)nativeSetSharedBufferModeEnabled},
        {"nativeSetAutoRefreshEnabled", "(JZ)I", (void *)nativeSetAutoRefreshEnabled},
        {"nativeSetFrameRate", "(JFII)I", (void *)nativeSetFrameRate},
        {"nativeGetFromBlastBufferQueue", "(JJ)J", (void *)nativeGetFromBlastBufferQueue},
        {"nativeDestroy", "(J)V", (void *)nativeDestroy},
};

const JNINativeMethod gWindowSurfaceMethods[] = {
        {"create", "(JII)Landroid/view/Surface;", (void *)windowSurfaceCreate},
        {"setBuffersGeometry", "(Landroid/view/Surface;III)I", (void *)windowSurfaceSetBuffersGeometry},
};

}  // namespace

ANativeWindow *minato_surface_window(JNIEnv *env, jobject surface) {
    if (surface == nullptr || !env->IsInstanceOf(surface, gSurfaceClassInfo.clazz)) {
        return nullptr;
    }
    ANativeWindow *window = nullptr;
    jobject lock = env->GetObjectField(surface, gSurfaceClassInfo.mLock);
    if (env->MonitorEnter(lock) == JNI_OK) {
        window = window_of(env->GetLongField(surface, gSurfaceClassInfo.mNativeObject));
        if (window != nullptr) {
            ANativeWindow_acquire(window);
        }
        env->MonitorExit(lock);
    }
    env->DeleteLocalRef(lock);
    return window;
}

int register_android_view_Surface(JNIEnv *env) {
    int err = RegisterMethodsOrDie(env, "android/view/Surface", gSurfaceMethods, NELEM(gSurfaceMethods));

    jclass clazz = FindClassOrDie(env, "android/view/Surface");
    gSurfaceClassInfo.clazz = MakeGlobalRefOrDie(env, clazz);
    gSurfaceClassInfo.mNativeObject = GetFieldIDOrDie(env, gSurfaceClassInfo.clazz, "mNativeObject", "J");
    gSurfaceClassInfo.mLock = GetFieldIDOrDie(env, gSurfaceClassInfo.clazz, "mLock", "Ljava/lang/Object;");
    gSurfaceClassInfo.ctor = GetMethodIDOrDie(env, gSurfaceClassInfo.clazz, "<init>", "(J)V");

    clazz = FindClassOrDie(env, "android/graphics/Rect");
    gRectClassInfo.left = GetFieldIDOrDie(env, clazz, "left", "I");
    gRectClassInfo.top = GetFieldIDOrDie(env, clazz, "top", "I");
    gRectClassInfo.right = GetFieldIDOrDie(env, clazz, "right", "I");
    gRectClassInfo.bottom = GetFieldIDOrDie(env, clazz, "bottom", "I");

    RegisterMethodsOrDie(env, "android/view/WindowSurface", gWindowSurfaceMethods,
                         NELEM(gWindowSurfaceMethods));
    return err;
}

}  // namespace android

// For libandroid's ANativeWindow_fromSurface: the window of a Java Surface, acquired for the caller; null for a
// released Surface or another object.
extern "C" ANativeWindow *minato_ANativeWindow_fromSurface(JNIEnv *env, jobject surface) {
    return android::minato_surface_window(env, surface);
}

// For libandroid's ANativeWindow_toSurface: a new Java Surface of the window, holding a reference of its own, as
// android_view_Surface_createFromSurface makes one; null, with the exception cleared, if it can't be made.
extern "C" jobject minato_ANativeWindow_toSurface(JNIEnv *env, ANativeWindow *window) {
    if (window == nullptr) {
        return nullptr;
    }
    jobject surface = env->NewObject(android::gSurfaceClassInfo.clazz, android::gSurfaceClassInfo.ctor,
                                     reinterpret_cast<jlong>(window));
    if (surface == nullptr) {
        if (env->ExceptionCheck()) {
            ALOGE("Could not create instance of Surface from ANativeWindow.");
            env->ExceptionClear();
        }
        return nullptr;
    }
    ANativeWindow_acquire(window);
    return surface;
}
