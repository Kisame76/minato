// Ours in place of AOSP's core/jni/android_os_Parcel.cpp (ADR-0043 d2): android.os.Parcel's natives over Minato's
// android::Parcel (minato_parcel.h) instead of libbinder's. The table is AOSP's 53, with the same names, signatures and
// @CriticalNative (no JNIEnv) or @FastNative forms as Parcel.java declares them, and each function does what AOSP's
// does. A binder goes into a parcel as the Java object itself and comes back out as the same object; the thread's
// StrictMode policy, which libbinder keeps in IPCThreadState, is our Binder's (framework/java/android/os/Binder.java).
// libbinder's status codes become the exceptions of AOSP's signalExceptionForError (android_util_Binder.cpp). Where
// AOSP trusts a range from Java without a check of its own (unmarshall), Minato checks it against the array.
#define LOG_TAG "Parcel"

#include <fcntl.h>
#include <inttypes.h>
#include <jni.h>
#include <log/log.h>
#include <nativehelper/JNIHelp.h>
#include <nativehelper/JNIPlatformHelp.h>
#include <string.h>
#include <unistd.h>

#include <algorithm>
#include <string>

#include "core_jni_helpers.h"
#include "minato_parcel.h"

namespace android {
namespace {

struct {
    jclass clazz;
    jmethodID getThreadStrictModePolicy;
    jmethodID setThreadStrictModePolicy;
} gBinderOffsets;

struct {
    jclass clazz;
    jmethodID callback;
} gStrictModeCallbackOffsets;

Parcel *toParcel(jlong nativePtr) {
    return reinterpret_cast<Parcel *>(nativePtr);
}

// AOSP's signalExceptionForError for a parcel's call, which never throws a RemoteException.
void signalExceptionForError(JNIEnv *env, status_t err) {
    switch (err) {
        case UNKNOWN_ERROR:
            jniThrowException(env, "java/lang/RuntimeException", "Unknown error");
            break;
        case NO_MEMORY:
            jniThrowException(env, "java/lang/OutOfMemoryError", nullptr);
            break;
        case INVALID_OPERATION:
            jniThrowException(env, "java/lang/UnsupportedOperationException", nullptr);
            break;
        case BAD_VALUE:
        case BAD_TYPE:
            jniThrowException(env, "java/lang/IllegalArgumentException", nullptr);
            break;
        case BAD_INDEX:
            jniThrowException(env, "java/lang/IndexOutOfBoundsException", nullptr);
            break;
        case NAME_NOT_FOUND:
            jniThrowException(env, "java/util/NoSuchElementException", nullptr);
            break;
        case PERMISSION_DENIED:
            jniThrowException(env, "java/lang/SecurityException", nullptr);
            break;
        case NOT_ENOUGH_DATA:
            jniThrowException(env, "android/os/ParcelFormatException", "Not enough data");
            break;
        case NO_INIT:
            jniThrowException(env, "java/lang/RuntimeException", "Not initialized");
            break;
        case ALREADY_EXISTS:
            jniThrowException(env, "java/lang/RuntimeException", "Item already exists");
            break;
        case DEAD_OBJECT:
            jniThrowException(env, "java/lang/RuntimeException", nullptr);
            break;
        case UNKNOWN_TRANSACTION:
            jniThrowException(env, "java/lang/RuntimeException", "Unknown transaction code");
            break;
        case FAILED_TRANSACTION:
            ALOGE("!!! FAILED BINDER TRANSACTION !!!  (parcel size = 0)");
            jniThrowException(env, "java/lang/RuntimeException",
                              "Transaction failed on small parcel; remote process probably died, but this could "
                              "also be caused by running out of binder buffer space");
            break;
        case FDS_NOT_ALLOWED:
            jniThrowException(env, "java/lang/RuntimeException", "Not allowed to write file descriptors here");
            break;
        case UNEXPECTED_NULL:
            jniThrowNullPointerException(env, nullptr);
            break;
        case -EBADF:
            jniThrowException(env, "java/lang/RuntimeException", "Bad file descriptor");
            break;
        case -ENFILE:
            jniThrowException(env, "java/lang/RuntimeException", "File table overflow");
            break;
        case -EMFILE:
            jniThrowException(env, "java/lang/RuntimeException", "Too many open files");
            break;
        case -EFBIG:
            jniThrowException(env, "java/lang/RuntimeException", "File too large");
            break;
        case -ENOSPC:
            jniThrowException(env, "java/lang/RuntimeException", "No space left on device");
            break;
        case -ESPIPE:
            jniThrowException(env, "java/lang/RuntimeException", "Illegal seek");
            break;
        case -EROFS:
            jniThrowException(env, "java/lang/RuntimeException", "Read-only file system");
            break;
        case -EMLINK:
            jniThrowException(env, "java/lang/RuntimeException", "Too many links");
            break;
        default:
            ALOGE("Unknown binder error code. 0x%" PRIx32, err);
            jniThrowExceptionFmt(env, "java/lang/RuntimeException", "Unknown binder error code. 0x%" PRIx32, err);
            break;
    }
}

// Whether [offset, offset + length) lies in the array, with ArrayUtils.throwsIfOutOfBounds's exception when not.
bool checkRange(JNIEnv *env, jobject array, jint offset, jint length) {
    if (array == nullptr) {
        jniThrowNullPointerException(env, nullptr);
        return false;
    }
    const jsize size = env->GetArrayLength(static_cast<jarray>(array));
    if (offset < 0 || length < 0 || offset > size - length) {
        jniThrowExceptionFmt(env, "java/lang/ArrayIndexOutOfBoundsException",
                             "length=%d; regionStart=%d; regionLength=%d", size, offset, length);
        return false;
    }
    return true;
}

// An interface descriptor's UTF-16 characters, as AOSP's InterfaceDescriptorString holds them.
std::u16string descriptorOf(JNIEnv *env, jstring name) {
    LOG_ALWAYS_FATAL_IF(name == nullptr);
    std::u16string descriptor(env->GetStringLength(name), u'\0');
    env->GetStringRegion(name, 0, static_cast<jsize>(descriptor.size()), reinterpret_cast<jchar *>(descriptor.data()));
    return descriptor;
}

int32_t threadStrictModePolicy(JNIEnv *env) {
    return env->CallStaticIntMethod(gBinderOffsets.clazz, gBinderOffsets.getThreadStrictModePolicy);
}

void markSensitive(jlong nativePtr) {
    if (Parcel *parcel = toParcel(nativePtr)) parcel->markSensitive();
}

void markForBinder(JNIEnv *, jclass, jlong, jobject binder) {
    LOG_ALWAYS_FATAL_IF(binder == nullptr, "Null binder specified for markForBinder");
    // Only a binder of an RPC session marks a parcel for RPC, and Minato's process has none.
}

jboolean isForRpc(jlong) {
    return JNI_FALSE;
}

jint dataSize(jlong nativePtr) {
    const Parcel *parcel = toParcel(nativePtr);
    return parcel != nullptr ? parcel->dataSize() : 0;
}

jint dataAvail(jlong nativePtr) {
    const Parcel *parcel = toParcel(nativePtr);
    return parcel != nullptr ? parcel->dataAvail() : 0;
}

jint dataPosition(jlong nativePtr) {
    const Parcel *parcel = toParcel(nativePtr);
    return parcel != nullptr ? parcel->dataPosition() : 0;
}

jint dataCapacity(jlong nativePtr) {
    const Parcel *parcel = toParcel(nativePtr);
    return parcel != nullptr ? parcel->dataCapacity() : 0;
}

// A negative size or position from Java becomes a huge size_t, which libbinder refuses (BAD_VALUE) or, for the
// position, ends the process on, as Android's does.
void setDataSize(JNIEnv *env, jclass, jlong nativePtr, jint size) {
    if (Parcel *parcel = toParcel(nativePtr)) {
        const status_t err = parcel->setDataSize(static_cast<size_t>(size));
        if (err != NO_ERROR) signalExceptionForError(env, err);
    }
}

void setDataPosition(jlong nativePtr, jint pos) {
    if (const Parcel *parcel = toParcel(nativePtr)) parcel->setDataPosition(static_cast<size_t>(pos));
}

void setDataCapacity(JNIEnv *env, jclass, jlong nativePtr, jint size) {
    if (Parcel *parcel = toParcel(nativePtr)) {
        const status_t err = parcel->setDataCapacity(static_cast<size_t>(size));
        if (err != NO_ERROR) signalExceptionForError(env, err);
    }
}

jboolean pushAllowFds(jlong nativePtr, jboolean allowFds) {
    Parcel *parcel = toParcel(nativePtr);
    return parcel != nullptr ? parcel->pushAllowFds(allowFds) : JNI_TRUE;
}

void restoreAllowFds(jlong nativePtr, jboolean lastValue) {
    if (Parcel *parcel = toParcel(nativePtr)) parcel->restoreAllowFds(lastValue);
}

void writeByteArray(JNIEnv *env, jclass, jlong nativePtr, jobject data, jint offset, jint length) {
    Parcel *parcel = toParcel(nativePtr);
    if (parcel == nullptr || !checkRange(env, data, offset, length)) return;
    const status_t err = parcel->writeInt32(length);
    if (err != NO_ERROR) {
        signalExceptionForError(env, err);
        return;
    }
    void *dest = parcel->writeInplace(length);
    if (dest == nullptr) {
        signalExceptionForError(env, NO_MEMORY);
        return;
    }
    auto *array = static_cast<jbyte *>(env->GetPrimitiveArrayCritical(static_cast<jarray>(data), nullptr));
    if (array != nullptr) {
        memcpy(dest, array + offset, length);
        env->ReleasePrimitiveArrayCritical(static_cast<jarray>(data), array, 0);
    }
}

void writeBlob(JNIEnv *env, jclass, jlong nativePtr, jobject data, jint offset, jint length) {
    Parcel *parcel = toParcel(nativePtr);
    if (parcel == nullptr) return;
    if (data == nullptr) {
        const status_t err = parcel->writeInt32(-1);
        if (err != NO_ERROR) signalExceptionForError(env, err);
        return;
    }
    if (!checkRange(env, data, offset, length)) return;
    status_t err = parcel->writeInt32(length);
    if (err != NO_ERROR) {
        signalExceptionForError(env, err);
        return;
    }
    void *blob;
    err = parcel->writeBlob(length, &blob);
    if (err != NO_ERROR) {
        signalExceptionForError(env, err);
        return;
    }
    auto *array = static_cast<jbyte *>(env->GetPrimitiveArrayCritical(static_cast<jarray>(data), nullptr));
    if (array == nullptr) {
        memset(blob, 0, length);
    } else {
        memcpy(blob, array + offset, length);
        env->ReleasePrimitiveArrayCritical(static_cast<jarray>(data), array, 0);
    }
}

jint writeInt(jlong nativePtr, jint val) {
    Parcel *parcel = toParcel(nativePtr);
    return parcel != nullptr ? parcel->writeInt32(val) : NO_ERROR;
}

jint writeLong(jlong nativePtr, jlong val) {
    Parcel *parcel = toParcel(nativePtr);
    return parcel != nullptr ? parcel->writeInt64(val) : NO_ERROR;
}

jint writeFloat(jlong nativePtr, jfloat val) {
    Parcel *parcel = toParcel(nativePtr);
    return parcel != nullptr ? parcel->writeFloat(val) : NO_ERROR;
}

jint writeDouble(jlong nativePtr, jdouble val) {
    Parcel *parcel = toParcel(nativePtr);
    return parcel != nullptr ? parcel->writeDouble(val) : NO_ERROR;
}

void nativeSignalExceptionForError(JNIEnv *env, jclass, jint err) {
    signalExceptionForError(env, err);
}

void writeString8(JNIEnv *env, jclass, jlong nativePtr, jstring val) {
    Parcel *parcel = toParcel(nativePtr);
    if (parcel == nullptr) return;
    status_t err = NO_ERROR;
    if (val != nullptr) {
        // As AOSP's: the length in modified UTF-8, then the characters and a terminator.
        const jsize len = env->GetStringLength(val);
        const jsize allocLen = env->GetStringUTFLength(val);
        err = parcel->writeInt32(allocLen);
        auto *data = static_cast<char *>(parcel->writeInplace(allocLen + sizeof(char)));
        if (data != nullptr) {
            env->GetStringUTFRegion(val, 0, len, data);
            data[allocLen] = 0;
        } else {
            err = NO_MEMORY;
        }
    } else {
        err = parcel->writeString8(nullptr, 0);
    }
    if (err != NO_ERROR) signalExceptionForError(env, err);
}

void writeString16(JNIEnv *env, jclass, jlong nativePtr, jstring val) {
    Parcel *parcel = toParcel(nativePtr);
    if (parcel == nullptr) return;
    status_t err = NO_ERROR;
    if (val != nullptr) {
        const jsize len = env->GetStringLength(val);
        const size_t allocLen = len * sizeof(char16_t);
        err = parcel->writeInt32(len);
        auto *data = static_cast<char *>(parcel->writeInplace(allocLen + sizeof(char16_t)));
        if (data != nullptr) {
            env->GetStringRegion(val, 0, len, reinterpret_cast<jchar *>(data));
            const char16_t terminator = 0;
            memcpy(data + allocLen, &terminator, sizeof(terminator));
        } else {
            err = NO_MEMORY;
        }
    } else {
        err = parcel->writeString16(nullptr, 0);
    }
    if (err != NO_ERROR) signalExceptionForError(env, err);
}

void writeStrongBinder(JNIEnv *env, jclass, jlong nativePtr, jobject object) {
    if (Parcel *parcel = toParcel(nativePtr)) {
        // As AOSP's ibinderForJavaObject: only an android.os.Binder is a binder here, as no BinderProxy exists
        // without IPC; any other IBinder goes in as a null binder, with Android's warning.
        if (object != nullptr && !env->IsInstanceOf(object, gBinderOffsets.clazz)) {
            ALOGW("ibinderForJavaObject: %p is not a Binder object", object);
            object = nullptr;
        }
        const status_t err = parcel->writeStrongBinder(env, object);
        if (err != NO_ERROR) signalExceptionForError(env, err);
    }
}

void writeFileDescriptor(JNIEnv *env, jclass, jlong nativePtr, jobject object) {
    if (Parcel *parcel = toParcel(nativePtr)) {
        const status_t err = parcel->writeDupFileDescriptor(jniGetFDFromFileDescriptor(env, object));
        if (err != NO_ERROR) signalExceptionForError(env, err);
    }
}

jbyteArray createByteArray(JNIEnv *env, jclass, jlong nativePtr) {
    const Parcel *parcel = toParcel(nativePtr);
    if (parcel == nullptr) return nullptr;
    const int32_t len = parcel->readInt32();
    // The stored length must fit in what is left of the data.
    if (len < 0 || len > static_cast<int32_t>(parcel->dataAvail())) return nullptr;
    jbyteArray ret = env->NewByteArray(len);
    if (ret == nullptr) return nullptr;
    auto *array = static_cast<jbyte *>(env->GetPrimitiveArrayCritical(ret, nullptr));
    if (array != nullptr) {
        const void *data = parcel->readInplace(len);
        if (data != nullptr) memcpy(array, data, len);
        env->ReleasePrimitiveArrayCritical(ret, array, 0);
        if (data == nullptr) return nullptr;
    }
    return ret;
}

jboolean readByteArray(JNIEnv *env, jclass, jlong nativePtr, jobject dest, jint destLen) {
    const Parcel *parcel = toParcel(nativePtr);
    if (parcel == nullptr) return JNI_FALSE;
    const int32_t len = parcel->readInt32();
    if (len < 0 || len > static_cast<int32_t>(parcel->dataAvail()) || len != destLen) return JNI_FALSE;
    // Parcel.java passes the array's own length; checked here too rather than write past it.
    if (dest == nullptr || env->GetArrayLength(static_cast<jarray>(dest)) < len) return JNI_FALSE;
    jboolean ret = JNI_FALSE;
    auto *array = static_cast<jbyte *>(env->GetPrimitiveArrayCritical(static_cast<jarray>(dest), nullptr));
    if (array != nullptr) {
        const void *data = parcel->readInplace(len);
        if (data != nullptr) {
            memcpy(array, data, len);
            ret = JNI_TRUE;
        }
        env->ReleasePrimitiveArrayCritical(static_cast<jarray>(dest), array, 0);
    }
    return ret;
}

jbyteArray readBlob(JNIEnv *env, jclass, jlong nativePtr) {
    const Parcel *parcel = toParcel(nativePtr);
    if (parcel == nullptr) return nullptr;
    const int32_t len = parcel->readInt32();
    if (len < 0) return nullptr;
    const void *blob;
    const status_t err = parcel->readBlob(len, &blob);
    if (err != NO_ERROR) {
        signalExceptionForError(env, err);
        return nullptr;
    }
    jbyteArray ret = env->NewByteArray(len);
    if (ret != nullptr) {
        auto *array = static_cast<jbyte *>(env->GetPrimitiveArrayCritical(ret, nullptr));
        if (array != nullptr) {
            memcpy(array, blob, len);
            env->ReleasePrimitiveArrayCritical(ret, array, 0);
        }
    }
    return ret;
}

jint readInt(jlong nativePtr) {
    const Parcel *parcel = toParcel(nativePtr);
    return parcel != nullptr ? parcel->readInt32() : 0;
}

jlong readLong(jlong nativePtr) {
    const Parcel *parcel = toParcel(nativePtr);
    return parcel != nullptr ? parcel->readInt64() : 0;
}

jfloat readFloat(jlong nativePtr) {
    const Parcel *parcel = toParcel(nativePtr);
    return parcel != nullptr ? parcel->readFloat() : 0;
}

jdouble readDouble(jlong nativePtr) {
    const Parcel *parcel = toParcel(nativePtr);
    return parcel != nullptr ? parcel->readDouble() : 0;
}

jstring readString8(JNIEnv *env, jclass, jlong nativePtr) {
    const Parcel *parcel = toParcel(nativePtr);
    if (parcel == nullptr) return nullptr;
    size_t len;
    const char *str = parcel->readString8Inplace(&len);
    return str != nullptr ? env->NewStringUTF(str) : nullptr;
}

jstring readString16(JNIEnv *env, jclass, jlong nativePtr) {
    const Parcel *parcel = toParcel(nativePtr);
    if (parcel == nullptr) return nullptr;
    size_t len;
    const char16_t *str = parcel->readString16Inplace(&len);
    return str != nullptr ? env->NewString(reinterpret_cast<const jchar *>(str), len) : nullptr;
}

jobject readStrongBinder(JNIEnv *env, jclass, jlong nativePtr) {
    const Parcel *parcel = toParcel(nativePtr);
    return parcel != nullptr ? parcel->readStrongBinder(env) : nullptr;
}

jobject readFileDescriptor(JNIEnv *env, jclass, jlong nativePtr) {
    const Parcel *parcel = toParcel(nativePtr);
    if (parcel == nullptr) return nullptr;
    int fd = parcel->readFileDescriptor();
    if (fd < 0) return nullptr;
    // The parcel keeps its own; the caller gets a dup.
    fd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if (fd < 0) return nullptr;
    jobject jifd = jniCreateFileDescriptor(env, fd);
    if (jifd == nullptr) close(fd);
    return jifd;
}

jlong create(JNIEnv *, jclass) {
    return reinterpret_cast<jlong>(new Parcel());
}

void freeBuffer(JNIEnv *, jclass, jlong nativePtr) {
    if (Parcel *parcel = toParcel(nativePtr)) parcel->freeData();
}

void destroy(JNIEnv *, jclass, jlong nativePtr) {
    delete toParcel(nativePtr);
}

jbyteArray marshall(JNIEnv *env, jclass, jlong nativePtr) {
    const Parcel *parcel = toParcel(nativePtr);
    if (parcel == nullptr) return nullptr;
    if (parcel->objectsCount() != 0) {
        jniThrowException(env, "java/lang/RuntimeException",
                          "Tried to marshall a Parcel that contains objects (binders or FDs).");
        return nullptr;
    }
    jbyteArray ret = env->NewByteArray(parcel->dataSize());
    if (ret != nullptr) {
        // dataSize() counts a position beyond the data, which AOSP copies from past its buffer; the bytes beyond what
        // the buffer holds stay zero here.
        const size_t held = std::min(parcel->dataSize(), parcel->dataCapacity());
        if (held > 0) env->SetByteArrayRegion(ret, 0, held, reinterpret_cast<const jbyte *>(parcel->data()));
    }
    return ret;
}

void unmarshall(JNIEnv *env, jclass, jlong nativePtr, jbyteArray data, jint offset, jint length) {
    Parcel *parcel = toParcel(nativePtr);
    if (parcel == nullptr || length < 0) return;
    // AOSP copies from the array without checking the range: Minato throws as for writeByteArray instead.
    if (!checkRange(env, data, offset, length)) return;
    // The parcel is sized before the bytes come, outside any critical section: shrinking it may release objects,
    // and a binder's release deletes a global reference, which no critical section allows.
    const status_t err = parcel->setDataSize(length);
    if (err != NO_ERROR) {
        signalExceptionForError(env, err);
        return;
    }
    parcel->setDataPosition(0);
    void *raw = parcel->writeInplace(length);
    if (raw == nullptr) {
        signalExceptionForError(env, NO_MEMORY);
        return;
    }
    env->GetByteArrayRegion(data, offset, length, static_cast<jbyte *>(raw));
}

jint compareData(JNIEnv *, jclass, jlong thisNativePtr, jlong otherNativePtr) {
    const Parcel *thisParcel = toParcel(thisNativePtr);
    LOG_ALWAYS_FATAL_IF(thisParcel == nullptr, "Should not be null");
    const Parcel *otherParcel = toParcel(otherNativePtr);
    LOG_ALWAYS_FATAL_IF(otherParcel == nullptr, "Should not be null");
    return thisParcel->compareData(*otherParcel);
}

jboolean compareDataInRange(JNIEnv *env, jclass, jlong thisNativePtr, jint thisOffset, jlong otherNativePtr,
                            jint otherOffset, jint length) {
    const Parcel *thisParcel = toParcel(thisNativePtr);
    LOG_ALWAYS_FATAL_IF(thisParcel == nullptr, "Should not be null");
    const Parcel *otherParcel = toParcel(otherNativePtr);
    LOG_ALWAYS_FATAL_IF(otherParcel == nullptr, "Should not be null");
    int result;
    const status_t err = thisParcel->compareDataInRange(static_cast<size_t>(thisOffset), *otherParcel,
                                                        static_cast<size_t>(otherOffset), static_cast<size_t>(length),
                                                        &result);
    if (err != NO_ERROR) {
        signalExceptionForError(env, err);
        return JNI_FALSE;
    }
    return result == 0 ? JNI_TRUE : JNI_FALSE;
}

void appendFrom(JNIEnv *env, jclass, jlong thisNativePtr, jlong otherNativePtr, jint offset, jint length) {
    Parcel *thisParcel = toParcel(thisNativePtr);
    const Parcel *otherParcel = toParcel(otherNativePtr);
    if (thisParcel == nullptr || otherParcel == nullptr) return;
    const status_t err =
            thisParcel->appendFrom(otherParcel, static_cast<size_t>(offset), static_cast<size_t>(length));
    if (err != NO_ERROR) signalExceptionForError(env, err);
}

jboolean hasBinders(JNIEnv *env, jclass, jlong nativePtr) {
    const Parcel *parcel = toParcel(nativePtr);
    if (parcel == nullptr) return JNI_FALSE;
    bool result;
    const status_t err = parcel->hasBinders(&result);
    if (err != NO_ERROR) {
        signalExceptionForError(env, err);
        return JNI_FALSE;
    }
    return result ? JNI_TRUE : JNI_FALSE;
}

jboolean hasBindersInRange(JNIEnv *env, jclass, jlong nativePtr, jint offset, jint length) {
    const Parcel *parcel = toParcel(nativePtr);
    if (parcel == nullptr) return JNI_FALSE;
    bool result;
    const status_t err =
            parcel->hasBindersInRange(static_cast<size_t>(offset), static_cast<size_t>(length), &result);
    if (err != NO_ERROR) {
        signalExceptionForError(env, err);
        return JNI_FALSE;
    }
    return result ? JNI_TRUE : JNI_FALSE;
}

jboolean hasFileDescriptors(jlong nativePtr) {
    const Parcel *parcel = toParcel(nativePtr);
    return parcel != nullptr && parcel->hasFileDescriptors() ? JNI_TRUE : JNI_FALSE;
}

jboolean hasFileDescriptorsInRange(JNIEnv *env, jclass, jlong nativePtr, jint offset, jint length) {
    const Parcel *parcel = toParcel(nativePtr);
    if (parcel == nullptr) return JNI_FALSE;
    bool result;
    const status_t err =
            parcel->hasFileDescriptorsInRange(static_cast<size_t>(offset), static_cast<size_t>(length), &result);
    if (err != NO_ERROR) {
        signalExceptionForError(env, err);
        return JNI_FALSE;
    }
    return result ? JNI_TRUE : JNI_FALSE;
}

void writeInterfaceToken(JNIEnv *env, jclass, jlong nativePtr, jstring name) {
    if (Parcel *parcel = toParcel(nativePtr)) {
        const std::u16string descriptor = descriptorOf(env, name);
        const int32_t policy = threadStrictModePolicy(env);
        if (env->ExceptionCheck()) return;
        parcel->writeInterfaceToken(descriptor.data(), descriptor.size(), policy);
    }
}

void enforceInterface(JNIEnv *env, jclass, jlong nativePtr, jstring name) {
    if (const Parcel *parcel = toParcel(nativePtr)) {
        const std::u16string descriptor = descriptorOf(env, name);
        const int32_t oldPolicy = threadStrictModePolicy(env);
        if (env->ExceptionCheck()) return;
        int32_t newPolicy;
        const bool isValid = parcel->enforceInterface(descriptor.data(), descriptor.size(), &newPolicy);
        // libbinder sets the thread's policy as it reads it, before the descriptor is checked.
        env->CallStaticVoidMethod(gBinderOffsets.clazz, gBinderOffsets.setThreadStrictModePolicy, newPolicy);
        if (env->ExceptionCheck()) return;
        if (isValid) {
            // Keep StrictMode's thread policy in step, as AOSP's set_dalvik_blockguard_policy does.
            if (oldPolicy != newPolicy) {
                env->CallStaticVoidMethod(gStrictModeCallbackOffsets.clazz, gStrictModeCallbackOffsets.callback,
                                          newPolicy);
            }
            return;
        }
    }
    jniThrowException(env, "java/lang/SecurityException", "Binder invocation to an incorrect interface");
}

jlong getGlobalAllocSize(JNIEnv *, jclass) {
    return Parcel::globalAllocSize();
}

jlong getGlobalAllocCount(JNIEnv *, jclass) {
    return Parcel::globalAllocCount();
}

jlong getOpenAshmemSize(jlong) {
    // Blobs stay in the data: a parcel holds no ashmem.
    return 0;
}

jint readCallingWorkSourceUid(jlong nativePtr) {
    const Parcel *parcel = toParcel(nativePtr);
    return parcel != nullptr ? parcel->readCallingWorkSourceUid() : -1;
}

jboolean replaceCallingWorkSourceUid(jlong nativePtr, jint uid) {
    Parcel *parcel = toParcel(nativePtr);
    return parcel != nullptr && parcel->replaceCallingWorkSourceUid(uid) ? JNI_TRUE : JNI_FALSE;
}

// clang-format off
const JNINativeMethod gParcelMethods[] = {
        // @CriticalNative
        {"nativeMarkSensitive", "(J)V", reinterpret_cast<void *>(markSensitive)},
        // @FastNative
        {"nativeMarkForBinder", "(JLandroid/os/IBinder;)V", reinterpret_cast<void *>(markForBinder)},
        // @CriticalNative
        {"nativeIsForRpc", "(J)Z", reinterpret_cast<void *>(isForRpc)},
        // @CriticalNative
        {"nativeDataSize", "(J)I", reinterpret_cast<void *>(dataSize)},
        // @CriticalNative
        {"nativeDataAvail", "(J)I", reinterpret_cast<void *>(dataAvail)},
        // @CriticalNative
        {"nativeDataPosition", "(J)I", reinterpret_cast<void *>(dataPosition)},
        // @CriticalNative
        {"nativeDataCapacity", "(J)I", reinterpret_cast<void *>(dataCapacity)},
        // @FastNative
        {"nativeSetDataSize", "(JI)V", reinterpret_cast<void *>(setDataSize)},
        // @CriticalNative
        {"nativeSetDataPosition", "(JI)V", reinterpret_cast<void *>(setDataPosition)},
        // @FastNative
        {"nativeSetDataCapacity", "(JI)V", reinterpret_cast<void *>(setDataCapacity)},

        // @CriticalNative
        {"nativePushAllowFds", "(JZ)Z", reinterpret_cast<void *>(pushAllowFds)},
        // @CriticalNative
        {"nativeRestoreAllowFds", "(JZ)V", reinterpret_cast<void *>(restoreAllowFds)},

        {"nativeWriteByteArray", "(J[BII)V", reinterpret_cast<void *>(writeByteArray)},
        {"nativeWriteBlob", "(J[BII)V", reinterpret_cast<void *>(writeBlob)},
        // @CriticalNative
        {"nativeWriteInt", "(JI)I", reinterpret_cast<void *>(writeInt)},
        // @CriticalNative
        {"nativeWriteLong", "(JJ)I", reinterpret_cast<void *>(writeLong)},
        // @CriticalNative
        {"nativeWriteFloat", "(JF)I", reinterpret_cast<void *>(writeFloat)},
        // @CriticalNative
        {"nativeWriteDouble", "(JD)I", reinterpret_cast<void *>(writeDouble)},
        {"nativeSignalExceptionForError", "(I)V", reinterpret_cast<void *>(nativeSignalExceptionForError)},
        // @FastNative
        {"nativeWriteString8", "(JLjava/lang/String;)V", reinterpret_cast<void *>(writeString8)},
        // @FastNative
        {"nativeWriteString16", "(JLjava/lang/String;)V", reinterpret_cast<void *>(writeString16)},
        // @FastNative
        {"nativeWriteStrongBinder", "(JLandroid/os/IBinder;)V", reinterpret_cast<void *>(writeStrongBinder)},
        // @FastNative
        {"nativeWriteFileDescriptor", "(JLjava/io/FileDescriptor;)V", reinterpret_cast<void *>(writeFileDescriptor)},

        {"nativeCreateByteArray", "(J)[B", reinterpret_cast<void *>(createByteArray)},
        {"nativeReadByteArray", "(J[BI)Z", reinterpret_cast<void *>(readByteArray)},
        {"nativeReadBlob", "(J)[B", reinterpret_cast<void *>(readBlob)},
        // @CriticalNative
        {"nativeReadInt", "(J)I", reinterpret_cast<void *>(readInt)},
        // @CriticalNative
        {"nativeReadLong", "(J)J", reinterpret_cast<void *>(readLong)},
        // @CriticalNative
        {"nativeReadFloat", "(J)F", reinterpret_cast<void *>(readFloat)},
        // @CriticalNative
        {"nativeReadDouble", "(J)D", reinterpret_cast<void *>(readDouble)},
        // @FastNative
        {"nativeReadString8", "(J)Ljava/lang/String;", reinterpret_cast<void *>(readString8)},
        // @FastNative
        {"nativeReadString16", "(J)Ljava/lang/String;", reinterpret_cast<void *>(readString16)},
        // @FastNative
        {"nativeReadStrongBinder", "(J)Landroid/os/IBinder;", reinterpret_cast<void *>(readStrongBinder)},
        // @FastNative
        {"nativeReadFileDescriptor", "(J)Ljava/io/FileDescriptor;", reinterpret_cast<void *>(readFileDescriptor)},

        {"nativeCreate", "()J", reinterpret_cast<void *>(create)},
        {"nativeFreeBuffer", "(J)V", reinterpret_cast<void *>(freeBuffer)},
        {"nativeDestroy", "(J)V", reinterpret_cast<void *>(destroy)},

        {"nativeMarshall", "(J)[B", reinterpret_cast<void *>(marshall)},
        {"nativeUnmarshall", "(J[BII)V", reinterpret_cast<void *>(unmarshall)},
        {"nativeCompareData", "(JJ)I", reinterpret_cast<void *>(compareData)},
        {"nativeCompareDataInRange", "(JIJII)Z", reinterpret_cast<void *>(compareDataInRange)},
        {"nativeAppendFrom", "(JJII)V", reinterpret_cast<void *>(appendFrom)},
        // @CriticalNative
        {"nativeHasFileDescriptors", "(J)Z", reinterpret_cast<void *>(hasFileDescriptors)},
        {"nativeHasFileDescriptorsInRange", "(JII)Z", reinterpret_cast<void *>(hasFileDescriptorsInRange)},

        {"nativeHasBinders", "(J)Z", reinterpret_cast<void *>(hasBinders)},
        {"nativeHasBindersInRange", "(JII)Z", reinterpret_cast<void *>(hasBindersInRange)},
        {"nativeWriteInterfaceToken", "(JLjava/lang/String;)V", reinterpret_cast<void *>(writeInterfaceToken)},
        {"nativeEnforceInterface", "(JLjava/lang/String;)V", reinterpret_cast<void *>(enforceInterface)},

        {"getGlobalAllocSize", "()J", reinterpret_cast<void *>(getGlobalAllocSize)},
        {"getGlobalAllocCount", "()J", reinterpret_cast<void *>(getGlobalAllocCount)},

        // @CriticalNative
        {"nativeGetOpenAshmemSize", "(J)J", reinterpret_cast<void *>(getOpenAshmemSize)},

        // @CriticalNative
        {"nativeReadCallingWorkSourceUid", "(J)I", reinterpret_cast<void *>(readCallingWorkSourceUid)},
        // @CriticalNative
        {"nativeReplaceCallingWorkSourceUid", "(JI)Z", reinterpret_cast<void *>(replaceCallingWorkSourceUid)},
};
// clang-format on

}  // namespace

int register_android_os_Parcel(JNIEnv *env) {
    JavaVM *vm;
    env->GetJavaVM(&vm);
    Parcel::setJavaVM(vm);

    jclass binder = FindClassOrDie(env, "android/os/Binder");
    gBinderOffsets.clazz = MakeGlobalRefOrDie(env, binder);
    gBinderOffsets.getThreadStrictModePolicy = GetStaticMethodIDOrDie(env, binder, "getThreadStrictModePolicy", "()I");
    gBinderOffsets.setThreadStrictModePolicy =
            GetStaticMethodIDOrDie(env, binder, "setThreadStrictModePolicy", "(I)V");
    jclass strictMode = FindClassOrDie(env, "android/os/StrictMode");
    gStrictModeCallbackOffsets.clazz = MakeGlobalRefOrDie(env, strictMode);
    gStrictModeCallbackOffsets.callback =
            GetStaticMethodIDOrDie(env, strictMode, "onBinderStrictModePolicyChange", "(I)V");

    return RegisterMethodsOrDie(env, "android/os/Parcel", gParcelMethods, NELEM(gParcelMethods));
}

}  // namespace android
