// Ours in place of AOSP's core/jni/android_view_InputQueue.cpp (ADR-0069 step 3): the natives of AOSP's
// InputQueue.java, NativeActivity's queue on the Java path. Its native pointer, which NativeActivity hands the app as
// its AInputQueue, is a local queue of libandroid's (runtime/android/input_queue.cpp, minato_AInputQueue_createLocal):
// nativeSendKeyEvent and nativeSendMotionEvent post a copy of the Java event as the input channel's records, the key
// with AKEY_EVENT_FLAG_PREDISPATCH when ViewRootImpl sends it before the input method's stage, as AOSP's does, and
// answer the event's ID. The app's finish of an event, on whatever thread, writes its ID and answer to a pipe that the
// queue's MessageQueue's looper reads, whose callback calls InputQueue.finishInputEvent there, as AOSP's
// MSG_FINISH_INPUT does. A motion event keeps its last APKRT_MOTION_SAMPLES samples and APKRT_MOTION_POINTERS
// pointers, each pointer's place, pressure and touch major, as libandroid's motion getters read them.
#define LOG_TAG "InputQueue"

#include <android/looper.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <jni.h>
#include <nativehelper/JNIHelp.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <APKRuntime/guest_interface.h>
#include <log/log.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>

#include <android_runtime/AndroidRuntime.h>
#include "core_jni_helpers.h"
#include "minato_message_queue.h"

struct AInputQueue;

