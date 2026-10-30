// Ours in place of AOSP's core/jni/android_view_InputEventReceiver.cpp (ADR-0045 step 7b2): InputEventReceiver's
// natives on the session's input channel (minato_input_channel.h), where AOSP's stand on libinput's InputConsumer and
// the dispatcher's socket. The receiver watches the channel's descriptor on its thread's looper (M3's ALooper, which
// MessageQueue's natives run) and takes the host's motion events with input_read, each a struct apkrt_motion_event of
// the touchscreen, as InputConsumer takes the dispatcher's messages:
//   - a MOVE's samples are batched, each one as one of InputConsumer's messages, and wait for
//     consumeBatchedInputEvents, which ViewRootImpl calls at the frame's CALLBACK_INPUT; onBatchedInputEventPending
//     says that a batch waits, once until a consume;
//   - consumeBatchedInputEvents(frameTime) sends the samples up to frameTime, all for -1, as one MotionEvent with the
//     earlier ones as its history; the later ones wait for the next frame. Android also resamples at frameTime - 5 ms
//     (InputConsumer's RESAMPLE_LATENCY); ours sends what Android sends with ro.input.resampling false, a known gap,
//     by which a drag's place per frame differs by up to a sample;
//   - a MOVE with other pointers than the batch's, or another action, first sends the batch whole; a CANCEL drops it,
//     as InputConsumer finishes a cancelled gesture's samples unhandled;
//   - every event becomes libinput's MotionEvent as libandroid's AMotionEvent answers for the same records
//     (runtime/android/input.cpp): device 1, the touchscreen, fingers, display 0, a precision of 1, no flags, buttons
//     or meta state, the raw places the places (the window fills the screen), and per pointer x, y, the pressure and
//     the touch major, also as the touch minor and the tool's sizes. AOSP's android_view_MotionEvent_obtainAsCopy
//     makes the Java MotionEvent, which goes to dispatchInputEvent with a sequence number of the receiver's;
//   - finishInputEvent tells no one: no dispatcher waits for the answer. reportTimeline does nothing too;
//     probablyHasInput says whether a batch waits or the descriptor is readable.
// An exception from the app stops the round's callbacks and stays pending, as AOSP's skipCallbacks: the looper's
// callback leaves it to MessageQueue's nativePollOnce, as the other callbacks of ours do. A receiver lives until
// nativeDispose and while a round runs, so that a dispose from the app's callback frees nothing under the round.
#define LOG_TAG "InputEventReceiver"

#include <android/input.h>
#include <android/looper.h>
#include <android_runtime/AndroidRuntime.h>
#include <attestation/HmacKeyManager.h>
#include <dlfcn.h>
#include <errno.h>
#include <input/Input.h>
#include <jni.h>
#include <log/log.h>
#include <nativehelper/JNIHelp.h>
#include <nativehelper/scoped_local_ref.h>
#include <nativehelper/scoped_utf_chars.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <memory>
#include <string>
#include <vector>

#include <APKRuntime/guest_interface.h>

#include "android_view_KeyEvent.h"
#include "android_view_MotionEvent.h"
#include "core_jni_helpers.h"
#include "minato_input_channel.h"

// The libandroid functions the natives call, with the NDK header's types.
#define LOOPER_FUNCTIONS(X) \
    X(ALooper_forThread) X(ALooper_acquire) X(ALooper_release) X(ALooper_addFd) X(ALooper_removeFd)

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
    jmethodID dispatchInputEvent;
    jmethodID onBatchedInputEventPending;
} gInputEventReceiverClassInfo;

struct {
    jclass clazz;
    jmethodID myQueue;
} gLooperClassInfo;

struct {
    jclass clazz;
    jmethodID onHostDevice;
} gInputServiceClassInfo;

struct {
    jclass clazz;
    jmethodID onHostText;
} gInputMethodManagerClassInfo;

// libandroid's answers for what a record does not carry (runtime/android/input.cpp).
constexpr int32_t kDeviceId = 1;
constexpr uint32_t kSource = AINPUT_SOURCE_TOUCHSCREEN;

// One of InputConsumer's messages of a MOVE: a sample's time and its pointers' places.
struct Sample {
    nsecs_t time;
    apkrt_pointer_coords coords[APKRT_MOTION_POINTERS];
};

