// ADR-0058 a)'s test (hardware-buffer-test): the JNI library HardwareBufferApplication loads, with libandroid's
// AHardwareBuffer_fromHardwareBuffer and AHardwareBuffer_toHardwareBuffer on both sides of a HardwareBuffer.
// getId is API 31's, above the library's 29: a weak reference, which Android 15 and Minato both resolve.
#define __ANDROID_UNAVAILABLE_SYMBOLS_ARE_WEAK__
#pragma clang diagnostic ignored "-Wunguarded-availability"
#include <android/hardware_buffer.h>
#include <android/hardware_buffer_jni.h>
#include <jni.h>
#include <stdio.h>
#include <string.h>

#define NAME(name) Java_io_github_kisame76_minato_test_hardwarebuffer_HardwareBufferApplication_##name

// The native buffer behind a HardwareBuffer: its description, its ID against the Java side's, and a pattern written
// through it.
JNIEXPORT jstring JNICALL NAME(fromJava)(JNIEnv *env, jclass type, jobject buffer, jlong id) {
    (void)type;
    AHardwareBuffer *native = AHardwareBuffer_fromHardwareBuffer(env, buffer);
    char line[200];
    if (native == NULL) return (*env)->NewStringUTF(env, "no native buffer");
    AHardwareBuffer_Desc desc;
    AHardwareBuffer_describe(native, &desc);
    uint64_t native_id = 0;
    AHardwareBuffer_getId(native, &native_id);
    void *address = NULL;
    const int locked = AHardwareBuffer_lock(native, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, NULL, &address);
    if (locked == 0) memset(address, 0x5a, 4);
    const int unlocked = AHardwareBuffer_unlock(native, NULL);
    snprintf(line, sizeof line, "%u x %u x %u, format %u, the same ID %d, lock %d, unlock %d", desc.width, desc.height,
             desc.layers, desc.format, (jlong)native_id == id, locked, unlocked);
    return (*env)->NewStringUTF(env, line);
}

// A buffer made natively, handed to Java; the native reference is let go after it.
JNIEXPORT jobject JNICALL NAME(toJava)(JNIEnv *env, jclass type) {
    (void)type;
    AHardwareBuffer_Desc desc;
    memset(&desc, 0, sizeof desc);
    desc.width = 32;
    desc.height = 16;
    desc.layers = 1;
    desc.format = AHARDWAREBUFFER_FORMAT_R5G6B5_UNORM;
    desc.usage = AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN;
    AHardwareBuffer *native = NULL;
    if (AHardwareBuffer_allocate(&desc, &native) != 0) return NULL;
    jobject buffer = AHardwareBuffer_toHardwareBuffer(env, native);
    AHardwareBuffer_release(native);
    return buffer;
}

// The first bytes of a buffer, read natively, to see a pattern the Java side's buffer was written with.
JNIEXPORT jint JNICALL NAME(firstWord)(JNIEnv *env, jclass type, jobject buffer) {
    (void)type;
    AHardwareBuffer *native = AHardwareBuffer_fromHardwareBuffer(env, buffer);
    void *address = NULL;
    if (native == NULL || AHardwareBuffer_lock(native, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, NULL, &address) != 0)
        return -1;
    jint word;
    memcpy(&word, address, sizeof word);
    AHardwareBuffer_unlock(native, NULL);
    return word;
}
