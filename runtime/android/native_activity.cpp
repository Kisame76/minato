// ANativeActivity's requests of libandroid.so (ADR-0028, decision 4). On Android they write a command for the
// main thread, which then calls Java; here the runtime reports them to the host at once (session_request):
// ANativeActivity_finish asks for the quit sequence, and the runtime keeps the window flags and tells the host
// when AWINDOW_FLAG_KEEP_SCREEN_ON changes, and keeps the window's format and tells the host when the window's
// opacity changes. The soft keyboard's two do nothing, as on Android 15, whose input method manager ignores them for
// NativeActivity's view, which is never the served one ("is not served"; ADR-0068 step 2e, native-ime-test on the
// AVD); they log once each. On the Java path (ADR-0069) the activity is libandroid_runtime's, whose
// android_NativeActivity_* take all five, as Android's libandroid calls them: they ask the activity's Java object on
// the main thread, so the soft keyboard's are ignored there by the input method manager itself.
#include <android/log.h>
#include <android/native_activity.h>
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>

#include "APKRuntime/guest_interface.h"

namespace {

void log_once(char *logged, const char *what) {
    if (!__atomic_exchange_n(logged, 1, __ATOMIC_RELAXED)) {
        __android_log_print(ANDROID_LOG_WARN, "libandroid", "%s", what);
    }
}

/// libandroid_runtime's android_NativeActivity_* when the process has it (the Java path), else none (activity-host).
struct JavaPath {
    void (*finish)(ANativeActivity *);
    void (*setWindowFormat)(ANativeActivity *, int32_t);
    void (*setWindowFlags)(ANativeActivity *, int32_t, int32_t);
    void (*showSoftInput)(ANativeActivity *, int32_t);
    void (*hideSoftInput)(ANativeActivity *, int32_t);
};

JavaPath java_path_functions;
pthread_once_t java_path_looked_up = PTHREAD_ONCE_INIT;

void look_up_java_path() {
    if (void *library = dlopen("libandroid_runtime.so", RTLD_NOW | RTLD_NOLOAD)) {
        JavaPath &found = java_path_functions;
        found.finish = reinterpret_cast<decltype(found.finish)>(dlsym(library, "android_NativeActivity_finish"));
        found.setWindowFormat = reinterpret_cast<decltype(found.setWindowFormat)>(
                dlsym(library, "android_NativeActivity_setWindowFormat"));
        found.setWindowFlags = reinterpret_cast<decltype(found.setWindowFlags)>(
                dlsym(library, "android_NativeActivity_setWindowFlags"));
        found.showSoftInput = reinterpret_cast<decltype(found.showSoftInput)>(
                dlsym(library, "android_NativeActivity_showSoftInput"));
        found.hideSoftInput = reinterpret_cast<decltype(found.hideSoftInput)>(
                dlsym(library, "android_NativeActivity_hideSoftInput"));
    }
}

const JavaPath &java_path() {
    pthread_once(&java_path_looked_up, look_up_java_path);
    return java_path_functions;
}

}  // namespace

extern "C" {

void ANativeActivity_finish(ANativeActivity *activity) {
    if (java_path().finish != nullptr) return java_path().finish(activity);
    syscall(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_FINISH, 0, 0);
}

// As Android's: the Java path's window takes the added flags under the mask of both.
void ANativeActivity_setWindowFlags(ANativeActivity *activity, uint32_t addFlags, uint32_t removeFlags) {
    if (java_path().setWindowFlags != nullptr) {
        return java_path().setWindowFlags(activity, static_cast<int32_t>(addFlags),
                                          static_cast<int32_t>(addFlags | removeFlags));
    }
    syscall(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_WINDOW_FLAGS, addFlags, removeFlags);
}

// The window's format decides only its opacity (PixelFormat.formatHasAlpha), which the runtime reports to
// the host; the buffers' format stays the graphics libraries' choice, as on Android.
void ANativeActivity_setWindowFormat(ANativeActivity *activity, int32_t format) {
    if (java_path().setWindowFormat != nullptr) return java_path().setWindowFormat(activity, format);
    syscall(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_WINDOW_FORMAT, static_cast<long>(format), 0);
}

void ANativeActivity_showSoftInput(ANativeActivity *activity, uint32_t flags) {
    if (java_path().showSoftInput != nullptr) return java_path().showSoftInput(activity, static_cast<int32_t>(flags));
    static char logged;
    log_once(&logged, "ANativeActivity_showSoftInput: ignored, as on Android 15: the activity's view is not served");
}

void ANativeActivity_hideSoftInput(ANativeActivity *activity, uint32_t flags) {
    if (java_path().hideSoftInput != nullptr) return java_path().hideSoftInput(activity, static_cast<int32_t>(flags));
    static char logged;
    log_once(&logged, "ANativeActivity_hideSoftInput: ignored, as on Android 15: the activity's view is not served");
}

}  // extern "C"
