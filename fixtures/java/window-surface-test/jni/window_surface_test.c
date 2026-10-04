// window-surface-test's JNI library (ADR-0043 c2): ANativeWindow_fromSurface and ANativeWindow_toSurface, as NDK code
// that takes a Java Surface reaches its window, and the reference that fromSurface takes for its caller.
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <jni.h>
#include <stdio.h>

static ANativeWindow *kept;  // fromSurface's reference, until nativeKept releases it

// The window of the Surface, kept: its size and format.
JNIEXPORT jstring JNICALL
Java_io_github_kisame76_minato_test_windowsurface_WindowSurfaceApplication_nativeFromSurface(JNIEnv *env, jclass clazz,
                                                                                           jobject surface) {
    (void)clazz;
    kept = ANativeWindow_fromSurface(env, surface);
    char text[96];
    if (kept == NULL) {
        snprintf(text, sizeof text, "no window");
    } else {
        snprintf(text, sizeof text, "%d x %d, format %d", ANativeWindow_getWidth(kept), ANativeWindow_getHeight(kept),
                 ANativeWindow_getFormat(kept));
    }
    return (*env)->NewStringUTF(env, text);
}

// A Surface of the kept window, which holds a reference of its own.
JNIEXPORT jobject JNICALL
Java_io_github_kisame76_minato_test_windowsurface_WindowSurfaceApplication_nativeToSurface(JNIEnv *env, jclass clazz) {
    (void)clazz;
    return kept != NULL ? ANativeWindow_toSurface(env, kept) : NULL;
}

// The kept window's size, after every Surface of it is released; then its reference goes too.
JNIEXPORT jstring JNICALL
Java_io_github_kisame76_minato_test_windowsurface_WindowSurfaceApplication_nativeKept(JNIEnv *env, jclass clazz) {
    (void)clazz;
    char text[96];
    if (kept == NULL) {
        snprintf(text, sizeof text, "none");
    } else {
        snprintf(text, sizeof text, "%d x %d", ANativeWindow_getWidth(kept), ANativeWindow_getHeight(kept));
        ANativeWindow_release(kept);
        kept = NULL;
    }
    return (*env)->NewStringUTF(env, text);
}
