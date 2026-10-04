// Ours in place of AOSP's core/jni/android_view_DisplayEventReceiver.cpp (ADR-0041 a), decision 6):
// DisplayEventReceiver's four natives on libandroid's AChoreographer (runtime/android/choreographer.cpp), where AOSP's
// stand on DisplayEventDispatcher and SurfaceFlinger's event connection. A receiver posts to the AChoreographer of the
// thread that makes it, whose Looper has c1's ALooper (MessageQueue): Choreographer makes its receiver on its own
// thread, so Java's and the NDK's frame callbacks on one thread share that thread's vsync channel and ticks.
//   - nativeScheduleVsync asks for the next tick once until it comes: a second call before it posts nothing, as
//     DisplayEventDispatcher::scheduleVsync.
//   - At the tick, the receiver copies AChoreographerFrameCallbackData's timelines and preferred index, and the
//     timelines' period as the frame interval, into its VsyncEventData, then calls dispatchVsync with the tick's time,
//     display 0 and the tick's number, the frame clock's count of ticks.
//   - nativeGetLatestVsyncEventData computes its timelines at the call, as SurfaceFlinger's
//     EventThread::getLatestVsyncEventData does from its vsync predictor: the refreshes after now on the grid of the
//     receiver's last tick, or, before one, on a grid that starts now at the session's period.
// vsyncSource, eventRegistration and layerHandle change nothing: the one display sends no hotplug, mode or frame-rate
// events. A receiver lives while its Java peer or a posted callback holds it, so that a peer collected before its tick
// leaves no callback to freed memory; a thread that ends with a callback posted keeps its receiver. As the other
// natives, libandroid's functions come through dlopen at the first call.
#include <android/choreographer.h>
#include <android_runtime/AndroidRuntime.h>
#include <dlfcn.h>
#include <jni.h>
#include <nativehelper/JNIHelp.h>
#include <nativehelper/scoped_local_ref.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <mutex>

#include <APKRuntime/guest_interface.h>

#include "core_jni_helpers.h"

// The libandroid functions the natives call, with the NDK header's types.
#define CHOREOGRAPHER_FUNCTIONS(X)                                                                                   \
    X(AChoreographer_getInstance) X(AChoreographer_postVsyncCallback)                                               \
    X(AChoreographerFrameCallbackData_getFrameTimeNanos) X(AChoreographerFrameCallbackData_getFrameTimelinesLength) \
    X(AChoreographerFrameCallbackData_getPreferredFrameTimelineIndex)                                               \
    X(AChoreographerFrameCallbackData_getFrameTimelineVsyncId)                                                      \
    X(AChoreographerFrameCallbackData_getFrameTimelineExpectedPresentationTimeNanos)                                \
    X(AChoreographerFrameCallbackData_getFrameTimelineDeadlineNanos)

namespace android {
namespace {

struct Libandroid {
#define FIELD(name) decltype(&::name) name = nullptr;
    CHOREOGRAPHER_FUNCTIONS(FIELD)
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
        CHOREOGRAPHER_FUNCTIONS(LOOK_UP)
#undef LOOK_UP
        return found;
    }();
    return functions;
}

struct {
    jmethodID dispatchVsync;
    struct {
        jclass clazz;
        jmethodID init;
        jfieldID preferredFrameTimelineIndex;
        jfieldID frameTimelinesLength;
        jfieldID frameInterval;
        jfieldID frameTimelines;
    } vsyncEventData;
    struct {
        jclass clazz;
        jmethodID init;
        jfieldID vsyncId;
        jfieldID expectedPresentationTime;
        jfieldID deadline;
    } frameTimeline;
} gClassInfo;

// VsyncEventData.FRAME_TIMELINES_CAPACITY.
constexpr size_t kTimelinesCapacity = 7;
// FrameInfo.INVALID_VSYNC_ID, with the times of an unused slot, as AOSP's gui::VsyncEventData has them.
constexpr int64_t kInvalidVsyncId = -1;
constexpr int64_t kNanosPerSecond = 1'000'000'000;
// The period without a session: 60 Hz, as the display service's rate (DisplayService).
constexpr int64_t kPeriodWithoutSession = kNanosPerSecond / 60;

struct Timeline {
    int64_t vsyncId = kInvalidVsyncId;
    int64_t expectedPresentationTime = INT64_MAX;
    int64_t deadline = INT64_MAX;
};

// A tick's frame data, in VsyncEventData's terms.
struct VsyncData {
    int64_t time = 0;  // the tick's vsync, the frame time
    int64_t frameInterval = 0;
    size_t preferred = 0;
    size_t length = 0;
    Timeline timelines[kTimelinesCapacity];
};

int64_t Now() {
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec * kNanosPerSecond + now.tv_nsec;
}

// The session's period: a second over its refresh rate (ADR-0028's configuration), or 60 Hz's without a session.
int64_t SessionPeriod() {
    apkrt_configuration session{};
    if (syscall(APKRT_NR_SESSION_CONFIGURATION, &session) != 0 || session.refresh_rate <= 0) {
        return kPeriodWithoutSession;
    }
    return kNanosPerSecond / session.refresh_rate;
}

class NativeDisplayEventReceiver {
public:
    NativeDisplayEventReceiver(JNIEnv *env, jobject receiverWeak, jobject vsyncEventDataWeak,
                               AChoreographer *choreographer)
          : mReceiverWeak(env->NewGlobalRef(receiverWeak)),
            mVsyncEventDataWeak(env->NewGlobalRef(vsyncEventDataWeak)),
            mChoreographer(choreographer) {}

