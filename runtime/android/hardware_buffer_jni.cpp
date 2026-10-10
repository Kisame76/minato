// libandroid.so's AHardwareBuffer_fromHardwareBuffer and AHardwareBuffer_toHardwareBuffer (ADR-0058 a)), as
// native_window_jni.cpp's ANativeWindow_fromSurface: Android's libandroid builds them on libandroid_runtime's
// android_hardware_HardwareBuffer_getNativeHardwareBuffer and _createFromAHardwareBuffer; ours call the two functions
// that HardwareBuffer's natives in libandroid_runtime export for them, looked up through dlopen at the first call, so
// that libandroid needs no libandroid_runtime where there is no Java. There they answer NULL and log once.
#include <android/hardware_buffer.h>
#include <jni.h>
#include <android/log.h>
#include <dlfcn.h>
#include <pthread.h>

namespace {

AHardwareBuffer* (*from_java)(JNIEnv*, jobject);
jobject (*to_java)(JNIEnv*, AHardwareBuffer*);
pthread_once_t looked_up = PTHREAD_ONCE_INIT;

void look_up() {
    if (void* library = dlopen("libandroid_runtime.so", RTLD_NOW)) {
        from_java = reinterpret_cast<decltype(from_java)>(dlsym(library, "minato_AHardwareBuffer_fromHardwareBuffer"));
        to_java = reinterpret_cast<decltype(to_java)>(dlsym(library, "minato_AHardwareBuffer_toHardwareBuffer"));
    }
    if (from_java == nullptr || to_java == nullptr) {
        __android_log_print(ANDROID_LOG_WARN, "libandroid",
                            "AHardwareBuffer_fromHardwareBuffer and AHardwareBuffer_toHardwareBuffer: no "
                            "HardwareBuffer without libandroid_runtime.so's functions for them: %s",
                            dlerror());
    }
}

}  // namespace

extern "C" {

// The buffer of a Java HardwareBuffer, not acquired: valid while the HardwareBuffer is; NULL for a closed one.
AHardwareBuffer* AHardwareBuffer_fromHardwareBuffer(JNIEnv* env, jobject hardwareBufferObj) {
    pthread_once(&looked_up, look_up);
    return from_java != nullptr ? from_java(env, hardwareBufferObj) : nullptr;
}

// A Java HardwareBuffer of the buffer, which holds a reference of its own until it is closed or collected.
jobject AHardwareBuffer_toHardwareBuffer(JNIEnv* env, AHardwareBuffer* hardwareBuffer) {
    pthread_once(&looked_up, look_up);
    return to_java != nullptr ? to_java(env, hardwareBuffer) : nullptr;
}

}  // extern "C"
