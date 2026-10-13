// libandroid.so's ASurfaceTexture (ADR-0058 c)), as hardware_buffer_jni.cpp's AHardwareBuffer_fromHardwareBuffer:
// Android's libandroid builds it on libandroid_runtime's SurfaceTexture; ours calls the functions that SurfaceTexture's
// natives in libandroid_runtime export for it (minato_ASurfaceTexture_*), looked up through dlopen at the first call,
// so that libandroid needs no libandroid_runtime where there is no Java. There fromSurfaceTexture answers NULL and logs
// once. An ASurfaceTexture is the SurfaceTexture's consumer with a reference of its own, which outlives the Java
// object until ASurfaceTexture_release, as AOSP's holds the native SurfaceTexture and its producer.
#include <android/log.h>
#include <android/native_window.h>
#include <android/surface_texture.h>
#include <dlfcn.h>
#include <jni.h>
#include <pthread.h>
#include <stdint.h>

namespace {

struct Functions {
    void* (*from_java)(JNIEnv*, jobject);
    void (*release)(void*);
    ANativeWindow* (*acquire_window)(void*);
    int (*attach)(void*, uint32_t);
    int (*detach)(void*);
    int (*update)(void*);
    void (*matrix)(void*, float*);
    int64_t (*timestamp)(void*);
};
Functions functions;
pthread_once_t looked_up = PTHREAD_ONCE_INIT;

void look_up() {
    if (void* library = dlopen("libandroid_runtime.so", RTLD_NOW)) {
#define FIND(field, name) functions.field = reinterpret_cast<decltype(functions.field)>(dlsym(library, #name))
        FIND(from_java, minato_ASurfaceTexture_fromSurfaceTexture);
        FIND(release, minato_ASurfaceTexture_release);
        FIND(acquire_window, minato_ASurfaceTexture_acquireANativeWindow);
        FIND(attach, minato_ASurfaceTexture_attachToGLContext);
        FIND(detach, minato_ASurfaceTexture_detachFromGLContext);
        FIND(update, minato_ASurfaceTexture_updateTexImage);
        FIND(matrix, minato_ASurfaceTexture_getTransformMatrix);
        FIND(timestamp, minato_ASurfaceTexture_getTimestamp);
#undef FIND
    }
    if (functions.from_java == nullptr || functions.release == nullptr || functions.acquire_window == nullptr ||
        functions.attach == nullptr || functions.detach == nullptr || functions.update == nullptr ||
        functions.matrix == nullptr || functions.timestamp == nullptr) {
        functions.from_java = nullptr;
        __android_log_print(ANDROID_LOG_WARN, "libandroid",
                            "ASurfaceTexture: no SurfaceTexture without libandroid_runtime.so's functions for it: %s",
                            dlerror());
    }
}

}  // namespace

extern "C" {

// The texture of a Java SurfaceTexture, with a reference of its own; NULL for a finalized one or without Java.
ASurfaceTexture* ASurfaceTexture_fromSurfaceTexture(JNIEnv* env, jobject surfacetexture) {
    pthread_once(&looked_up, look_up);
    return functions.from_java != nullptr ? static_cast<ASurfaceTexture*>(functions.from_java(env, surfacetexture))
                                          : nullptr;
}

void ASurfaceTexture_release(ASurfaceTexture* st) {
    if (st != nullptr) functions.release(st);
}

// Its queue window, acquired for the caller, who releases it.
ANativeWindow* ASurfaceTexture_acquireANativeWindow(ASurfaceTexture* st) {
    return functions.acquire_window(st);
}

int ASurfaceTexture_attachToGLContext(ASurfaceTexture* st, uint32_t texName) {
    return functions.attach(st, texName);
}

int ASurfaceTexture_detachFromGLContext(ASurfaceTexture* st) {
    return functions.detach(st);
}

int ASurfaceTexture_updateTexImage(ASurfaceTexture* st) {
    return functions.update(st);
}

void ASurfaceTexture_getTransformMatrix(ASurfaceTexture* st, float mtx[16]) {
    functions.matrix(st, mtx);
}

int64_t ASurfaceTexture_getTimestamp(ASurfaceTexture* st) {
    return functions.timestamp(st);
}

}  // extern "C"