    // The Java peer's hold goes, from the finalizer or dispose.
    static void ReleasePeer(void *ptr) { static_cast<NativeDisplayEventReceiver *>(ptr)->release(); }

    // DisplayEventDispatcher::scheduleVsync: one callback until the tick.
    void scheduleVsync() {
        {
            std::lock_guard<std::mutex> hold(mLock);
            if (mScheduled) return;
            mScheduled = true;
            ++mHolds;
        }
        libandroid().AChoreographer_postVsyncCallback(mChoreographer, OnVsync, this);
    }

    // The timelines after now on the grid of the last tick, or on one from now before a tick.
    VsyncData latestVsyncData() {
        VsyncData last;
        bool ticked;
        {
            std::lock_guard<std::mutex> hold(mLock);
            last = mLast;
            ticked = mTicked;
        }
        const int64_t now = Now();
        int64_t period = last.frameInterval;
        int64_t grid = last.time;
        int64_t firstId = last.timelines[0].vsyncId;
        if (!ticked || period <= 0) {
            period = SessionPeriod();
            grid = now;
            firstId = 0;
        }
        // The last refresh at or before now, and the frame due at the next one, as a tick then would have it.
        const int64_t elapsed = std::max<int64_t>(0, (now - grid) / period);
        const int64_t target = grid + (elapsed + 1) * period;
        VsyncData latest;
        latest.time = grid + elapsed * period;
        latest.frameInterval = period;
        latest.length = APKRT_FRAME_TIMELINES;
        for (size_t index = 0; index < latest.length; ++index) {
            const int64_t k = static_cast<int64_t>(index);
            latest.timelines[index] = {firstId + elapsed + k, target + (k + 1) * period, target + k * period};
        }
        return latest;
    }

private:
    ~NativeDisplayEventReceiver() {
        JNIEnv *env = AndroidRuntime::getJNIEnv();
        if (env != nullptr) {
            env->DeleteGlobalRef(mReceiverWeak);
            env->DeleteGlobalRef(mVsyncEventDataWeak);
        }
    }

    void release() {
        bool last;
        {
            std::lock_guard<std::mutex> hold(mLock);
            last = --mHolds == 0;
        }
        if (last) delete this;
    }

    static VsyncData Read(const AChoreographerFrameCallbackData *data) {
        const Libandroid &lib = libandroid();
        VsyncData tick;
        tick.time = lib.AChoreographerFrameCallbackData_getFrameTimeNanos(data);
        tick.length = std::min(lib.AChoreographerFrameCallbackData_getFrameTimelinesLength(data), kTimelinesCapacity);
        tick.preferred = lib.AChoreographerFrameCallbackData_getPreferredFrameTimelineIndex(data);
        for (size_t index = 0; index < tick.length; ++index) {
            tick.timelines[index] = {
                    lib.AChoreographerFrameCallbackData_getFrameTimelineVsyncId(data, index),
                    lib.AChoreographerFrameCallbackData_getFrameTimelineExpectedPresentationTimeNanos(data, index),
                    lib.AChoreographerFrameCallbackData_getFrameTimelineDeadlineNanos(data, index)};
        }
        // The runtime spaces the timelines by the display's period (guest_interface.h's apkrt_vsync).
        const Timeline &first = tick.timelines[0];
        tick.frameInterval = tick.length > 1
                ? tick.timelines[1].expectedPresentationTime - first.expectedPresentationTime
                : first.expectedPresentationTime - first.deadline;
        return tick;
    }

