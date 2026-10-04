// Ours in place of AOSP's core/jni/android_os_MessageQueue.cpp (ADR-0039 decision 10, step c1): MessageQueue's six
// natives on M3's ALooper (runtime/android/looper.cpp), where AOSP's stand on libutils' Looper, which the NDK's
// ALooper is on Android. A queue holds its thread's ALooper, made or found by ALooper_prepare as AOSP's
// NativeMessageQueue does with Looper::getForThread, so a NativeActivity's ALooper_forThread finds the same looper on
// that thread, as on Android. nativePollOnce polls it once and hands a descriptor's events to
// MessageQueue.dispatchEvents, which may change or end the watch; nativeWake wakes it from any thread. The NDK has no
// isPolling, so the queue tells it itself: true while its thread is in ALooper_pollOnce, where AOSP's Looper says it
// only for the epoll_wait in there. As ActivityThread's native, the functions come from libandroid through dlopen at
// the first call, since on Android libandroid needs libandroid_runtime, not the other way.
#include <android/looper.h>
#include <dlfcn.h>
#include <jni.h>
#include <nativehelper/JNIHelp.h>
#include <stdio.h>

#include <atomic>
#include <map>
#include <mutex>

#include "core_jni_helpers.h"

// The libandroid functions the natives call, with the NDK header's types.
#define LOOPER_FUNCTIONS(X) \
    X(ALooper_prepare) X(ALooper_acquire) X(ALooper_release) X(ALooper_pollOnce) X(ALooper_wake) X(ALooper_addFd) \
    X(ALooper_removeFd)

