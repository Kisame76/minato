// Ours in place of AOSP's core/jni/android_hardware_HardwareBuffer.cpp (ADR-0058 a)): android.hardware.HardwareBuffer's
// natives on libnativewindow's AHardwareBuffer, where the tag's hold a GraphicBuffer of libui. The method table and
// its signatures are the tag's. What differs:
// - mNativeObject is the AHardwareBuffer*, with one reference of its own; the native finalizer is
//   AHardwareBuffer_release. The tag's create answers 0 for a format without a pixel format and for a buffer gralloc
//   cannot make, as ours does for any allocate error, so HardwareBuffer.create's IllegalArgumentException is the same.
// - The usage bits and formats are AHardwareBuffer's, which the tag converts to gralloc's and back without a change
//   for the bits the NDK has.
// - A parcel carries the buffer's ID (minato_parcel, one process): a read takes a reference of its own while the
//   buffer still lives, and answers 0 otherwise, as the tag's for a failed unflatten (BadParcelableException).
//   A parcel holds no reference, so a buffer released before its parcel is read cannot be read back.
// - createFromGraphicBuffer has no GraphicBuffer to take (a hidden type, compile-only): it answers 0.
// - estimateSize is the tag's height x stride x bytes per pixel, from describe.
// - android_hardware_HardwareBuffer_getNativeHardwareBuffer and _createFromAHardwareBuffer are exported for
//   libandroid's AHardwareBuffer_fromHardwareBuffer and toHardwareBuffer as minato_AHardwareBuffer_fromHardwareBuffer
//   and minato_AHardwareBuffer_toHardwareBuffer (runtime/android/hardware_buffer_jni.cpp).
#include <android/hardware_buffer.h>
#include <jni.h>
#include <nativehelper/JNIHelp.h>

#include <binder/Parcel.h>

#include "core_jni_helpers.h"