// The MOVEs that wait for the frame, as InputConsumer's Batch: the pointers they share and their samples in order.
struct Batch {
    nsecs_t downTime = 0;
    uint32_t pointerCount = 0;
    int32_t pointerIds[APKRT_MOTION_POINTERS] = {};
    std::vector<Sample> samples;
};

// libandroid's axes for a record's places.
void Fill(PointerCoords *out, const apkrt_pointer_coords *places, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        out[i].clear();
        out[i].setAxisValue(AMOTION_EVENT_AXIS_X, places[i].x);
        out[i].setAxisValue(AMOTION_EVENT_AXIS_Y, places[i].y);
        out[i].setAxisValue(AMOTION_EVENT_AXIS_PRESSURE, places[i].pressure);
        out[i].setAxisValue(AMOTION_EVENT_AXIS_TOUCH_MAJOR, places[i].touch_major);
        out[i].setAxisValue(AMOTION_EVENT_AXIS_TOUCH_MINOR, places[i].touch_major);
        out[i].setAxisValue(AMOTION_EVENT_AXIS_TOOL_MAJOR, places[i].touch_major);
        out[i].setAxisValue(AMOTION_EVENT_AXIS_TOOL_MINOR, places[i].touch_major);
    }
}

class NativeInputEventReceiver {
public:
    NativeInputEventReceiver(JNIEnv *env, jobject receiverWeak, std::shared_ptr<InputDescriptor> descriptor,
                             std::string name, ALooper *looper)
          : mReceiverWeakGlobal(env->NewGlobalRef(receiverWeak)),
            mDescriptor(std::move(descriptor)),
            mName(std::move(name)),
            mLooper(looper) {
        libandroid().ALooper_acquire(mLooper);
    }

    // Watches the descriptor; false when the looper refuses it.
    bool initialize() {
        return libandroid().ALooper_addFd(mLooper, mDescriptor->get(), ALOOPER_POLL_CALLBACK, ALOOPER_EVENT_INPUT,
                                          HandleEvent, this) == 1;
    }

    // nativeDispose: the watch ends, and the receiver goes once no round runs.
    void dispose() {
        if (!mDisposed) {
            mDisposed = true;
            libandroid().ALooper_removeFd(mLooper, mDescriptor->get());
        }
        release();
    }

    // The rounds hold the receiver, as the peer does until its dispose.
    void hold() { ++mHolds; }
    void release() {
        if (--mHolds == 0) delete this;
    }

    // As AOSP's NativeInputEventReceiver::consumeEvents over InputConsumer::consume: what the channel holds, then,
    // with consumeBatches, the batch's samples up to frameTime, again until nothing more comes; a batch that still
    // waits is announced once. 0, or the negative errno of input_read.
    int consumeEvents(JNIEnv *env, bool consumeBatches, nsecs_t frameTime, bool *outConsumedBatch) {
        if (consumeBatches) mBatchedInputEventPending = false;
        if (outConsumedBatch) *outConsumedBatch = false;
        Round round(env, outConsumedBatch);
        for (;;) {
            apkrt_motion_event record;
            while (!mDisposed && syscall(APKRT_NR_INPUT_READ, mDescriptor->get(), &record) == 0) {
                receive(round, record);
            }
            if (mDisposed) return 0;
            if (errno != EAGAIN) {
                ALOGE("channel '%s' ~ Failed to consume input event.  status=%s", mName.c_str(), strerror(errno));
                return -errno;
            }
            if (!consumeBatches || mBatch.samples.empty()) break;
            // InputConsumer::consumeBatch: the samples no later than the frame, all for a frame time below 0.
            size_t count = mBatch.samples.size();
            if (frameTime >= 0) {
                count = 0;
                while (count < mBatch.samples.size() && mBatch.samples[count].time <= frameTime) ++count;
            }
            if (count == 0) break;
            sendBatch(round, count);
        }
        if (!round.skipCallbacks && !mDisposed && !mBatchedInputEventPending && !mBatch.samples.empty()) {
            jobject receiver = round.receiver(mReceiverWeakGlobal, mName);
            if (receiver == nullptr) return 0;
            mBatchedInputEventPending = true;
            env->CallVoidMethod(receiver, gInputEventReceiverClassInfo.onBatchedInputEventPending,
                                static_cast<jint>(kSource));
            if (env->ExceptionCheck()) {
                ALOGE("Exception dispatching batched input events.");
                mBatchedInputEventPending = false;  // try again later
            }
        }
        return 0;
    }

