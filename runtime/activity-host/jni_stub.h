// The JNI stub of activity-host (ADR-0028, decision 8): a JavaVM and a JNIEnv for ANativeActivity until
// ART's replace them in M4 (ADR-0033). Plain C, so that fixtures/suite/jni-test.c tests it as it is.
#ifndef MINATO_ACTIVITY_HOST_JNI_STUB_H
#define MINATO_ACTIVITY_HOST_JNI_STUB_H

#include <jni.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Attaches the calling thread (activity-host's main thread, attached from the start as on Android) and
/// gives the stub's JavaVM, the thread's JNIEnv and the handle that stands in for the activity's object
/// (never NULL).
void minato_jni_stub(JavaVM **vm, JNIEnv **env, jobject *clazz);

#ifdef __cplusplus
}
#endif

#endif