namespace android {

extern Parcel *parcelForJavaObject(JNIEnv *env, jobject obj);

namespace {

struct {
    jclass clazz;
    jfieldID mNativeObject;
    jmethodID ctor;
} gHardwareBufferClassInfo;

// "MNAH" before the ID, so that a parcel's other bytes never read as a buffer.
constexpr int32_t kParcelMagic = 0x48414e4d;

AHardwareBuffer *buffer_of(jlong nativeObject) {
    return reinterpret_cast<AHardwareBuffer *>(nativeObject);
}

AHardwareBuffer_Desc describe(jlong nativeObject) {
    AHardwareBuffer_Desc desc{};
    AHardwareBuffer_describe(buffer_of(nativeObject), &desc);
    return desc;
}

uint32_t bytes_per_pixel(uint32_t format) {
    switch (format) {
        case AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM:
        case AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM:
        case AHARDWAREBUFFER_FORMAT_R10G10B10A2_UNORM:
        case AHARDWAREBUFFER_FORMAT_D24_UNORM_S8_UINT:
        case AHARDWAREBUFFER_FORMAT_D32_FLOAT:
            return 4;
        case AHARDWAREBUFFER_FORMAT_R8G8B8_UNORM:
        case AHARDWAREBUFFER_FORMAT_D24_UNORM:
            return 3;
        case AHARDWAREBUFFER_FORMAT_R5G6B5_UNORM:
        case AHARDWAREBUFFER_FORMAT_D16_UNORM:
            return 2;
        case AHARDWAREBUFFER_FORMAT_R16G16B16A16_FLOAT:
            return 8;
        default:
            return 1;  // the tag's default for a format it does not know
    }
}

jlong android_hardware_HardwareBuffer_create(JNIEnv *, jobject, jint width, jint height, jint format, jint layers,
                                             jlong usage) {
    AHardwareBuffer_Desc desc{};
    desc.width = static_cast<uint32_t>(width);
    desc.height = static_cast<uint32_t>(height);
    desc.layers = static_cast<uint32_t>(layers);
    desc.format = static_cast<uint32_t>(format);
    desc.usage = static_cast<uint64_t>(usage);
    AHardwareBuffer *buffer = nullptr;
    if (AHardwareBuffer_allocate(&desc, &buffer) != 0) return 0;
    return reinterpret_cast<jlong>(buffer);
}

jlong android_hardware_HardwareBuffer_createFromGraphicBuffer(JNIEnv *, jobject, jobject) {
    return 0;
}

void release_buffer(AHardwareBuffer *buffer) {
    AHardwareBuffer_release(buffer);
}

jlong android_hardware_HardwareBuffer_getNativeFinalizer(JNIEnv *, jobject) {
    return static_cast<jlong>(reinterpret_cast<uintptr_t>(&release_buffer));
}

jboolean android_hardware_HardwareBuffer_isSupported(JNIEnv *, jobject, jint width, jint height, jint format,
                                                     jint layers, jlong usage) {
    AHardwareBuffer_Desc desc{};
    desc.width = static_cast<uint32_t>(width);
    desc.height = static_cast<uint32_t>(height);
    desc.format = static_cast<uint32_t>(format);
    desc.layers = static_cast<uint32_t>(layers);
    desc.usage = static_cast<uint64_t>(usage);
    return AHardwareBuffer_isSupported(&desc) != 0;
}

jint android_hardware_HardwareBuffer_getWidth(JNIEnv *, jobject, jlong nativeObject) {
    return static_cast<jint>(describe(nativeObject).width);
}

jint android_hardware_HardwareBuffer_getHeight(JNIEnv *, jobject, jlong nativeObject) {
    return static_cast<jint>(describe(nativeObject).height);
}

jint android_hardware_HardwareBuffer_getFormat(JNIEnv *, jobject, jlong nativeObject) {
    return static_cast<jint>(describe(nativeObject).format);
}

jint android_hardware_HardwareBuffer_getLayers(JNIEnv *, jobject, jlong nativeObject) {
    return static_cast<jint>(describe(nativeObject).layers);
}

jlong android_hardware_HardwareBuffer_getUsage(JNIEnv *, jobject, jlong nativeObject) {
    return static_cast<jlong>(describe(nativeObject).usage);
}

jlong android_hardware_HardwareBuffer_estimateSize(jlong nativeObject) {
    const AHardwareBuffer_Desc desc = describe(nativeObject);
    const uint64_t stride = desc.stride > 0 ? desc.stride : desc.width;
    return static_cast<jlong>(static_cast<uint64_t>(desc.height) * stride * bytes_per_pixel(desc.format));
}

jlong android_hardware_HardwareBuffer_getId(jlong nativeObject) {
    uint64_t id = 0;
    AHardwareBuffer_getId(buffer_of(nativeObject), &id);
    return static_cast<jlong>(id);
}

void android_hardware_HardwareBuffer_write(JNIEnv *env, jobject, jlong nativeObject, jobject dest) {
    Parcel *parcel = parcelForJavaObject(env, dest);
    if (parcel == nullptr) return;
    // Always the magic and an ID, 0 for one getId refuses, so the parcel's next fields stay where they are and the
    // read answers 0.
    uint64_t id = 0;
    if (AHardwareBuffer_getId(buffer_of(nativeObject), &id) != 0) id = 0;
    parcel->writeInt32(kParcelMagic);
    parcel->writeInt64(static_cast<int64_t>(id));
}

jlong android_hardware_HardwareBuffer_read(JNIEnv *env, jobject, jobject in) {
    Parcel *parcel = parcelForJavaObject(env, in);
    if (parcel == nullptr || parcel->readInt32() != kParcelMagic) return 0;
    const uint64_t id = static_cast<uint64_t>(parcel->readInt64());
    // libnativewindow's AHardwareBuffer* is its ID shifted by 4; describe finds out whether the buffer still lives.
    AHardwareBuffer *buffer = reinterpret_cast<AHardwareBuffer *>(static_cast<uintptr_t>(id << 4));
    AHardwareBuffer_Desc desc{};
    if (id == 0) return 0;
    AHardwareBuffer_describe(buffer, &desc);
    if (desc.width == 0) return 0;
    AHardwareBuffer_acquire(buffer);
    return reinterpret_cast<jlong>(buffer);
}

// clang-format off
const JNINativeMethod gMethods[] = {
    { "nCreateHardwareBuffer",  "(IIIIJ)J",
            (void*) android_hardware_HardwareBuffer_create },
    { "nCreateFromGraphicBuffer", "(Landroid/graphics/GraphicBuffer;)J",
            (void*) android_hardware_HardwareBuffer_createFromGraphicBuffer },
    { "nGetNativeFinalizer", "()J",
            (void*) android_hardware_HardwareBuffer_getNativeFinalizer },
    { "nWriteHardwareBufferToParcel",  "(JLandroid/os/Parcel;)V",
            (void*) android_hardware_HardwareBuffer_write },
    { "nReadHardwareBufferFromParcel", "(Landroid/os/Parcel;)J",
            (void*) android_hardware_HardwareBuffer_read },
    { "nIsSupported",  "(IIIIJ)Z",
            (void*) android_hardware_HardwareBuffer_isSupported },

    // --------------- @FastNative ----------------------
    { "nGetWidth", "(J)I",      (void*) android_hardware_HardwareBuffer_getWidth },
    { "nGetHeight", "(J)I",     (void*) android_hardware_HardwareBuffer_getHeight },
    { "nGetFormat", "(J)I",     (void*) android_hardware_HardwareBuffer_getFormat },
    { "nGetLayers", "(J)I",     (void*) android_hardware_HardwareBuffer_getLayers },
    { "nGetUsage", "(J)J",      (void*) android_hardware_HardwareBuffer_getUsage },

    // --------------- @CriticalNative ----------------------
    { "nEstimateSize", "(J)J",  (void*) android_hardware_HardwareBuffer_estimateSize },
    { "nGetId", "(J)J",  (void*) android_hardware_HardwareBuffer_getId },
};
// clang-format on

}  // namespace

// The tag's: the AHardwareBuffer of a HardwareBuffer, not acquired; NULL for another object or a closed buffer.
AHardwareBuffer *android_hardware_HardwareBuffer_getNativeHardwareBuffer(JNIEnv *env, jobject hardwareBufferObj) {
    if (hardwareBufferObj == nullptr || !env->IsInstanceOf(hardwareBufferObj, gHardwareBufferClassInfo.clazz)) {
        return nullptr;
    }
    return buffer_of(env->GetLongField(hardwareBufferObj, gHardwareBufferClassInfo.mNativeObject));
}

// The tag's: a HardwareBuffer of an AHardwareBuffer, with a reference of its own.
jobject android_hardware_HardwareBuffer_createFromAHardwareBuffer(JNIEnv *env, AHardwareBuffer *hardwareBuffer) {
    if (hardwareBuffer == nullptr) return nullptr;
    AHardwareBuffer_acquire(hardwareBuffer);
    jobject hardwareBufferObj = env->NewObject(gHardwareBufferClassInfo.clazz, gHardwareBufferClassInfo.ctor,
                                               reinterpret_cast<jlong>(hardwareBuffer));
    if (hardwareBufferObj == nullptr) {
        AHardwareBuffer_release(hardwareBuffer);
        if (env->ExceptionCheck()) {
            ALOGE("Could not create instance of HardwareBuffer from AHardwareBuffer.");
            env->ExceptionClear();
        }
        return nullptr;
    }
    return hardwareBufferObj;
}

int register_android_hardware_HardwareBuffer(JNIEnv *env) {
    int err = RegisterMethodsOrDie(env, "android/hardware/HardwareBuffer", gMethods, NELEM(gMethods));
    jclass clazz = FindClassOrDie(env, "android/hardware/HardwareBuffer");
    gHardwareBufferClassInfo.clazz = MakeGlobalRefOrDie(env, clazz);
    gHardwareBufferClassInfo.mNativeObject =
        GetFieldIDOrDie(env, gHardwareBufferClassInfo.clazz, "mNativeObject", "J");
    gHardwareBufferClassInfo.ctor = GetMethodIDOrDie(env, gHardwareBufferClassInfo.clazz, "<init>", "(J)V");
    return err;
}

}  // namespace android

// For libandroid's AHardwareBuffer_fromHardwareBuffer and toHardwareBuffer (runtime/android/hardware_buffer_jni.cpp).
extern "C" AHardwareBuffer *minato_AHardwareBuffer_fromHardwareBuffer(JNIEnv *env, jobject hardwareBufferObj) {
    return android::android_hardware_HardwareBuffer_getNativeHardwareBuffer(env, hardwareBufferObj);
}

extern "C" jobject minato_AHardwareBuffer_toHardwareBuffer(JNIEnv *env, AHardwareBuffer *hardwareBuffer) {
    return android::android_hardware_HardwareBuffer_createFromAHardwareBuffer(env, hardwareBuffer);
}
