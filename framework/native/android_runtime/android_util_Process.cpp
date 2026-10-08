// Ours in place of AOSP's core/jni/android_util_Process.cpp, for its thread priorities only (the Unity test APK probe's blocker
// 7): android.os.Process's setThreadPriority, both forms, and getThreadPriority as the tag's, over the runtime's
// setpriority and getpriority, which keep a nice value per thread (ADR-0033 decision 8, changed 2026-10-08). Their
// exceptions are the tag's signalExceptionForPriorityError and signalExceptionForError, with the same messages. A set
// goes as libutils' androidSetThreadPriority at the tag does (Threads.cpp): nothing when getpriority already gives the
// value, no task profiles; GUARD_THREAD_PRIORITY is 0 at the tag. The rest of AOSP's file (process groups, cgroups,
// /proc's readers) has no counterpart on Minato yet.
#include <errno.h>
#include <jni.h>
#include <nativehelper/JNIHelp.h>
#include <sys/resource.h>
#include <unistd.h>

#include "core_jni_helpers.h"

namespace android {
namespace {

void SignalExceptionForError(JNIEnv *env, int err, int tid) {
    switch (err) {
        case EINVAL:
            jniThrowExceptionFmt(env, "java/lang/IllegalArgumentException", "Invalid argument: %d", tid);
            break;
        case ESRCH:
            jniThrowExceptionFmt(env, "java/lang/IllegalArgumentException", "Given thread %d does not exist", tid);
            break;
        case EPERM:
            jniThrowExceptionFmt(env, "java/lang/SecurityException", "No permission to modify given thread %d", tid);
            break;
        default:
            jniThrowException(env, "java/lang/RuntimeException", "Unknown error");
            break;
    }
}

void SignalExceptionForPriorityError(JNIEnv *env, int err, int tid) {
    if (err == EACCES) {
        jniThrowExceptionFmt(env, "java/lang/SecurityException", "No permission to set the priority of %d", tid);
    } else {
        SignalExceptionForError(env, err, tid);
    }
}

void SetThreadPriority(JNIEnv *env, jclass, jint tid, jint priority) {
    // A thread that already has the value stays as it is, which for an unknown thread is getpriority's -1.
    if (getpriority(PRIO_PROCESS, tid) == priority) {
        return;
    }
    if (setpriority(PRIO_PROCESS, tid, priority) < 0) {
        SignalExceptionForPriorityError(env, errno, tid);
    }
}

void SetCallingThreadPriority(JNIEnv *env, jclass clazz, jint priority) {
    SetThreadPriority(env, clazz, gettid(), priority);
}

jint GetThreadPriority(JNIEnv *env, jclass, jint tid) {
    errno = 0;
    jint priority = getpriority(PRIO_PROCESS, tid);
    if (errno != 0) {
        SignalExceptionForPriorityError(env, errno, tid);
    }
    return priority;
}

const JNINativeMethod gProcessMethods[] = {
        {"setThreadPriority", "(II)V", reinterpret_cast<void *>(SetThreadPriority)},
        {"setThreadPriority", "(I)V", reinterpret_cast<void *>(SetCallingThreadPriority)},
        {"getThreadPriority", "(I)I", reinterpret_cast<void *>(GetThreadPriority)},
};

}  // namespace

int register_android_os_Process(JNIEnv *env) {
    return RegisterMethodsOrDie(env, "android/os/Process", gProcessMethods, NELEM(gProcessMethods));
}

}  // namespace android