    // Whether a batch waits or the channel holds an event.
    bool probablyHasInput() const {
        if (!mBatch.samples.empty()) return true;
        pollfd descriptor = {mDescriptor->get(), POLLIN, 0};
        return poll(&descriptor, 1, 0) == 1 && (descriptor.revents & POLLIN) != 0;
    }

    std::string dump(const char *prefix) const {
        char text[256];
        snprintf(text, sizeof text, "%schannel '%s', mBatchedInputEventPending: %s, batched samples: %zu\n", prefix,
                 mName.c_str(), mBatchedInputEventPending ? "true" : "false", mBatch.samples.size());
        return text;
    }

private:
    ~NativeInputEventReceiver() {
        JNIEnv *env = AndroidRuntime::getJNIEnv();
        if (env != nullptr) env->DeleteGlobalRef(mReceiverWeakGlobal);
        libandroid().ALooper_release(mLooper);
    }

    // One consume: the receiver's object, once found, and whether an exception or a lost receiver stopped the
    // callbacks.
    struct Round {
        Round(JNIEnv *env, bool *consumedBatch) : env(env), consumedBatch(consumedBatch), receiverObj(env, nullptr) {}

        // The Java receiver, null once it was collected without its dispose (AOSP's warning) or the round stopped.
        jobject receiver(jobject weak, const std::string &name) {
            if (skipCallbacks) return nullptr;
            if (receiverObj.get() == nullptr) {
                receiverObj.reset(GetReferent(env, weak));
                if (receiverObj.get() == nullptr) {
                    ALOGW("channel '%s' ~ Receiver object was finalized without being disposed.", name.c_str());
                    skipCallbacks = true;
                }
            }
            return receiverObj.get();
        }

        JNIEnv *const env;
        bool *const consumedBatch;
        ScopedLocalRef<jobject> receiverObj;
        bool skipCallbacks = false;
    };

    static int HandleEvent(int, int events, void *data) {
        auto *receiver = static_cast<NativeInputEventReceiver *>(data);
        if ((events & (ALOOPER_EVENT_ERROR | ALOOPER_EVENT_HANGUP)) != 0) {
            ALOGW("channel '%s' ~ Publisher closed input channel or an error occurred.  events=0x%x",
                  receiver->mName.c_str(), events);
            return 0;
        }
        JNIEnv *env = AndroidRuntime::getJNIEnv();
        // An exception an earlier callback left stays pending for nativePollOnce to throw, and no JNI call may come
        // before it: the events wait for the next poll.
        if (env == nullptr || env->ExceptionCheck()) return 1;
        receiver->hold();
        receiver->consumeEvents(env, false, -1, nullptr);
        const bool disposed = receiver->mDisposed;
        receiver->release();
        return disposed ? 0 : 1;
    }

    // A record as InputConsumer's messages: each sample of a MOVE, or the record's one event.
    void receive(Round &round, const apkrt_motion_event &record) {
        // A key is a KeyEvent for the receiver as it is (ADR-0045 step 7c2); a text edit is the input method
        // manager's (ADR-0068 part 1) and a device the input service's.
        if (record.kind == APKRT_INPUT_TEXT) {
            textEdited(round.env, reinterpret_cast<const apkrt_text_event &>(record));
            return;
        }
        if (record.kind == APKRT_INPUT_KEY) {
            dispatchKey(round, reinterpret_cast<const apkrt_key_event &>(record));
            return;
        }
        if (record.kind == APKRT_INPUT_DEVICE) {
            deviceChanged(round.env, reinterpret_cast<const apkrt_device_event &>(record));
            return;
        }
        if (record.kind == APKRT_INPUT_AXES) {
            dispatchAxes(round, reinterpret_cast<const apkrt_axes_event &>(record));
            return;
        }
        if (record.kind != APKRT_INPUT_MOTION) return;
        if (record.action == AMOTION_EVENT_ACTION_MOVE) {
            for (uint32_t sample = 0; sample < record.sample_count; ++sample) {
                if (!mBatch.samples.empty() && !canAddSample(record)) sendBatch(round, mBatch.samples.size());
                if (mBatch.samples.empty()) {
                    mBatch.downTime = record.down_time;
                    mBatch.pointerCount = record.pointer_count;
                    memcpy(mBatch.pointerIds, record.pointer_ids, sizeof mBatch.pointerIds);
                }
                Sample &added = mBatch.samples.emplace_back();
                added.time = record.sample_times[sample];
                memcpy(added.coords, record.coords[sample], sizeof added.coords);
            }
            return;
        }
        if (!mBatch.samples.empty()) {
            if (record.action == AMOTION_EVENT_ACTION_CANCEL) {
                mBatch.samples.clear();
            } else {
                sendBatch(round, mBatch.samples.size());
            }
        }
        Sample own;
        own.time = record.sample_times[record.sample_count - 1];
        memcpy(own.coords, record.coords[record.sample_count - 1], sizeof own.coords);
        dispatch(round, record.action, record.down_time, record.pointer_count, record.pointer_ids, &own, 1);
    }