    // On the receiver's thread, at the tick it asked for.
    static void OnVsync(const AChoreographerFrameCallbackData *data, void *self) {
        auto *receiver = static_cast<NativeDisplayEventReceiver *>(self);
        const VsyncData tick = Read(data);
        {
            std::lock_guard<std::mutex> hold(receiver->mLock);
            receiver->mScheduled = false;
            receiver->mLast = tick;
            receiver->mTicked = true;
        }
        JNIEnv *env = AndroidRuntime::getJNIEnv();
        // An exception an earlier callback of this tick left stays pending for nativePollOnce to throw (c1's
        // MessageQueue), and no JNI call may come before it.
        if (env != nullptr && !env->ExceptionCheck()) {
            receiver->dispatchVsync(env, tick);
        }
        receiver->release();
    }

    // NativeDisplayEventReceiver::dispatchVsync: the data into the receiver's VsyncEventData, then dispatchVsync.
    void dispatchVsync(JNIEnv *env, const VsyncData &tick) {
        ScopedLocalRef<jobject> receiver(env, GetReferent(env, mReceiverWeak));
        ScopedLocalRef<jobject> data(env, GetReferent(env, mVsyncEventDataWeak));
        if (receiver.get() == nullptr || data.get() == nullptr) return;
        const auto &info = gClassInfo.vsyncEventData;
        env->SetIntField(data.get(), info.preferredFrameTimelineIndex, static_cast<jint>(tick.preferred));
        env->SetIntField(data.get(), info.frameTimelinesLength, static_cast<jint>(tick.length));
        env->SetLongField(data.get(), info.frameInterval, tick.frameInterval);
        ScopedLocalRef<jobjectArray> timelines(
                env, static_cast<jobjectArray>(env->GetObjectField(data.get(), info.frameTimelines)));
        for (size_t index = 0; index < tick.length; ++index) {
            ScopedLocalRef<jobject> timeline(env,
                                             env->GetObjectArrayElement(timelines.get(), static_cast<jsize>(index)));
            const Timeline &values = tick.timelines[index];
            env->SetLongField(timeline.get(), gClassInfo.frameTimeline.vsyncId, values.vsyncId);
            env->SetLongField(timeline.get(), gClassInfo.frameTimeline.expectedPresentationTime,
                              values.expectedPresentationTime);
            env->SetLongField(timeline.get(), gClassInfo.frameTimeline.deadline, values.deadline);
        }
        env->CallVoidMethod(receiver.get(), gClassInfo.dispatchVsync, tick.time, jlong{0},
                            static_cast<jint>(tick.timelines[0].vsyncId));
    }

