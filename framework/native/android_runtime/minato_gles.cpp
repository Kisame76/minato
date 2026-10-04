// Ours (ADR-0041 a)): see minato_gles.h.
#define LOG_TAG "libEGL"

#include "minato_gles.h"

#include <atomic>

#include <log/log.h>

namespace android {

ANativeWindow *minato_surface_window(JNIEnv *, jobject) {
    static std::atomic_flag logged = ATOMIC_FLAG_INIT;
    if (!logged.test_and_set(std::memory_order_relaxed)) {
        ALOGW("eglCreateWindowSurface: a Surface has no window until Surface's natives (ADR-0041 a))");
    }
    return nullptr;
}

void minato_unwritten(JNIEnv *env, const char *method) {
    jclass unwritten = env->FindClass("com/android/minato/services/Unwritten");
    jmethodID log = unwritten != nullptr
            ? env->GetStaticMethodID(unwritten, "log", "(Ljava/lang/String;Ljava/lang/String;)V")
            : nullptr;
    jstring text = log != nullptr ? env->NewStringUTF(method) : nullptr;
    if (text != nullptr) {
        env->CallStaticVoidMethod(unwritten, log, text, nullptr);
    }
    // The caller throws next: whatever went wrong here must not stand in its way.
    env->ExceptionClear();
    if (text != nullptr) env->DeleteLocalRef(text);
    if (unwritten != nullptr) env->DeleteLocalRef(unwritten);
}

}  // namespace android