namespace android {
namespace {

using Finished = void (*)(void *, uint64_t, int);

/// libandroid's functions, through dlopen, as the other natives of ours take them.
struct Libandroid {
    AInputQueue *(*createLocal)(Finished, void *) = nullptr;
    bool (*postKey)(AInputQueue *, const apkrt_key_event *, uint64_t) = nullptr;
    bool (*postMotion)(AInputQueue *, const apkrt_motion_event *, uint64_t) = nullptr;
    void (*remove)(AInputQueue *) = nullptr;
    int (*addFd)(ALooper *, int, int, int, ALooper_callbackFunc, void *) = nullptr;
    int (*removeFd)(ALooper *, int) = nullptr;
    void (*acquire)(ALooper *) = nullptr;
    void (*release)(ALooper *) = nullptr;
};

const Libandroid &libandroid() {
    static const Libandroid functions = [] {
        Libandroid found;
        void *library = dlopen("libandroid.so", RTLD_NOW);
        if (library == nullptr) return found;
        found.createLocal = reinterpret_cast<decltype(found.createLocal)>(
                dlsym(library, "minato_AInputQueue_createLocal"));
        found.postKey = reinterpret_cast<decltype(found.postKey)>(dlsym(library, "minato_AInputQueue_postKey"));
        found.postMotion =
                reinterpret_cast<decltype(found.postMotion)>(dlsym(library, "minato_AInputQueue_postMotion"));
        found.remove = reinterpret_cast<decltype(found.remove)>(dlsym(library, "minato_AInputQueue_delete"));
        found.addFd = reinterpret_cast<decltype(found.addFd)>(dlsym(library, "ALooper_addFd"));
        found.removeFd = reinterpret_cast<decltype(found.removeFd)>(dlsym(library, "ALooper_removeFd"));
        found.acquire = reinterpret_cast<decltype(found.acquire)>(dlsym(library, "ALooper_acquire"));
        found.release = reinterpret_cast<decltype(found.release)>(dlsym(library, "ALooper_release"));
        return found;
    }();
    return functions;
}

struct {
    jmethodID finishInputEvent;
    jmethodID referenceGet;  // WeakReference.get
} gInputQueueClassInfo;

struct {
    jfieldID mDeviceId, mSource, mMetaState, mAction, mKeyCode, mScanCode, mRepeatCount, mFlags, mDownTime,
            mEventTime;
} gKeyEventClassInfo;

struct {
    jmethodID getAction, getPointerCount, getPointerId, getHistorySize, getHistoricalX, getHistoricalY,
            getHistoricalPressure, getHistoricalTouchMajor, getHistoricalEventTimeNanos, getX, getY, getPressure,
            getTouchMajor, getEventTimeNanos, getDownTime, getSource, getDeviceId, getToolType, getButtonState,
            getMetaState, getAxisValue;
} gMotionEventClassInfo;

constexpr int32_t kPredispatch = 0x20000000;  // AKEY_EVENT_FLAG_PREDISPATCH
constexpr int32_t kTouchscreen = 0x00001002;  // SOURCE_TOUCHSCREEN

/// A finished event as the pipe carries it.
struct Finish {
    uint64_t id;
    int32_t handled;
};

/// One Java InputQueue's native side.
struct NativeQueue {
    AInputQueue *queue = nullptr;
    jobject weakQueue = nullptr;  // a global reference to the WeakReference<InputQueue>
    ALooper *looper = nullptr;
    int finishRead = -1;
    int finishWrite = -1;
    uint64_t lastId = 0;
};

std::mutex gQueuesLock;
std::map<AInputQueue *, NativeQueue *> gQueues;

NativeQueue *native_queue(jlong ptr) {
    const std::lock_guard hold(gQueuesLock);
    const auto found = gQueues.find(reinterpret_cast<AInputQueue *>(ptr));
    return found != gQueues.end() ? found->second : nullptr;
}

/// libandroid's finish, on the app's thread: to the main looper through the pipe.
void finished(void *context, uint64_t id, int handled) {
    auto *native = static_cast<NativeQueue *>(context);
    const Finish finish{id, handled ? 1 : 0};
    ssize_t written;
    do {
        written = write(native->finishWrite, &finish, sizeof finish);
    } while (written < 0 && errno == EINTR);
    if (written != sizeof finish) ALOGW("Failed writing a finished event: %s", strerror(errno));
}

/// On the queue's looper: InputQueue.finishInputEvent for each finished event, as AOSP's handleMessage.
int finish_callback(int fd, int events, void *data) {
    if ((events & ALOOPER_EVENT_INPUT) == 0) return 1;
    auto *native = static_cast<NativeQueue *>(data);
    JNIEnv *env = AndroidRuntime::getJNIEnv();
    Finish finish;
    while (read(fd, &finish, sizeof finish) == sizeof finish) {
        jobject queue = env->CallObjectMethod(native->weakQueue, gInputQueueClassInfo.referenceGet);
        if (queue == nullptr) {
            ALOGW("InputQueue was finalized without being disposed");
            return 1;
        }
        env->CallVoidMethod(queue, gInputQueueClassInfo.finishInputEvent, static_cast<jlong>(finish.id),
                            static_cast<jboolean>(finish.handled != 0));
        env->DeleteLocalRef(queue);
        if (env->ExceptionCheck()) return 1;  // as AOSP's: the looper's Java caller throws it
    }
    return 1;
}

jlong nativeInit(JNIEnv *env, jclass, jobject weakQueue, jobject messageQueue) {
    const Libandroid &functions = libandroid();
    if (functions.createLocal == nullptr || functions.addFd == nullptr) {
        jniThrowRuntimeException(env, "libandroid.so has no local input queue");
        return 0;
    }
    ALooper *looper = android_os_MessageQueue_getALooper(env, messageQueue);
    if (looper == nullptr) {
        jniThrowRuntimeException(env, "MessageQueue is not initialized.");
        return 0;
    }
    auto *native = new NativeQueue;
    int pipes[2];
    if (pipe(pipes) != 0) {
        delete native;
        jniThrowRuntimeException(env, "could not create the finish pipe");
        return 0;
    }
    native->finishRead = pipes[0];
    native->finishWrite = pipes[1];
    fcntl(native->finishRead, F_SETFL, O_NONBLOCK);
    native->queue = functions.createLocal(finished, native);
    native->weakQueue = env->NewGlobalRef(weakQueue);
    native->looper = looper;
    functions.acquire(looper);
    functions.addFd(looper, native->finishRead, 0, ALOOPER_EVENT_INPUT, finish_callback, native);
    const std::lock_guard hold(gQueuesLock);
    gQueues[native->queue] = native;
    return reinterpret_cast<jlong>(native->queue);
}

void nativeDispose(JNIEnv *env, jclass, jlong ptr) {
    NativeQueue *native = native_queue(ptr);
    if (native == nullptr) return;
    {
        const std::lock_guard hold(gQueuesLock);
        gQueues.erase(native->queue);
    }
    const Libandroid &functions = libandroid();
    functions.removeFd(native->looper, native->finishRead);
    functions.release(native->looper);
    functions.remove(native->queue);
    close(native->finishRead);
    close(native->finishWrite);
    env->DeleteGlobalRef(native->weakQueue);
    delete native;
}

jlong nativeSendKeyEvent(JNIEnv *env, jclass, jlong ptr, jobject eventObj, jboolean predispatch) {
    NativeQueue *native = native_queue(ptr);
    if (native == nullptr) return 0;
    apkrt_key_event key{};
    key.kind = APKRT_INPUT_KEY;
    key.action = env->GetIntField(eventObj, gKeyEventClassInfo.mAction);
    key.down_time = env->GetLongField(eventObj, gKeyEventClassInfo.mDownTime);
    key.event_time = env->GetLongField(eventObj, gKeyEventClassInfo.mEventTime);
    key.key_code = env->GetIntField(eventObj, gKeyEventClassInfo.mKeyCode);
    key.scan_code = env->GetIntField(eventObj, gKeyEventClassInfo.mScanCode);
    key.meta_state = env->GetIntField(eventObj, gKeyEventClassInfo.mMetaState);
    key.repeat_count = env->GetIntField(eventObj, gKeyEventClassInfo.mRepeatCount);
    key.device_id = env->GetIntField(eventObj, gKeyEventClassInfo.mDeviceId);
    key.flags = env->GetIntField(eventObj, gKeyEventClassInfo.mFlags) | (predispatch ? kPredispatch : 0);
    key.source = env->GetIntField(eventObj, gKeyEventClassInfo.mSource);
    const uint64_t id = ++native->lastId;
    return libandroid().postKey(native->queue, &key, id) ? static_cast<jlong>(id) : 0;
}

/// A motion event of another source than the touchscreen (ADR-0068 decision 5, step 4: a pad's stick, a mouse's move)
/// as an axes record: its action, device, source, tool, buttons and meta state, and the axes that are not zero, the
/// first APKRT_AXES_MAX of them.
jlong sendAxesEvent(JNIEnv *env, NativeQueue *native, jobject eventObj) {
    const auto &info = gMotionEventClassInfo;
    apkrt_axes_event axes{};
    axes.kind = APKRT_INPUT_AXES;
    axes.action = env->CallIntMethod(eventObj, info.getAction);
    axes.event_time = env->CallLongMethod(eventObj, info.getEventTimeNanos);
    axes.device_id = env->CallIntMethod(eventObj, info.getDeviceId);
    axes.source = env->CallIntMethod(eventObj, info.getSource);
    axes.tool_type = env->CallIntMethod(eventObj, info.getToolType, 0);
    axes.button_state = env->CallIntMethod(eventObj, info.getButtonState);
    axes.meta_state = env->CallIntMethod(eventObj, info.getMetaState);
    for (int axis = 0; axis < APKRT_AXIS_LIMIT && axes.axis_count < APKRT_AXES_MAX; ++axis) {
        const float value = env->CallFloatMethod(eventObj, info.getAxisValue, axis, 0);
        if (value != 0.f) axes.axes[axes.axis_count++] = {axis, value};
    }
    // The record is smaller than a motion record, which the queue's copy takes whole.
    apkrt_motion_event record{};
    std::memcpy(&record, &axes, sizeof axes);
    const uint64_t id = ++native->lastId;
    return libandroid().postMotion(native->queue, &record, id) ? static_cast<jlong>(id) : 0;
}

jlong nativeSendMotionEvent(JNIEnv *env, jclass, jlong ptr, jobject eventObj) {
    NativeQueue *native = native_queue(ptr);
    if (native == nullptr) return 0;
    const auto &info = gMotionEventClassInfo;
    if ((env->CallIntMethod(eventObj, info.getSource) & kTouchscreen) != kTouchscreen) {
        return sendAxesEvent(env, native, eventObj);
    }
    apkrt_motion_event motion{};
    motion.kind = APKRT_INPUT_MOTION;
    motion.action = env->CallIntMethod(eventObj, info.getAction);
    const int pointers = std::min<int>(env->CallIntMethod(eventObj, info.getPointerCount), APKRT_MOTION_POINTERS);
    const int history = env->CallIntMethod(eventObj, info.getHistorySize);
    const int kept = std::min<int>(history, APKRT_MOTION_SAMPLES - 1);  // the event's own sample comes last
    motion.pointer_count = static_cast<uint32_t>(pointers);
    motion.sample_count = static_cast<uint32_t>(kept + 1);
    motion.down_time = env->CallLongMethod(eventObj, info.getDownTime) * 1000000;
    for (int pointer = 0; pointer < pointers; ++pointer) {
        motion.pointer_ids[pointer] = env->CallIntMethod(eventObj, info.getPointerId, pointer);
    }
    for (int sample = 0; sample <= kept; ++sample) {
        const int pos = history - kept + sample;  // the oldest kept first; pos == history is the event's own
        const bool own = pos == history;
        motion.sample_times[sample] = own ? env->CallLongMethod(eventObj, info.getEventTimeNanos)
                                          : env->CallLongMethod(eventObj, info.getHistoricalEventTimeNanos, pos);
        for (int pointer = 0; pointer < pointers; ++pointer) {
            apkrt_pointer_coords &at = motion.coords[sample][pointer];
            if (own) {
                at.x = env->CallFloatMethod(eventObj, info.getX, pointer);
                at.y = env->CallFloatMethod(eventObj, info.getY, pointer);
                at.pressure = env->CallFloatMethod(eventObj, info.getPressure, pointer);
                at.touch_major = env->CallFloatMethod(eventObj, info.getTouchMajor, pointer);
            } else {
                at.x = env->CallFloatMethod(eventObj, info.getHistoricalX, pointer, pos);
                at.y = env->CallFloatMethod(eventObj, info.getHistoricalY, pointer, pos);
                at.pressure = env->CallFloatMethod(eventObj, info.getHistoricalPressure, pointer, pos);
                at.touch_major = env->CallFloatMethod(eventObj, info.getHistoricalTouchMajor, pointer, pos);
            }
        }
    }
    const uint64_t id = ++native->lastId;
    return libandroid().postMotion(native->queue, &motion, id) ? static_cast<jlong>(id) : 0;
}

const JNINativeMethod g_methods[] = {
        {"nativeInit", "(Ljava/lang/ref/WeakReference;Landroid/os/MessageQueue;)J",
         reinterpret_cast<void *>(nativeInit)},
        {"nativeDispose", "(J)V", reinterpret_cast<void *>(nativeDispose)},
        {"nativeSendKeyEvent", "(JLandroid/view/KeyEvent;Z)J", reinterpret_cast<void *>(nativeSendKeyEvent)},
        {"nativeSendMotionEvent", "(JLandroid/view/MotionEvent;)J", reinterpret_cast<void *>(nativeSendMotionEvent)},
};

}  // namespace

int register_android_view_InputQueue(JNIEnv *env) {
    jclass queue = FindClassOrDie(env, "android/view/InputQueue");
    gInputQueueClassInfo.finishInputEvent = GetMethodIDOrDie(env, queue, "finishInputEvent", "(JZ)V");
    gInputQueueClassInfo.referenceGet =
            GetMethodIDOrDie(env, FindClassOrDie(env, "java/lang/ref/Reference"), "get", "()Ljava/lang/Object;");
    jclass key = FindClassOrDie(env, "android/view/KeyEvent");
    auto &k = gKeyEventClassInfo;
    k.mDeviceId = GetFieldIDOrDie(env, key, "mDeviceId", "I");
    k.mSource = GetFieldIDOrDie(env, key, "mSource", "I");
    k.mMetaState = GetFieldIDOrDie(env, key, "mMetaState", "I");
    k.mAction = GetFieldIDOrDie(env, key, "mAction", "I");
    k.mKeyCode = GetFieldIDOrDie(env, key, "mKeyCode", "I");
    k.mScanCode = GetFieldIDOrDie(env, key, "mScanCode", "I");
    k.mRepeatCount = GetFieldIDOrDie(env, key, "mRepeatCount", "I");
    k.mFlags = GetFieldIDOrDie(env, key, "mFlags", "I");
    k.mDownTime = GetFieldIDOrDie(env, key, "mDownTime", "J");
    k.mEventTime = GetFieldIDOrDie(env, key, "mEventTime", "J");
    jclass motion = FindClassOrDie(env, "android/view/MotionEvent");
    auto &m = gMotionEventClassInfo;
    m.getAction = GetMethodIDOrDie(env, motion, "getAction", "()I");
    m.getPointerCount = GetMethodIDOrDie(env, motion, "getPointerCount", "()I");
    m.getPointerId = GetMethodIDOrDie(env, motion, "getPointerId", "(I)I");
    m.getHistorySize = GetMethodIDOrDie(env, motion, "getHistorySize", "()I");
    m.getHistoricalX = GetMethodIDOrDie(env, motion, "getHistoricalX", "(II)F");
    m.getHistoricalY = GetMethodIDOrDie(env, motion, "getHistoricalY", "(II)F");
    m.getHistoricalPressure = GetMethodIDOrDie(env, motion, "getHistoricalPressure", "(II)F");
    m.getHistoricalTouchMajor = GetMethodIDOrDie(env, motion, "getHistoricalTouchMajor", "(II)F");
    m.getHistoricalEventTimeNanos = GetMethodIDOrDie(env, motion, "getHistoricalEventTimeNanos", "(I)J");
    m.getX = GetMethodIDOrDie(env, motion, "getX", "(I)F");
    m.getY = GetMethodIDOrDie(env, motion, "getY", "(I)F");
    m.getPressure = GetMethodIDOrDie(env, motion, "getPressure", "(I)F");
    m.getTouchMajor = GetMethodIDOrDie(env, motion, "getTouchMajor", "(I)F");
    m.getEventTimeNanos = GetMethodIDOrDie(env, motion, "getEventTimeNanos", "()J");
    m.getDownTime = GetMethodIDOrDie(env, motion, "getDownTime", "()J");
    m.getSource = GetMethodIDOrDie(env, motion, "getSource", "()I");
    m.getDeviceId = GetMethodIDOrDie(env, motion, "getDeviceId", "()I");
    m.getToolType = GetMethodIDOrDie(env, motion, "getToolType", "(I)I");
    m.getButtonState = GetMethodIDOrDie(env, motion, "getButtonState", "()I");
    m.getMetaState = GetMethodIDOrDie(env, motion, "getMetaState", "()I");
    m.getAxisValue = GetMethodIDOrDie(env, motion, "getAxisValue", "(II)F");
    return RegisterMethodsOrDie(env, "android/view/InputQueue", g_methods, NELEM(g_methods));
}

}  // namespace android
