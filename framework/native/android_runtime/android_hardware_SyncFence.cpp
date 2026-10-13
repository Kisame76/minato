// Ours in place of AOSP's core/jni/android_hardware_SyncFence.cpp (the graphics breadth's G2): SyncFence's natives on
// a file descriptor of our own counting instead of libui's Fence, whose answers they follow at android-15.0.0_r1:
// - a fence holds its descriptor (-1 for none) and closes it at its last reference, as Fence's sp<> counts it;
// - isValid: a descriptor; wait, as Fence::wait over libsync's sync_wait: OK at once without one, else poll for
//   POLLIN with the timeout as AOSP's JNI hands it on, the nanoseconds in an int, which Fence::wait(int) takes as
//   milliseconds [inferred: libui is not fetched];
// - getSignalTime: SIGNAL_TIME_INVALID without one, and for every descriptor, as Minato has no sync files: Fence logs
//   what libsync's sync_file_info answers for a file that is no sync file and returns SIGNAL_TIME_INVALID.
#define LOG_TAG "Fence"

#include <errno.h>
#include <jni.h>
#include <nativehelper/JNIHelp.h>
#include <poll.h>
#include <stdint.h>
#include <unistd.h>

#include <atomic>

#include <log/log.h>

#include "core_jni_helpers.h"

namespace android {
namespace {

// SyncFence.SIGNAL_TIME_INVALID, Fence's.
constexpr jlong kSignalTimeInvalid = -1;

struct Fence {
    explicit Fence(int descriptor) : fd(descriptor) {}
    std::atomic<int> references{1};
    int fd;
};

void unref(Fence *fence) {
    if (fence->references.fetch_sub(1, std::memory_order_acq_rel) != 1) return;
    if (fence->fd != -1) close(fence->fd);
    delete fence;
}

Fence *fence_of(jlong pointer) {
    return reinterpret_cast<Fence *>(static_cast<uintptr_t>(pointer));
}

// The destructor NativeAllocationRegistry calls with the pointer.
void destroy(Fence *fence) {
    unref(fence);
}

jlong SyncFence_getDestructor(JNIEnv *, jclass) {
    return static_cast<jlong>(reinterpret_cast<uintptr_t>(&destroy));
}

jlong SyncFence_create(JNIEnv *, jclass, jint fd) {
    return static_cast<jlong>(reinterpret_cast<uintptr_t>(new Fence(fd)));
}

jboolean SyncFence_isValid(JNIEnv *, jclass, jlong pointer) {
    return fence_of(pointer)->fd != -1 ? JNI_TRUE : JNI_FALSE;
}

jint SyncFence_getFd(JNIEnv *, jclass, jlong pointer) {
    return fence_of(pointer)->fd;
}

// libsync's sync_wait: poll for POLLIN; an error or a closed descriptor is EINVAL, a timeout ETIME.
int sync_wait(int fd, int timeout) {
    struct pollfd request = {fd, POLLIN, 0};
    int answer;
    do {
        answer = poll(&request, 1, timeout);
    } while (answer == -1 && (errno == EINTR || errno == EAGAIN));
    if (answer > 0) {
        if (request.revents & (POLLERR | POLLNVAL)) {
            errno = EINVAL;
            return -1;
        }
        return 0;
    }
    if (answer == 0) errno = ETIME;
    return -1;
}

jboolean SyncFence_wait(JNIEnv *, jclass, jlong pointer, jlong timeoutNanos) {
    const Fence *fence = fence_of(pointer);
    if (fence->fd == -1) return JNI_TRUE;
    // As AOSP's: the nanoseconds as Fence::wait(int timeout) takes them.
    return sync_wait(fence->fd, static_cast<int>(timeoutNanos)) == 0 ? JNI_TRUE : JNI_FALSE;
}

jlong SyncFence_getSignalTime(JNIEnv *, jclass, jlong pointer) {
    const Fence *fence = fence_of(pointer);
    if (fence->fd == -1) return kSignalTimeInvalid;
    ALOGE("sync_file_info returned NULL for fd %d", fence->fd);
    return kSignalTimeInvalid;
}

void SyncFence_incRef(JNIEnv *, jclass, jlong pointer) {
    fence_of(pointer)->references.fetch_add(1, std::memory_order_relaxed);
}

const JNINativeMethod gMethods[] = {
        {"nGetDestructor", "()J", (void *)SyncFence_getDestructor},
        {"nCreate", "(I)J", (void *)SyncFence_create},
        {"nIsValid", "(J)Z", (void *)SyncFence_isValid},
        {"nGetFd", "(J)I", (void *)SyncFence_getFd},
        {"nWait", "(JJ)Z", (void *)SyncFence_wait},
        {"nGetSignalTime", "(J)J", (void *)SyncFence_getSignalTime},
        {"nIncRef", "(J)V", (void *)SyncFence_incRef},
};

}  // namespace

int register_android_hardware_SyncFence(JNIEnv *env) {
    return RegisterMethodsOrDie(env, "android/hardware/SyncFence", gMethods, NELEM(gMethods));
}

}  // namespace android