    const jobject mReceiverWeak;        // a global reference to the WeakReference to the receiver
    const jobject mVsyncEventDataWeak;  // and to its VsyncEventData
    AChoreographer *const mChoreographer;
    std::mutex mLock;
    int mHolds = 1;  // the Java peer's and a posted callback's; under mLock, as the three below
    bool mScheduled = false;
    bool mTicked = false;
    VsyncData mLast;
};

NativeDisplayEventReceiver *Receiver(jlong ptr) {
    return reinterpret_cast<NativeDisplayEventReceiver *>(ptr);
}

jlong NativeInit(JNIEnv *env, jclass, jobject receiverWeak, jobject vsyncEventDataWeak, jobject, jint, jint, jlong) {
    const Libandroid &lib = libandroid();
    if (lib.missing[0] != 0) {
        jniThrowException(env, "java/lang/UnsatisfiedLinkError", lib.missing);
        return 0;
    }
    AChoreographer *choreographer = lib.AChoreographer_getInstance();
    if (choreographer == nullptr) {
        jniThrowRuntimeException(env, "Failed to initialize display event receiver.");
        return 0;
    }
    return reinterpret_cast<jlong>(
            new NativeDisplayEventReceiver(env, receiverWeak, vsyncEventDataWeak, choreographer));
}

jlong NativeGetDisplayEventReceiverFinalizer(JNIEnv *, jclass) {
    return static_cast<jlong>(reinterpret_cast<uintptr_t>(&NativeDisplayEventReceiver::ReleasePeer));
}

void NativeScheduleVsync(JNIEnv *, jclass, jlong ptr) {
    Receiver(ptr)->scheduleVsync();
}

// createJavaVsyncEventData: every slot of the capacity, the unused ones as gui::VsyncEventData leaves them.
jobject NativeGetLatestVsyncEventData(JNIEnv *env, jclass, jlong ptr) {
    const VsyncData latest = Receiver(ptr)->latestVsyncData();
    const auto &timelineInfo = gClassInfo.frameTimeline;
    ScopedLocalRef<jobjectArray> timelines(
            env, env->NewObjectArray(static_cast<jsize>(kTimelinesCapacity), timelineInfo.clazz, nullptr));
    if (timelines.get() == nullptr) return nullptr;
    for (size_t index = 0; index < kTimelinesCapacity; ++index) {
        const Timeline &values = latest.timelines[index];
        ScopedLocalRef<jobject> timeline(env, env->NewObject(timelineInfo.clazz, timelineInfo.init, values.vsyncId,
                                                             values.expectedPresentationTime, values.deadline));
        if (timeline.get() == nullptr) return nullptr;
        env->SetObjectArrayElement(timelines.get(), static_cast<jsize>(index), timeline.get());
    }
    return env->NewObject(gClassInfo.vsyncEventData.clazz, gClassInfo.vsyncEventData.init, timelines.get(),
                          static_cast<jint>(latest.preferred), static_cast<jint>(latest.length),
                          latest.frameInterval);
}

const JNINativeMethod gMethods[] = {
        {"nativeInit",
         "(Ljava/lang/ref/WeakReference;Ljava/lang/ref/WeakReference;Landroid/os/MessageQueue;IIJ)J",
         reinterpret_cast<void *>(NativeInit)},
        {"nativeGetDisplayEventReceiverFinalizer", "()J",
         reinterpret_cast<void *>(NativeGetDisplayEventReceiverFinalizer)},
        {"nativeScheduleVsync", "(J)V", reinterpret_cast<void *>(NativeScheduleVsync)},
        {"nativeGetLatestVsyncEventData", "(J)Landroid/view/DisplayEventReceiver$VsyncEventData;",
         reinterpret_cast<void *>(NativeGetLatestVsyncEventData)},
};

}  // namespace

int register_android_view_DisplayEventReceiver(JNIEnv *env) {
    int result = RegisterMethodsOrDie(env, "android/view/DisplayEventReceiver", gMethods, NELEM(gMethods));
    jclass receiver = FindClassOrDie(env, "android/view/DisplayEventReceiver");
    gClassInfo.dispatchVsync = GetMethodIDOrDie(env, receiver, "dispatchVsync", "(JJI)V");

    jclass data = FindClassOrDie(env, "android/view/DisplayEventReceiver$VsyncEventData");
    auto &dataInfo = gClassInfo.vsyncEventData;
    dataInfo.clazz = MakeGlobalRefOrDie(env, data);
    dataInfo.init = GetMethodIDOrDie(env, data, "<init>",
                                     "([Landroid/view/DisplayEventReceiver$VsyncEventData$FrameTimeline;IIJ)V");
    dataInfo.preferredFrameTimelineIndex = GetFieldIDOrDie(env, data, "preferredFrameTimelineIndex", "I");
    dataInfo.frameTimelinesLength = GetFieldIDOrDie(env, data, "frameTimelinesLength", "I");
    dataInfo.frameInterval = GetFieldIDOrDie(env, data, "frameInterval", "J");
    dataInfo.frameTimelines = GetFieldIDOrDie(env, data, "frameTimelines",
                                              "[Landroid/view/DisplayEventReceiver$VsyncEventData$FrameTimeline;");

    jclass timeline = FindClassOrDie(env, "android/view/DisplayEventReceiver$VsyncEventData$FrameTimeline");
    auto &timelineInfo = gClassInfo.frameTimeline;
    timelineInfo.clazz = MakeGlobalRefOrDie(env, timeline);
    timelineInfo.init = GetMethodIDOrDie(env, timeline, "<init>", "(JJJ)V");
    timelineInfo.vsyncId = GetFieldIDOrDie(env, timeline, "vsyncId", "J");
    timelineInfo.expectedPresentationTime = GetFieldIDOrDie(env, timeline, "expectedPresentationTime", "J");
    timelineInfo.deadline = GetFieldIDOrDie(env, timeline, "deadline", "J");
    return result;
}

}  // namespace android