    // InputConsumer::canAddSample: a MOVE of the batch's pointers (each a finger).
    bool canAddSample(const apkrt_motion_event &record) const {
        if (record.pointer_count != mBatch.pointerCount) return false;
        for (uint32_t i = 0; i < record.pointer_count; ++i) {
            if (record.pointer_ids[i] != mBatch.pointerIds[i]) return false;
        }
        return true;
    }

    // The batch's first `count` samples as one MOVE, taken out of the batch before the app sees them.
    void sendBatch(Round &round, size_t count) {
        Batch sent;
        sent.downTime = mBatch.downTime;
        sent.pointerCount = mBatch.pointerCount;
        memcpy(sent.pointerIds, mBatch.pointerIds, sizeof sent.pointerIds);
        const auto end = mBatch.samples.begin() + static_cast<long>(count);
        sent.samples.assign(mBatch.samples.begin(), end);
        mBatch.samples.erase(mBatch.samples.begin(), end);
        dispatch(round, AMOTION_EVENT_ACTION_MOVE, sent.downTime, sent.pointerCount, sent.pointerIds,
                 sent.samples.data(), sent.samples.size());
    }

    // A device the host adds, removes or changes (ADR-0068 step 7c2): the input service of the process hears of it, as
    // InputReader's device list change tells InputManagerService; the app's listeners hear from that.
    void deviceChanged(JNIEnv *env, const apkrt_device_event &record) {
        ScopedLocalRef<jstring> name(env, env->NewStringUTF(record.name));
        env->CallStaticVoidMethod(gInputServiceClassInfo.clazz, gInputServiceClassInfo.onHostDevice,
                                  static_cast<jint>(record.op), static_cast<jint>(record.device_id),
                                  static_cast<jint>(record.sources), static_cast<jint>(record.vendor_id),
                                  static_cast<jint>(record.product_id), name.get());
        if (env->ExceptionCheck()) {
            ALOGE("Exception reporting a device of the host.");
            env->ExceptionClear();
        }
    }

    // A text edit of the host's keyboard (ADR-0068 part 1): the input method manager turns it into a call on the
    // served view's connection. The text goes as UTF-8 bytes, which Java decodes: JNI's own strings are modified UTF-8.
    void textEdited(JNIEnv *env, const apkrt_text_event &record) {
        const uint32_t length = std::min<uint32_t>(record.length, APKRT_TEXT_BYTES);
        ScopedLocalRef<jbyteArray> text(env, env->NewByteArray(static_cast<jsize>(length)));
        if (text.get() == nullptr) {
            env->ExceptionClear();
            return;
        }
        env->SetByteArrayRegion(text.get(), 0, static_cast<jsize>(length),
                                reinterpret_cast<const jbyte *>(record.text));
        env->CallStaticVoidMethod(gInputMethodManagerClassInfo.clazz, gInputMethodManagerClassInfo.onHostText,
                                  static_cast<jint>(record.op), static_cast<jint>(record.arg0),
                                  static_cast<jint>(record.arg1), static_cast<jlong>(record.serial), text.get());
        if (env->ExceptionCheck()) {
            ALOGE("Exception delivering a text of the host.");
            env->ExceptionClear();
        }
    }

