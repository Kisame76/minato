// Ours in place of AOSP's core/jni/android_os_Trace.cpp (ADR-0039 decision 10, b3.3): android.os.Trace's natives,
// which do nothing. AOSP's write to Perfetto through libtracing_perfetto; Minato has no tracing service, so no tag is
// ever enabled, and Trace.isTagEnabled answers false, as on a device where nothing traces. The table is AOSP's, with
// nativeIsTagEnabled @CriticalNative (no JNIEnv) and the rest @FastNative or plain, as Trace.java declares them.
#include <jni.h>
#include <nativehelper/JNIHelp.h>

#include "core_jni_helpers.h"

namespace android {
namespace {

void SetBoolean(JNIEnv *, jclass, jboolean) {}
void TraceCounter(JNIEnv *, jclass, jlong, jstring, jlong) {}
void TraceBegin(JNIEnv *, jclass, jlong, jstring) {}
void TraceEnd(JNIEnv *, jclass, jlong) {}
void AsyncTrace(JNIEnv *, jclass, jlong, jstring, jint) {}
void AsyncTraceForTrackBegin(JNIEnv *, jclass, jlong, jstring, jstring, jint) {}
void Instant(JNIEnv *, jclass, jlong, jstring) {}
void InstantForTrack(JNIEnv *, jclass, jlong, jstring, jstring) {}
void RegisterWithPerfetto(JNIEnv *, jclass) {}
jboolean IsTagEnabled(jlong) {
    return JNI_FALSE;
}

const JNINativeMethod gTraceMethods[] = {
        {"nativeSetAppTracingAllowed", "(Z)V", reinterpret_cast<void *>(SetBoolean)},
        {"nativeSetTracingEnabled", "(Z)V", reinterpret_cast<void *>(SetBoolean)},
        {"nativeTraceCounter", "(JLjava/lang/String;J)V", reinterpret_cast<void *>(TraceCounter)},
        {"nativeTraceBegin", "(JLjava/lang/String;)V", reinterpret_cast<void *>(TraceBegin)},
        {"nativeTraceEnd", "(J)V", reinterpret_cast<void *>(TraceEnd)},
        {"nativeAsyncTraceBegin", "(JLjava/lang/String;I)V", reinterpret_cast<void *>(AsyncTrace)},
        {"nativeAsyncTraceEnd", "(JLjava/lang/String;I)V", reinterpret_cast<void *>(AsyncTrace)},
        {"nativeAsyncTraceForTrackBegin", "(JLjava/lang/String;Ljava/lang/String;I)V",
         reinterpret_cast<void *>(AsyncTraceForTrackBegin)},
        {"nativeAsyncTraceForTrackEnd", "(JLjava/lang/String;I)V", reinterpret_cast<void *>(AsyncTrace)},
        {"nativeInstant", "(JLjava/lang/String;)V", reinterpret_cast<void *>(Instant)},
        {"nativeInstantForTrack", "(JLjava/lang/String;Ljava/lang/String;)V",
         reinterpret_cast<void *>(InstantForTrack)},
        {"nativeRegisterWithPerfetto", "()V", reinterpret_cast<void *>(RegisterWithPerfetto)},
        {"nativeIsTagEnabled", "(J)Z", reinterpret_cast<void *>(IsTagEnabled)},
};

}  // namespace

int register_android_os_Trace(JNIEnv *env) {
    return RegisterMethodsOrDie(env, "android/os/Trace", gTraceMethods, NELEM(gTraceMethods));
}

}  // namespace android
