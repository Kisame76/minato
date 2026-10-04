// Ours in place of AOSP's core/jni/android_util_EventLog.cpp (ADR-0039 step c2c3): EventLog's natives, which write
// binary events to liblog's events buffer and read them back. Minato's liblog has no events buffer (logd's "events"
// log, which only dumpsys and the bug report read), so a write goes nowhere and reports 0 bytes, which no caller
// checks, and a read finds no events. Every Activity lifecycle step writes one through EventLogTags.
#include <jni.h>
#include <nativehelper/JNIHelp.h>

#include "core_jni_helpers.h"

namespace android {
namespace {

jint WriteInteger(JNIEnv *, jclass, jint, jint) {
    return 0;
}

jint WriteLong(JNIEnv *, jclass, jint, jlong) {
    return 0;
}

jint WriteFloat(JNIEnv *, jclass, jint, jfloat) {
    return 0;
}

jint WriteString(JNIEnv *, jclass, jint, jstring) {
    return 0;
}

jint WriteList(JNIEnv *, jclass, jint, jobjectArray) {
    return 0;
}

void ReadEvents(JNIEnv *, jclass, jintArray, jobject) {
}

void ReadEventsOnWrapping(JNIEnv *, jclass, jintArray, jlong, jobject) {
}

const JNINativeMethod gMethods[] = {
        {"writeEvent", "(II)I", reinterpret_cast<void *>(WriteInteger)},
        {"writeEvent", "(IJ)I", reinterpret_cast<void *>(WriteLong)},
        {"writeEvent", "(IF)I", reinterpret_cast<void *>(WriteFloat)},
        {"writeEvent", "(ILjava/lang/String;)I", reinterpret_cast<void *>(WriteString)},
        {"writeEvent", "(I[Ljava/lang/Object;)I", reinterpret_cast<void *>(WriteList)},
        {"readEvents", "([ILjava/util/Collection;)V", reinterpret_cast<void *>(ReadEvents)},
        {"readEventsOnWrapping", "([IJLjava/util/Collection;)V", reinterpret_cast<void *>(ReadEventsOnWrapping)},
};

}  // namespace

int register_android_util_EventLog(JNIEnv *env) {
    return RegisterMethodsOrDie(env, "android/util/EventLog", gMethods, NELEM(gMethods));
}

}  // namespace android