namespace android {
namespace {

struct Libandroid {
#define FIELD(name) decltype(&::name) name = nullptr;
    LOOPER_FUNCTIONS(FIELD)
#undef FIELD
    // What could not be found: libandroid.so's dlerror or a function's name; empty when everything was found.
    char missing[256] = "";
};

const Libandroid &libandroid() {
    static const Libandroid functions = [] {
        Libandroid found;
        void *library = dlopen("libandroid.so", RTLD_NOW);
        if (library == nullptr) {
            snprintf(found.missing, sizeof found.missing, "%s", dlerror());
            return found;
        }
#define LOOK_UP(name)                                                                     \
    found.name = reinterpret_cast<decltype(found.name)>(dlsym(library, #name));           \
    if (found.name == nullptr && found.missing[0] == 0) {                                 \
        snprintf(found.missing, sizeof found.missing, "libandroid.so has no %s", #name); \
    }
        LOOPER_FUNCTIONS(LOOK_UP)
#undef LOOK_UP
        return found;
    }();
    return functions;
}

struct {
    jfieldID mPtr;
    jmethodID dispatchEvents;
} gMessageQueueClassInfo;

// MessageQueue.OnFileDescriptorEventListener's events, as AOSP's CALLBACK_EVENT_* keep them.
constexpr int kCallbackEventInput = 1 << 0;
constexpr int kCallbackEventOutput = 1 << 1;
constexpr int kCallbackEventError = 1 << 2;

class NativeMessageQueue {
public:
    explicit NativeMessageQueue(ALooper *looper) : mLooper(looper) {}

    ~NativeMessageQueue() { libandroid().ALooper_release(mLooper); }

    // As AOSP's: the Java queue and its thread's JNIEnv are kept for the callbacks the poll makes. An exception a
    // listener throws stays pending, so nativePollOnce throws it when it returns, as on Android.
    void pollOnce(JNIEnv *env, jobject pollObj, int timeoutMillis) {
        mPollEnv = env;
        mPollObj = pollObj;
        mPolling.store(true, std::memory_order_relaxed);
        libandroid().ALooper_pollOnce(timeoutMillis, nullptr, nullptr, nullptr);
        mPolling.store(false, std::memory_order_relaxed);
        mPollObj = nullptr;
        mPollEnv = nullptr;
    }

    void wake() { libandroid().ALooper_wake(mLooper); }

    bool isPolling() const { return mPolling.load(std::memory_order_relaxed); }

    // The events MessageQueue watches on `fd`, none to stop watching it; AOSP's maps them to Looper's alike.
    void setFileDescriptorEvents(int fd, int events) {
        if (events != 0) {
            int looperEvents = 0;
            if (events & kCallbackEventInput) looperEvents |= ALOOPER_EVENT_INPUT;
            if (events & kCallbackEventOutput) looperEvents |= ALOOPER_EVENT_OUTPUT;
            {
                std::lock_guard<std::mutex> guard(mLock);
                mWatched[fd] = events;
            }
            libandroid().ALooper_addFd(mLooper, fd, ALOOPER_POLL_CALLBACK, looperEvents, HandleEvent, this);
        } else {
            {
                std::lock_guard<std::mutex> guard(mLock);
                mWatched.erase(fd);
            }
            libandroid().ALooper_removeFd(mLooper, fd);
        }
    }

private:
    // On the queue's thread, in its poll: the events to MessageQueue.dispatchEvents, whose answer keeps, changes
    // or (0) ends the watch.
    static int HandleEvent(int fd, int looperEvents, void *data) {
        auto *queue = static_cast<NativeMessageQueue *>(data);
        int events = 0;
        if (looperEvents & ALOOPER_EVENT_INPUT) events |= kCallbackEventInput;
        if (looperEvents & ALOOPER_EVENT_OUTPUT) events |= kCallbackEventOutput;
        if (looperEvents & (ALOOPER_EVENT_ERROR | ALOOPER_EVENT_HANGUP | ALOOPER_EVENT_INVALID)) {
            events |= kCallbackEventError;
        }
        int watched;
        {
            std::lock_guard<std::mutex> guard(queue->mLock);
            auto found = queue->mWatched.find(fd);
            watched = found != queue->mWatched.end() ? found->second : 0;
        }
        const int now = queue->mPollEnv->CallIntMethod(queue->mPollObj, gMessageQueueClassInfo.dispatchEvents, fd,
                                                       events);
        if (now == 0) {
            std::lock_guard<std::mutex> guard(queue->mLock);
            queue->mWatched.erase(fd);
            return 0;  // the looper stops watching the descriptor
        }
        if (now != watched) {
            queue->setFileDescriptorEvents(fd, now);
        }
        return 1;
    }

    ALooper *const mLooper;
    std::atomic<bool> mPolling{false};
    JNIEnv *mPollEnv = nullptr;
    jobject mPollObj = nullptr;
    std::mutex mLock;
    std::map<int, int> mWatched;  // under mLock
};

NativeMessageQueue *Queue(jlong ptr) {
    return reinterpret_cast<NativeMessageQueue *>(ptr);
}

jlong NativeInit(JNIEnv *env, jclass) {
    const Libandroid &lib = libandroid();
    if (lib.missing[0] != 0) {
        jniThrowException(env, "java/lang/UnsatisfiedLinkError", lib.missing);
        return 0;
    }
    ALooper *looper = lib.ALooper_prepare(0);
    if (looper == nullptr) {
        jniThrowRuntimeException(env, "Unable to allocate native queue");
        return 0;
    }
    lib.ALooper_acquire(looper);
    return reinterpret_cast<jlong>(new NativeMessageQueue(looper));
}

void NativeDestroy(JNIEnv *, jclass, jlong ptr) {
    delete Queue(ptr);
}

void NativePollOnce(JNIEnv *env, jobject obj, jlong ptr, jint timeoutMillis) {
    Queue(ptr)->pollOnce(env, obj, timeoutMillis);
}

void NativeWake(JNIEnv *, jclass, jlong ptr) {
    Queue(ptr)->wake();
}

jboolean NativeIsPolling(JNIEnv *, jclass, jlong ptr) {
    return Queue(ptr)->isPolling() ? JNI_TRUE : JNI_FALSE;
}

void NativeSetFileDescriptorEvents(JNIEnv *, jclass, jlong ptr, jint fd, jint events) {
    Queue(ptr)->setFileDescriptorEvents(fd, events);
}

const JNINativeMethod gMessageQueueMethods[] = {
        {"nativeInit", "()J", reinterpret_cast<void *>(NativeInit)},
        {"nativeDestroy", "(J)V", reinterpret_cast<void *>(NativeDestroy)},
        {"nativePollOnce", "(JI)V", reinterpret_cast<void *>(NativePollOnce)},
        {"nativeWake", "(J)V", reinterpret_cast<void *>(NativeWake)},
        {"nativeIsPolling", "(J)Z", reinterpret_cast<void *>(NativeIsPolling)},
        {"nativeSetFileDescriptorEvents", "(JII)V", reinterpret_cast<void *>(NativeSetFileDescriptorEvents)},
};

}  // namespace

int register_android_os_MessageQueue(JNIEnv *env) {
    int result = RegisterMethodsOrDie(env, "android/os/MessageQueue", gMessageQueueMethods, NELEM(gMessageQueueMethods));
    jclass clazz = FindClassOrDie(env, "android/os/MessageQueue");
    gMessageQueueClassInfo.mPtr = GetFieldIDOrDie(env, clazz, "mPtr", "J");
    gMessageQueueClassInfo.dispatchEvents = GetMethodIDOrDie(env, clazz, "dispatchEvents", "(II)I");
    return result;
}

}  // namespace android