    // A KeyEvent, as InputConsumer's key message makes one, to the receiver's dispatchInputEvent.
    void dispatchKey(Round &round, const apkrt_key_event &record) {
        jobject receiver = mDisposed ? nullptr : round.receiver(mReceiverWeakGlobal, mName);
        if (receiver == nullptr) return;
        KeyEvent event;
        event.initialize(InputEvent::nextId(), record.device_id, static_cast<uint32_t>(record.source),
                         ui::LogicalDisplayId::DEFAULT, INVALID_HMAC, record.action, record.flags, record.key_code,
                         record.scan_code, record.meta_state, record.repeat_count, record.down_time,
                         record.event_time);
        JNIEnv *env = round.env;
        ScopedLocalRef<jobject> eventObj = android_view_KeyEvent_obtainAsCopy(env, event);
        if (eventObj.get() == nullptr) {
            ALOGW("channel '%s' ~ Failed to obtain key event object.", mName.c_str());
            round.skipCallbacks = true;
            return;
        }
        env->CallVoidMethod(receiver, gInputEventReceiverClassInfo.dispatchInputEvent, static_cast<jint>(++mSeq),
                            eventObj.get());
        if (env->ExceptionCheck()) {
            ALOGE("Exception dispatching key event.");
            round.skipCallbacks = true;
        }
    }

    // A game controller's or a mouse's motion (ADR-0068 decision 5), as InputReader makes one for a joystick or a
    // pointer device: one pointer of the record's tool type with the axes the host listed, and the triggers' second
    // axes, BRAKE and GAS, which Android reports with LTRIGGER and RTRIGGER (measured by gamepad-test). Between a
    // touch's samples a batch waits: the controller's events are no part of it and go at once.
    void dispatchAxes(Round &round, const apkrt_axes_event &record) {
        jobject receiver = mDisposed ? nullptr : round.receiver(mReceiverWeakGlobal, mName);
        if (receiver == nullptr) return;
        PointerProperties properties;
        properties.clear();
        properties.id = 0;
        properties.toolType = static_cast<ToolType>(record.tool_type);
        PointerCoords coords;
        coords.clear();
        const uint32_t count = std::min<uint32_t>(record.axis_count, APKRT_AXES_MAX);
        float x = 0, y = 0;
        for (uint32_t at = 0; at < count; ++at) {
            const apkrt_axis_value &axis = record.axes[at];
            coords.setAxisValue(axis.axis, axis.value);
            if (axis.axis == AMOTION_EVENT_AXIS_X) x = axis.value;
            if (axis.axis == AMOTION_EVENT_AXIS_Y) y = axis.value;
            if (axis.axis == AMOTION_EVENT_AXIS_LTRIGGER) coords.setAxisValue(AMOTION_EVENT_AXIS_BRAKE, axis.value);
            if (axis.axis == AMOTION_EVENT_AXIS_RTRIGGER) coords.setAxisValue(AMOTION_EVENT_AXIS_GAS, axis.value);
        }
        const bool pointer = (static_cast<uint32_t>(record.source) & AINPUT_SOURCE_CLASS_POINTER) != 0;
        MotionEvent event;
        const ui::Transform identity;
        event.initialize(InputEvent::nextId(), record.device_id, static_cast<uint32_t>(record.source),
                         ui::LogicalDisplayId::DEFAULT, INVALID_HMAC, record.action, 0, 0, 0,
                         record.meta_state, record.button_state, MotionClassification::NONE, identity, 1, 1,
                         pointer ? x : AMOTION_EVENT_INVALID_CURSOR_POSITION,
                         pointer ? y : AMOTION_EVENT_INVALID_CURSOR_POSITION, identity, record.event_time,
                         record.event_time, 1, &properties, &coords);
        JNIEnv *env = round.env;
        ScopedLocalRef<jobject> eventObj = android_view_MotionEvent_obtainAsCopy(env, event);
        if (eventObj.get() == nullptr) {
            ALOGW("channel '%s' ~ Failed to obtain axes event object.", mName.c_str());
            round.skipCallbacks = true;
            return;
        }
        env->CallVoidMethod(receiver, gInputEventReceiverClassInfo.dispatchInputEvent, static_cast<jint>(++mSeq),
                            eventObj.get());
        if (env->ExceptionCheck()) {
            ALOGE("Exception dispatching axes event.");
            round.skipCallbacks = true;
        }
    }

