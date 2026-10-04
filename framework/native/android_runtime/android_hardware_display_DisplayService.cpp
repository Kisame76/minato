// Ours (ADR-0041 a)): the display service's one native, the session's highest refresh rate from ADR-0028's
// configuration, which guest code reads through session_configuration (guest_interface.h) as ActivityThread's native
// does through libandroid. 0 without a session; any other error of the runtime throws.
#include <errno.h>
#include <jni.h>
#include <nativehelper/JNIHelp.h>
#include <string.h>
#include <unistd.h>

#include <APKRuntime/guest_interface.h>

#include "core_jni_helpers.h"

namespace android {
namespace {

jint NativeSessionRefreshRate(JNIEnv *env, jclass) {
    apkrt_configuration session{};
    if (syscall(APKRT_NR_SESSION_CONFIGURATION, &session) != 0) {
        if (errno != ENOENT) {
            jniThrowExceptionFmt(env, "java/lang/IllegalStateException", "session_configuration: %s",
                                 strerror(errno));
        }
        return 0;
    }
    return session.refresh_rate;
}

const JNINativeMethod gMethods[] = {
        {"nativeSessionRefreshRate", "()I", reinterpret_cast<void *>(NativeSessionRefreshRate)},
};

}  // namespace

int register_android_hardware_display_DisplayService(JNIEnv *env) {
    return RegisterMethodsOrDie(env, "android/hardware/display/DisplayService", gMethods, NELEM(gMethods));
}

}  // namespace android
