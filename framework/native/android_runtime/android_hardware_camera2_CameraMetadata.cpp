// Ours in place of AOSP's CameraMetadataNative natives in core/jni (ADR-0073 decision 4, step a): the same answers
// and exceptions, written directly over libcamera_metadata's C API (system/media) instead of frameworks/av's C++
// CameraMetadata, which is not fetched. A Java object's mMetadataPtr points to a Holder, whose buffer grows as
// entries are added, as CameraMetadata::update grows its own. Minato has no vendor tags (its camera service lists
// none, as a device without a vendor tag provider), so every vendor lookup answers as AOSP's does without them.
// Parcels stay in the process (camera2's service is local): a buffer travels as one byte array of its compact copy.
#define LOG_TAG "CameraMetadata-JNI"

#include <camera_metadata_hidden.h>
#include <errno.h>
#include <jni.h>
#include <log/log.h>
#include <nativehelper/JNIHelp.h>
#include <nativehelper/ScopedPrimitiveArray.h>
#include <nativehelper/ScopedUtfChars.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <system/camera_metadata.h>
#include <unistd.h>

#include <mutex>
#include <string>

#include "core_jni_helpers.h"

namespace android {
namespace {

constexpr const char *kClassName = "android/hardware/camera2/impl/CameraMetadataNative";

// One metadata buffer, or none (an empty object, as AOSP's CameraMetadata with a null buffer). The lock guards the
// buffer against the Java side's unsynchronized calls (nativeAllocate, nativeUpdate, nativeGetTagFromKey...).
struct Holder {
    std::mutex lock;
    camera_metadata_t *buffer = nullptr;
    ~Holder() {
        if (buffer != nullptr) free_camera_metadata(buffer);
    }
};

Holder *holder_of(jlong ptr) {
    return reinterpret_cast<Holder *>(static_cast<uintptr_t>(ptr));
}

Holder *holder_or_throw(JNIEnv *env, jlong ptr) {
    Holder *holder = holder_of(ptr);
    if (holder == nullptr) {
        jniThrowException(env, "java/lang/IllegalStateException", "Metadata object was already closed");
    }
    return holder;
}

size_t type_size(int type) {
    return type >= 0 && type < NUM_TYPES ? camera_metadata_type_size[type] : 0;
}

// Sets `count` values of `tag` from `data`, growing the buffer when it lacks room, as CameraMetadata::update does:
// 0, or -EINVAL for data of another type or count, -ENOMEM.
int update(Holder *holder, uint32_t tag, const void *data, size_t count) {
    const int type = get_local_camera_metadata_tag_type(tag, holder->buffer);
    if (type < 0) return -EINVAL;
    const size_t data_size = calculate_camera_metadata_entry_data_size(static_cast<uint8_t>(type), count);
    if (holder->buffer == nullptr) {
        holder->buffer = allocate_camera_metadata(10, data_size + 10);
        if (holder->buffer == nullptr) return -ENOMEM;
    }
    camera_metadata_entry_t entry;
    if (find_camera_metadata_entry(holder->buffer, tag, &entry) == 0) {
        // In place when the old entry's room holds the new data, else delete and add again below.
        const size_t old_size = calculate_camera_metadata_entry_data_size(entry.type, entry.count);
        if (data_size <= old_size || get_camera_metadata_data_count(holder->buffer) + data_size - old_size <=
                                             get_camera_metadata_data_capacity(holder->buffer)) {
            return update_camera_metadata_entry(holder->buffer, entry.index, data, count, nullptr) == 0 ? 0
                                                                                                        : -EINVAL;
        }
        if (delete_camera_metadata_entry(holder->buffer, entry.index) != 0) return -EINVAL;
    }
    const size_t entries = get_camera_metadata_entry_count(holder->buffer);
    const size_t used = get_camera_metadata_data_count(holder->buffer);
    if (entries + 1 > get_camera_metadata_entry_capacity(holder->buffer) ||
        used + data_size > get_camera_metadata_data_capacity(holder->buffer)) {
        camera_metadata_t *grown = allocate_camera_metadata((entries + 1) * 2, (used + data_size) * 2);
        if (grown == nullptr) return -ENOMEM;
        if (append_camera_metadata(grown, holder->buffer) != 0) {
            free_camera_metadata(grown);
            return -ENOMEM;
        }
        set_camera_metadata_vendor_id(grown, get_camera_metadata_vendor_id(holder->buffer));
        free_camera_metadata(holder->buffer);
        holder->buffer = grown;
    }
    return add_camera_metadata_entry(holder->buffer, tag, data, count) == 0 ? 0 : -EINVAL;
}

// CameraMetadata::getTagFromName without vendor tags: the section whose name and a '.' begin the key, then the tag
// of that section with the rest as its name.
bool tag_from_name(const char *name, uint32_t *tag) {
    for (uint32_t section = 0; section < ANDROID_SECTION_COUNT; section++) {
        const char *section_name = camera_metadata_section_names[section];
        const size_t length = strlen(section_name);
        if (strncmp(name, section_name, length) != 0 || name[length] != '.') continue;
        const char *rest = name + length + 1;
        for (uint32_t candidate = camera_metadata_section_bounds[section][0];
             candidate < camera_metadata_section_bounds[section][1]; candidate++) {
            const char *tag_name = get_camera_metadata_tag_name(candidate);
            if (tag_name != nullptr && strcmp(tag_name, rest) == 0) {
                *tag = candidate;
                return true;
            }
        }
    }
    return false;
}

jlong CameraMetadata_allocate(JNIEnv *, jclass) {
    return static_cast<jlong>(reinterpret_cast<uintptr_t>(new Holder));
}

jlong CameraMetadata_allocateCopy(JNIEnv *env, jclass, jlong other) {
    Holder *source = holder_or_throw(env, other);
    if (source == nullptr) return 0;
    auto *copy = new Holder;
    std::lock_guard<std::mutex> guard(source->lock);
    if (source->buffer != nullptr) copy->buffer = clone_camera_metadata(source->buffer);
    return static_cast<jlong>(reinterpret_cast<uintptr_t>(copy));
}

jboolean CameraMetadata_isEmpty(JNIEnv *env, jclass, jlong ptr) {
    Holder *holder = holder_or_throw(env, ptr);
    if (holder == nullptr) return JNI_TRUE;
    std::lock_guard<std::mutex> guard(holder->lock);
    return holder->buffer == nullptr || get_camera_metadata_entry_count(holder->buffer) == 0;
}

jint CameraMetadata_getEntryCount(JNIEnv *env, jclass, jlong ptr) {
    Holder *holder = holder_or_throw(env, ptr);
    if (holder == nullptr) return 0;
    std::lock_guard<std::mutex> guard(holder->lock);
    return holder->buffer == nullptr ? 0 : static_cast<jint>(get_camera_metadata_entry_count(holder->buffer));
}

jlong CameraMetadata_getBufferSize(JNIEnv *env, jclass, jlong ptr) {
    Holder *holder = holder_or_throw(env, ptr);
    if (holder == nullptr) return 0;
    std::lock_guard<std::mutex> guard(holder->lock);
    return holder->buffer == nullptr ? 0 : static_cast<jlong>(get_camera_metadata_size(holder->buffer));
}

// Every entry of src into dst; nothing when either is empty, as AOSP's.
void CameraMetadata_update(JNIEnv *env, jclass, jlong dst, jlong src) {
    Holder *target = holder_or_throw(env, dst);
    if (target == nullptr) return;
    Holder *source = holder_or_throw(env, src);
    if (source == nullptr || target == source) return;
    std::scoped_lock guard(target->lock, source->lock);
    if (target->buffer == nullptr || get_camera_metadata_entry_count(target->buffer) == 0) return;
    if (source->buffer == nullptr) return;
    const size_t count = get_camera_metadata_entry_count(source->buffer);
    for (size_t index = 0; index < count; index++) {
        camera_metadata_ro_entry_t entry;
        if (get_camera_metadata_ro_entry(source->buffer, index, &entry) != 0) {
            ALOGE("%s: Failed to retrieve source metadata!", __func__);
            return;
        }
        update(target, entry.tag, entry.data.u8, entry.count);
    }
}

// Idempotent: Java clears its pointer after the first close.
void CameraMetadata_close(JNIEnv *, jclass, jlong ptr) {
    delete holder_of(ptr);
}

void CameraMetadata_swap(JNIEnv *, jclass, jlong ptr, jlong other) {
    Holder *holder = holder_of(ptr);
    Holder *other_holder = holder_of(other);
    if (holder == nullptr || other_holder == nullptr || holder == other_holder) return;
    std::scoped_lock guard(holder->lock, other_holder->lock);
    std::swap(holder->buffer, other_holder->buffer);
}

jbyteArray CameraMetadata_readValues(JNIEnv *env, jclass, jint tag, jlong ptr) {
    Holder *holder = holder_or_throw(env, ptr);
    if (holder == nullptr) return nullptr;
    std::lock_guard<std::mutex> guard(holder->lock);
    const int type = get_local_camera_metadata_tag_type(static_cast<uint32_t>(tag), holder->buffer);
    if (type < 0) {
        jniThrowExceptionFmt(env, "java/lang/IllegalArgumentException", "Tag (%d) did not have a type", tag);
        return nullptr;
    }
    camera_metadata_ro_entry_t entry;
    if (holder->buffer == nullptr ||
        find_camera_metadata_ro_entry(holder->buffer, static_cast<uint32_t>(tag), &entry) != 0) {
        return nullptr;
    }
    const jsize bytes = static_cast<jsize>(entry.count * type_size(type));
    jbyteArray array = env->NewByteArray(bytes);
    if (array == nullptr) return nullptr;
    env->SetByteArrayRegion(array, 0, bytes, reinterpret_cast<const jbyte *>(entry.data.u8));
    return array;
}

void CameraMetadata_writeValues(JNIEnv *env, jclass, jint tag, jbyteArray source, jlong ptr) {
    Holder *holder = holder_or_throw(env, ptr);
    if (holder == nullptr) return;
    std::lock_guard<std::mutex> guard(holder->lock);
    const uint32_t key = static_cast<uint32_t>(tag);
    const int type = get_local_camera_metadata_tag_type(key, holder->buffer);
    if (type < 0) {
        jniThrowExceptionFmt(env, "java/lang/IllegalArgumentException", "Tag (%d) did not have a type", tag);
        return;
    }
    int result = 0;
    if (source == nullptr) {
        camera_metadata_entry_t entry;
        if (holder->buffer != nullptr && find_camera_metadata_entry(holder->buffer, key, &entry) == 0) {
            result = delete_camera_metadata_entry(holder->buffer, entry.index) == 0 ? 0 : -EINVAL;
        }
    } else {
        ScopedByteArrayRO bytes(env, source);
        if (bytes.get() == nullptr) return;
        const size_t size = type_size(type);
        if (bytes.size() % size != 0) {
            jniThrowException(env, "java/lang/IllegalArgumentException", "Src byte array was poorly formed");
            return;
        }
        result = update(holder, key, bytes.get(), bytes.size() / size);
    }
    if (result == -EINVAL) {
        jniThrowException(env, "java/lang/IllegalArgumentException", "Src byte array was poorly formed");
    } else if (result != 0) {
        jniThrowExceptionFmt(env, "java/lang/IllegalStateException",
                             "Unknown error (%d) while trying to update metadata", result);
    }
}

// The dump to the log at debug level, line by line, as AOSP's: dump_camera_metadata writes to a pipe that a thread
// of ours drains, so that a buffer larger than the pipe never blocks.
struct DumpWriter {
    int fd;
    const camera_metadata_t *buffer;
};

void *dump_writer(void *argument) {
    auto *writer = static_cast<DumpWriter *>(argument);
    dump_camera_metadata(writer->buffer, writer->fd, 2);
    close(writer->fd);
    return nullptr;
}

void CameraMetadata_dump(JNIEnv *env, jclass, jlong ptr) {
    Holder *holder = holder_or_throw(env, ptr);
    if (holder == nullptr) return;
    std::lock_guard<std::mutex> guard(holder->lock);
    if (holder->buffer == nullptr) return;
    int fds[2];
    if (pipe(fds) != 0) {
        jniThrowExceptionFmt(env, "java/io/IOException", "Failed to create pipe: %s", strerror(errno));
        return;
    }
    DumpWriter writer{fds[1], holder->buffer};
    pthread_t thread;
    if (pthread_create(&thread, nullptr, dump_writer, &writer) != 0) {
        close(fds[0]);
        close(fds[1]);
        jniThrowException(env, "java/io/IOException", "Failed to start the dump");
        return;
    }
    std::string line;
    char chunk[512];
    ssize_t got;
    while ((got = read(fds[0], chunk, sizeof chunk)) > 0) {
        for (ssize_t index = 0; index < got; index++) {
            if (chunk[index] == '\n') {
                ALOGD("%s", line.c_str());
                line.clear();
            } else {
                line += chunk[index];
            }
        }
    }
    if (!line.empty()) ALOGD("%s", line.c_str());
    close(fds[0]);
    pthread_join(thread, nullptr);
}

// The compact buffer as one byte array; an empty object writes an empty array.
void CameraMetadata_writeToParcel(JNIEnv *env, jclass, jobject parcel, jlong ptr) {
    Holder *holder = holder_or_throw(env, ptr);
    if (holder == nullptr) return;
    if (parcel == nullptr) {
        jniThrowNullPointerException(env, "parcel");
        return;
    }
    jbyteArray array;
    {
        std::lock_guard<std::mutex> guard(holder->lock);
        const size_t size = holder->buffer == nullptr ? 0 : get_camera_metadata_compact_size(holder->buffer);
        array = env->NewByteArray(static_cast<jsize>(size));
        if (array == nullptr) return;
        if (size > 0) {
            ScopedByteArrayRW bytes(env, array);
            copy_camera_metadata(bytes.get(), size, holder->buffer);
        }
    }
    jclass parcel_class = env->GetObjectClass(parcel);
    jmethodID write = env->GetMethodID(parcel_class, "writeByteArray", "([B)V");
    env->CallVoidMethod(parcel, write, array);
}

void CameraMetadata_readFromParcel(JNIEnv *env, jclass, jobject parcel, jlong ptr) {
    Holder *holder = holder_or_throw(env, ptr);
    if (holder == nullptr) return;
    if (parcel == nullptr) {
        jniThrowNullPointerException(env, "parcel");
        return;
    }
    jclass parcel_class = env->GetObjectClass(parcel);
    jmethodID create = env->GetMethodID(parcel_class, "createByteArray", "()[B");
    auto array = static_cast<jbyteArray>(env->CallObjectMethod(parcel, create));
    if (env->ExceptionCheck()) return;
    camera_metadata_t *buffer = nullptr;
    if (array != nullptr && env->GetArrayLength(array) > 0) {
        // A Java array's bytes need not be aligned as a buffer must be: into an aligned copy first.
        const size_t size = static_cast<size_t>(env->GetArrayLength(array));
        void *aligned = nullptr;
        if (posix_memalign(&aligned, get_camera_metadata_alignment(), size) != 0) {
            jniThrowException(env, "java/lang/OutOfMemoryError", "metadata from a parcel");
            return;
        }
        env->GetByteArrayRegion(array, 0, static_cast<jsize>(size), static_cast<jbyte *>(aligned));
        const auto *read = static_cast<const camera_metadata_t *>(aligned);
        if (validate_camera_metadata_structure(read, &size) != 0) {
            free(aligned);
            jniThrowException(env, "java/lang/IllegalStateException",
                              "Failed to read from parcel (error code -22)");
            return;
        }
        buffer = clone_camera_metadata(read);
        free(aligned);
    }
    std::lock_guard<std::mutex> guard(holder->lock);
    if (holder->buffer != nullptr) free_camera_metadata(holder->buffer);
    holder->buffer = buffer;
}

void CameraMetadata_setVendorId(JNIEnv *env, jclass, jlong ptr, jlong vendor) {
    Holder *holder = holder_or_throw(env, ptr);
    if (holder == nullptr) return;
    std::lock_guard<std::mutex> guard(holder->lock);
    if (holder->buffer == nullptr) holder->buffer = allocate_camera_metadata(10, 0);
    if (holder->buffer != nullptr) set_camera_metadata_vendor_id(holder->buffer, static_cast<uint64_t>(vendor));
}

// No vendor tags: null, as AOSP's answers without a descriptor.
jobject CameraMetadata_getAllVendorKeys(JNIEnv *, jclass, jlong, jclass) {
    return nullptr;
}

jint tag_from_key(JNIEnv *env, jstring key) {
    ScopedUtfChars name(env, key);
    if (name.c_str() == nullptr) return 0;
    uint32_t tag = 0;
    if (!tag_from_name(name.c_str(), &tag)) {
        jniThrowExceptionFmt(env, "java/lang/IllegalArgumentException", "Could not find tag for key '%s')",
                             name.c_str());
    }
    return static_cast<jint>(tag);
}

jint CameraMetadata_getTagFromKey(JNIEnv *env, jclass, jstring key, jlong) {
    return tag_from_key(env, key);
}

jint CameraMetadata_getTagFromKeyLocal(JNIEnv *env, jclass, jlong, jstring key) {
    return tag_from_key(env, key);
}

jint type_from_tag(JNIEnv *env, jint tag) {
    const int type = get_local_camera_metadata_tag_type_vendor_id(static_cast<uint32_t>(tag),
                                                                  CAMERA_METADATA_INVALID_VENDOR_ID);
    if (type < 0) {
        jniThrowExceptionFmt(env, "java/lang/IllegalArgumentException", "Tag (%d) did not have a type", tag);
        return -1;
    }
    return type;
}

jint CameraMetadata_getTypeFromTag(JNIEnv *env, jclass, jint tag, jlong) {
    return type_from_tag(env, tag);
}

jint CameraMetadata_getTypeFromTagLocal(JNIEnv *env, jclass, jlong, jint tag) {
    return type_from_tag(env, tag);
}

// The camera service has no vendor tags: OK, as AOSP's for a device without them.
jint CameraMetadata_setupGlobalVendorTagDescriptor(JNIEnv *, jclass) {
    return 0;
}

const JNINativeMethod gMethods[] = {
        {"nativeSetVendorId", "(JJ)V", (void *)CameraMetadata_setVendorId},
        {"nativeGetTagFromKey", "(Ljava/lang/String;J)I", (void *)CameraMetadata_getTagFromKey},
        {"nativeGetTypeFromTag", "(IJ)I", (void *)CameraMetadata_getTypeFromTag},
        {"nativeSetupGlobalVendorTagDescriptor", "()I", (void *)CameraMetadata_setupGlobalVendorTagDescriptor},
        {"nativeAllocate", "()J", (void *)CameraMetadata_allocate},
        {"nativeAllocateCopy", "(J)J", (void *)CameraMetadata_allocateCopy},
        {"nativeUpdate", "(JJ)V", (void *)CameraMetadata_update},
        {"nativeIsEmpty", "(J)Z", (void *)CameraMetadata_isEmpty},
        {"nativeGetEntryCount", "(J)I", (void *)CameraMetadata_getEntryCount},
        {"nativeGetBufferSize", "(J)J", (void *)CameraMetadata_getBufferSize},
        {"nativeClose", "(J)V", (void *)CameraMetadata_close},
        {"nativeSwap", "(JJ)V", (void *)CameraMetadata_swap},
        {"nativeGetTagFromKeyLocal", "(JLjava/lang/String;)I", (void *)CameraMetadata_getTagFromKeyLocal},
        {"nativeGetTypeFromTagLocal", "(JI)I", (void *)CameraMetadata_getTypeFromTagLocal},
        {"nativeReadValues", "(IJ)[B", (void *)CameraMetadata_readValues},
        {"nativeWriteValues", "(I[BJ)V", (void *)CameraMetadata_writeValues},
        {"nativeDump", "(J)V", (void *)CameraMetadata_dump},
        {"nativeGetAllVendorKeys", "(JLjava/lang/Class;)Ljava/util/ArrayList;",
         (void *)CameraMetadata_getAllVendorKeys},
        {"nativeReadFromParcel", "(Landroid/os/Parcel;J)V", (void *)CameraMetadata_readFromParcel},
        {"nativeWriteToParcel", "(Landroid/os/Parcel;J)V", (void *)CameraMetadata_writeToParcel},
};

}  // namespace

int register_android_hardware_camera2_CameraMetadata(JNIEnv *env) {
    return RegisterMethodsOrDie(env, kClassName, gMethods, NELEM(gMethods));
}

}  // namespace android