    // A MotionEvent of `count` samples, the history and then its own, to the receiver's dispatchInputEvent.
    void dispatch(Round &round, int32_t action, nsecs_t downTime, uint32_t pointerCount, const int32_t *ids,
                  const Sample *samples, size_t count) {
        jobject receiver = mDisposed ? nullptr : round.receiver(mReceiverWeakGlobal, mName);
        if (receiver == nullptr) return;
        PointerProperties properties[APKRT_MOTION_POINTERS];
        PointerCoords coords[APKRT_MOTION_POINTERS];
        for (uint32_t i = 0; i < pointerCount; ++i) {
            properties[i].clear();
            properties[i].id = ids[i];
            properties[i].toolType = ToolType::FINGER;
        }
        Fill(coords, samples[0].coords, pointerCount);
        MotionEvent event;
        const ui::Transform identity;
        event.initialize(InputEvent::nextId(), kDeviceId, kSource, ui::LogicalDisplayId::DEFAULT, INVALID_HMAC, action,
                         0, 0, 0, 0, 0, MotionClassification::NONE, identity, 1, 1,
                         AMOTION_EVENT_INVALID_CURSOR_POSITION, AMOTION_EVENT_INVALID_CURSOR_POSITION, identity,
                         downTime, samples[0].time, pointerCount, properties, coords);
        for (size_t sample = 1; sample < count; ++sample) {
            Fill(coords, samples[sample].coords, pointerCount);
            event.addSample(samples[sample].time, coords);
        }
        if ((event.getAction() & AMOTION_EVENT_ACTION_MOVE) && round.consumedBatch != nullptr) {
            *round.consumedBatch = true;
        }
        JNIEnv *env = round.env;
        ScopedLocalRef<jobject> eventObj = android_view_MotionEvent_obtainAsCopy(env, event);
        if (eventObj.get() == nullptr) {
            ALOGW("channel '%s' ~ Failed to obtain event object.", mName.c_str());
            round.skipCallbacks = true;
            return;
        }
        env->CallVoidMethod(receiver, gInputEventReceiverClassInfo.dispatchInputEvent, static_cast<jint>(++mSeq),
                            eventObj.get());
        if (env->ExceptionCheck()) {
            ALOGE("Exception dispatching input event.");
            round.skipCallbacks = true;
        }
    }

    const jobject mReceiverWeakGlobal;  // a global reference to the WeakReference to the Java receiver
    const std::shared_ptr<InputDescriptor> mDescriptor;
    const std::string mName;
    ALooper *const mLooper;
    int mHolds = 1;  // the peer's until its dispose, and one per round
    bool mDisposed = false;
    bool mBatchedInputEventPending = false;
    Batch mBatch;
    uint32_t mSeq = 0;
};

NativeInputEventReceiver *Receiver(jlong ptr) {
    return reinterpret_cast<NativeInputEventReceiver *>(ptr);
}

jlong NativeInit(JNIEnv *env, jclass, jobject receiverWeak, jobject inputChannelObj, jobject messageQueueObj) {
    const Libandroid &lib = libandroid();
    if (lib.missing[0] != 0) {
        jniThrowException(env, "java/lang/UnsatisfiedLinkError", lib.missing);
        return 0;
    }
    std::shared_ptr<InputDescriptor> descriptor = android_view_InputChannel_getDescriptor(env, inputChannelObj);
    if (descriptor == nullptr) {
        jniThrowRuntimeException(env, "InputChannel is not initialized.");
        return 0;
    }
    if (messageQueueObj == nullptr) {
        jniThrowRuntimeException(env, "MessageQueue is not initialized.");
        return 0;
    }
    // The looper is the calling thread's ALooper, MessageQueue's on that thread: a receiver on another thread's
    // looper, which only hidden API can make, would need that queue's native, so it is refused.
    ScopedLocalRef<jobject> myQueue(env, env->CallStaticObjectMethod(gLooperClassInfo.clazz, gLooperClassInfo.myQueue));
    if (env->ExceptionCheck()) env->ExceptionClear();
    ALooper *looper = lib.ALooper_forThread();
    if (looper == nullptr || !env->IsSameObject(myQueue.get(), messageQueueObj)) {
        jniThrowException(env, "java/lang/UnsupportedOperationException",
                          "an InputEventReceiver takes the looper of the thread that makes it (ADR-0045 step 7b2)");
        return 0;
    }
    auto *receiver = new NativeInputEventReceiver(env, receiverWeak, std::move(descriptor),
                                                  android_view_InputChannel_getName(env, inputChannelObj), looper);
    if (!receiver->initialize()) {
        receiver->dispose();
        jniThrowRuntimeException(env, "Failed to initialize input event receiver.");
        return 0;
    }
    return reinterpret_cast<jlong>(receiver);
}

void NativeDispose(JNIEnv *, jclass, jlong ptr) {
    Receiver(ptr)->dispose();
}

