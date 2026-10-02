// Ours in place of AOSP's core/jni/android_util_Binder.cpp (ADR-0039 decision 10, b3.3): of its functions, the
// core/jni files we take call only newParcelFileDescriptor (AssetManager's openFd and openNonAssetFd). It makes a
// ParcelFileDescriptor around the FileDescriptor as AOSP's does, with ParcelFileDescriptor(FileDescriptor), looked
// up at the call, so registration does not depend on it. framework.jar holds the class (b3.3), but its constructor
// calls android.util.CloseGuard.get(), a stub until b3.4, so the call throws NoClassDefFoundError in the app until
// then. Binder itself comes with step c.
#include "android_util_Binder.h"

namespace android {

jobject newParcelFileDescriptor(JNIEnv *env, jobject fileDesc) {
    jclass clazz = env->FindClass("android/os/ParcelFileDescriptor");
    if (clazz == nullptr) {
        return nullptr;
    }
    jmethodID constructor = env->GetMethodID(clazz, "<init>", "(Ljava/io/FileDescriptor;)V");
    jobject result = constructor != nullptr ? env->NewObject(clazz, constructor, fileDesc) : nullptr;
    env->DeleteLocalRef(clazz);
    return result;
}

}  // namespace android
