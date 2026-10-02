// libandroid_runtime.so's registration (ADR-0039 decision 10, b3.3), which app_process calls on the main thread right
// after JNI_CreateJavaVM, where AOSP's AndroidRuntime::start calls its private startReg.
#pragma once

#include <jni.h>

namespace android {

// Records the VM for AndroidRuntime::getJavaVM and getJNIEnv, then registers the natives of framework.jar's classes
// in gRegJNI's order. 0, or -1 when a registration fails.
int register_framework_natives(JNIEnv *env);

}  // namespace android