// Nothing waits for the answer: the host's channel has no dispatcher.
void NativeFinishInputEvent(JNIEnv *, jclass, jlong, jint, jboolean) {}

jboolean NativeProbablyHasInput(JNIEnv *, jclass, jlong ptr) {
    return Receiver(ptr)->probablyHasInput() ? JNI_TRUE : JNI_FALSE;
}

void NativeReportTimeline(JNIEnv *, jclass, jlong, jint, jlong, jlong) {}

jboolean NativeConsumeBatchedInputEvents(JNIEnv *env, jclass, jlong ptr, jlong frameTimeNanos) {
    NativeInputEventReceiver *receiver = Receiver(ptr);
    bool consumedBatch = false;
    receiver->hold();
    const int status = receiver->consumeEvents(env, true, frameTimeNanos, &consumedBatch);
    receiver->release();
    if (status != 0 && !env->ExceptionCheck()) {
        char message[128];
        snprintf(message, sizeof message, "Failed to consume batched input event.  status=%s(%d)", strerror(-status),
                 status);
        jniThrowRuntimeException(env, message);
        return JNI_FALSE;
    }
    return consumedBatch ? JNI_TRUE : JNI_FALSE;
}

// As AOSP's, with the exception pending, and nothing more done, when the prefix cannot be read.
jstring NativeDump(JNIEnv *env, jclass, jlong ptr, jstring prefix) {
    ScopedUtfChars chars(env, prefix);
    if (chars.c_str() == nullptr) return nullptr;
    return env->NewStringUTF(Receiver(ptr)->dump(chars.c_str()).c_str());
}

const JNINativeMethod gMethods[] = {
        {"nativeInit", "(Ljava/lang/ref/WeakReference;Landroid/view/InputChannel;Landroid/os/MessageQueue;)J",
         reinterpret_cast<void *>(NativeInit)},
        {"nativeDispose", "(J)V", reinterpret_cast<void *>(NativeDispose)},
        {"nativeFinishInputEvent", "(JIZ)V", reinterpret_cast<void *>(NativeFinishInputEvent)},
        {"nativeProbablyHasInput", "(J)Z", reinterpret_cast<void *>(NativeProbablyHasInput)},
        {"nativeReportTimeline", "(JIJJ)V", reinterpret_cast<void *>(NativeReportTimeline)},
        {"nativeConsumeBatchedInputEvents", "(JJ)Z", reinterpret_cast<void *>(NativeConsumeBatchedInputEvents)},
        {"nativeDump", "(JLjava/lang/String;)Ljava/lang/String;", reinterpret_cast<void *>(NativeDump)},
};

}  // namespace

int register_android_view_InputEventReceiver(JNIEnv *env) {
    const int result = RegisterMethodsOrDie(env, "android/view/InputEventReceiver", gMethods, NELEM(gMethods));
    jclass clazz = FindClassOrDie(env, "android/view/InputEventReceiver");
    gInputEventReceiverClassInfo.dispatchInputEvent =
            GetMethodIDOrDie(env, clazz, "dispatchInputEvent", "(ILandroid/view/InputEvent;)V");
    gInputEventReceiverClassInfo.onBatchedInputEventPending =
            GetMethodIDOrDie(env, clazz, "onBatchedInputEventPending", "(I)V");
    jclass service = FindClassOrDie(env, "android/hardware/input/InputService");
    gInputServiceClassInfo.clazz = MakeGlobalRefOrDie(env, service);
    gInputServiceClassInfo.onHostDevice =
            GetStaticMethodIDOrDie(env, service, "onHostDevice", "(IIIIILjava/lang/String;)V");
    jclass methodManager = FindClassOrDie(env, "android/view/inputmethod/InputMethodManager");
    gInputMethodManagerClassInfo.clazz = MakeGlobalRefOrDie(env, methodManager);
    gInputMethodManagerClassInfo.onHostText =
            GetStaticMethodIDOrDie(env, methodManager, "onHostText", "(IIIJ[B)V");
    jclass looper = FindClassOrDie(env, "android/os/Looper");
    gLooperClassInfo.clazz = MakeGlobalRefOrDie(env, looper);
    gLooperClassInfo.myQueue = GetStaticMethodIDOrDie(env, looper, "myQueue", "()Landroid/os/MessageQueue;");
    return result;
}

}  // namespace android
