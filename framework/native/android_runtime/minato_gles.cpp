// Ours (ADR-0041 a)): see minato_gles.h.
#define LOG_TAG "libEGL"

#include "minato_gles.h"

namespace android {

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
