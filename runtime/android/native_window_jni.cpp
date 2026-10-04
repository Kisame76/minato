// libandroid.so's ANativeWindow_fromSurface and ANativeWindow_toSurface (ADR-0043 c2). Android's libandroid builds them
// on libandroid_runtime's android_view_Surface_getNativeWindow and android_view_Surface_createFromSurface; ours call
// the two functions that Surface's natives in libandroid_runtime export for them, minato_ANativeWindow_fromSurface and
// minato_ANativeWindow_toSurface, looked up through dlopen at the first call, so that libandroid needs no
// libandroid_runtime where there is no Java, on M3's path. There they answer NULL and log once, as their stubs did.
#include <android/log.h>
#include <android/native_window_jni.h>
#include <dlfcn.h>
#include <pthread.h>

namespace {

ANativeWindow* (*from_surface)(JNIEnv*, jobject);
jobject (*to_surface)(JNIEnv*, ANativeWindow*);
pthread_once_t looked_up = PTHREAD_ONCE_INIT;

void look_up() {
    if (void* library = dlopen("libandroid_runtime.so", RTLD_NOW)) {
        from_surface = reinterpret_cast<decltype(from_surface)>(dlsym(library, "minato_ANativeWindow_fromSurface"));
        to_surface = reinterpret_cast<decltype(to_surface)>(dlsym(library, "minato_ANativeWindow_toSurface"));
    }
    if (from_surface == nullptr || to_surface == nullptr) {
        __android_log_print(ANDROID_LOG_WARN, "libandroid",
                            "ANativeWindow_fromSurface and ANativeWindow_toSurface: no Surface without "
                            "libandroid_runtime.so's functions for them: %s",
                            dlerror());
    }
}

}  // namespace

extern "C" {

// The window of a Java Surface, acquired for the caller; NULL for a released Surface.
ANativeWindow* ANativeWindow_fromSurface(JNIEnv* env, jobject surface) {
    pthread_once(&looked_up, look_up);
    return from_surface != nullptr ? from_surface(env, surface) : nullptr;
}

// A Java Surface of the window, which holds a reference of its own until it is released or collected.
jobject ANativeWindow_toSurface(JNIEnv* env, ANativeWindow* window) {
    pthread_once(&looked_up, look_up);
    return to_surface != nullptr ? to_surface(env, window) : nullptr;
}

}  // extern "C"
