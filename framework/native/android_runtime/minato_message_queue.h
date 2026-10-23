// Ours: the ALooper of a Java MessageQueue, which natives that watch a descriptor for a listener's queue need (the
// sensors' event queues, ADR-0061 step 2). AOSP's counterpart is android_os_MessageQueue_getMessageQueue.
#pragma once

#include <android/looper.h>
#include <jni.h>

namespace android {

// The ALooper that `messageQueueObj`'s native queue polls, or null for an object that is none or whose queue is
// destroyed. The looper stays valid while the queue is: callers that keep it ALooper_acquire it.
ALooper *android_os_MessageQueue_getALooper(JNIEnv *env, jobject messageQueueObj);

}  // namespace android
