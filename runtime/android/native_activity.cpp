// ANativeActivity's requests of libandroid.so (ADR-0028, decision 4). On Android they write a command for the
// main thread, which then calls Java; here the runtime reports them to the host at once (session_request):
// ANativeActivity_finish asks for the quit sequence, and the runtime keeps the window flags and tells the host
// when AWINDOW_FLAG_KEEP_SCREEN_ON changes. The window's format is the front libs' business and M3 has no
// keyboard, so the other three only log, once each.
#include <android/log.h>
#include <android/native_activity.h>
#include <unistd.h>

#include "APKRuntime/guest_interface.h"

namespace {

void log_once(char *logged, const char *what) {
    if (!__atomic_exchange_n(logged, 1, __ATOMIC_RELAXED)) {
        __android_log_print(ANDROID_LOG_WARN, "libandroid", "%s", what);
    }
}

}  // namespace

extern "C" {

void ANativeActivity_finish(ANativeActivity *) {
    syscall(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_FINISH, 0, 0);
}

void ANativeActivity_setWindowFlags(ANativeActivity *, uint32_t addFlags, uint32_t removeFlags) {
    syscall(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_WINDOW_FLAGS, addFlags, removeFlags);
}

void ANativeActivity_setWindowFormat(ANativeActivity *, int32_t) {
    static char logged;
    log_once(&logged, "ANativeActivity_setWindowFormat: the graphics libraries choose the format");
}

void ANativeActivity_showSoftInput(ANativeActivity *, uint32_t) {
    static char logged;
    log_once(&logged, "ANativeActivity_showSoftInput: no keyboard in M3");
}

void ANativeActivity_hideSoftInput(ANativeActivity *, uint32_t) {
    static char logged;
    log_once(&logged, "ANativeActivity_hideSoftInput: no keyboard in M3");
}

}  // extern "C"
