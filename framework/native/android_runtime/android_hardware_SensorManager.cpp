// Ours (ADR-0045 decision 3, ADR-0061 step 2): SystemSensorManager's natives, which on Android stand on libsensor's
// SensorManager and the sensor service, on libandroid's ASensorManager (runtime/android/sensor_manager.c, the hub of
// ADR-0061) through dlopen, as the other natives of ours take libandroid's functions. The sensors are the hub's: what
// the host (CoreMotion) can feed, in Android's metadata. Where libandroid has no hub (an older runtime, or one without
// the private functions below) the natives answer as a device without any sensor, as before this step: no sensor at any
// index, no event queue.
//   - nativeGetSensorAtIndex fills android.hardware.Sensor as AOSP's translateNativeSensorToJavaSensor does, from the
//     NDK's getters and the hub's private ones (range, power, version, maximum delay, flags, required permission);
//   - an event queue is a Receiver: the hub's queue (minato_ASensorManager_createQueue) whose descriptor is watched on the
//     ALooper of the MessageQueue the listener asked for (android_os_MessageQueue_getALooper), reading whole ASensorEvents
//     with ASensorEventQueue_getEvents and handing them to dispatchSensorEvent, dispatchFlushCompleteEvent and
//     dispatchAdditionalInfoEvent as AOSP's Receiver::handleEvent does. A receiver lives until nativeDestroySensorEventQueue
//     and while a round runs, so that an unregister from the listener's callback frees nothing under the round;
//   - no direct channels, no data injection, no dynamic or runtime sensors (the iPhone has none).
// The names and signatures are those of AOSP's android_hardware_SensorManager.cpp at android-15.0.0_r1.
#define LOG_TAG "SensorManager"

#include <android/looper.h>
#include <android/sensor.h>
#include <android_runtime/AndroidRuntime.h>
#include <dlfcn.h>
#include <errno.h>
#include <jni.h>
#include <log/log.h>
#include <nativehelper/JNIHelp.h>
#include <nativehelper/scoped_local_ref.h>
#include <nativehelper/scoped_utf_chars.h>
#include <stdio.h>
#include <string.h>

#include <vector>

#include "core_jni_helpers.h"
#include "minato_message_queue.h"
#include <APKRuntime/guest_interface.h>

// The hub's private functions in libandroid (stubgen's private list of libandroid, ADR-0061 step 1).
extern "C" {
// One sensor's metadata from the hub's table (apkrt_sensor_info, whose handle is its type and whose fifo counts are 0).
// 0, or -EINVAL for a null sensor.
int minato_ASensor_getInfo(const ASensor *sensor, struct apkrt_sensor_info *out);
ASensorEventQueue *minato_ASensorManager_createQueue(ASensorManager *manager);
int minato_ASensorEventQueue_getFd(ASensorEventQueue *queue);
int minato_ASensorEventQueue_flush(ASensorEventQueue *queue);
}

// The libandroid functions the natives call, with the NDK header's types.
#define SENSOR_FUNCTIONS(X)                                                                                     \
    X(ASensorManager_getInstanceForPackage) X(ASensorManager_getSensorList) X(ASensorManager_destroyEventQueue) \
    X(ASensorEventQueue_registerSensor) X(ASensorEventQueue_disableSensor) X(ASensorEventQueue_getEvents)       \
    X(ASensor_getHandle)
#define MINATO_SENSOR_FUNCTIONS(X)                                                                              \
    X(minato_ASensor_getInfo) X(minato_ASensorManager_createQueue) X(minato_ASensorEventQueue_getFd) X(minato_ASensorEventQueue_flush)
#define LOOPER_FUNCTIONS(X) X(ALooper_acquire) X(ALooper_release) X(ALooper_addFd) X(ALooper_removeFd)

namespace android {
namespace {

struct Libandroid {
#define FIELD(name) decltype(&::name) name = nullptr;
    SENSOR_FUNCTIONS(FIELD)
    MINATO_SENSOR_FUNCTIONS(FIELD)
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
        SENSOR_FUNCTIONS(LOOK_UP)
        MINATO_SENSOR_FUNCTIONS(LOOK_UP)
        LOOPER_FUNCTIONS(LOOK_UP)
#undef LOOK_UP
        return found;
    }();
    return functions;
}

bool hub() {
    return libandroid().missing[0] == 0;
}

struct {
    jclass clazz;
    jmethodID dispatchSensorEvent;
    jmethodID dispatchFlushCompleteEvent;
    jmethodID dispatchAdditionalInfoEvent;
} gBaseEventQueueClassInfo;

struct {
    jclass clazz;
    jfieldID name, vendor, version, handle, range, resolution, power, minDelay, fifoReservedEventCount,
            fifoMaxEventCount, stringType, requiredPermission, maxDelay, flags;
    jmethodID setType, setId, setUuid, init;
} gSensorOffsets;

struct {
    jclass clazz;
    jmethodID add;
} gListOffsets;

struct {
    jclass clazz;
    jmethodID intern;
    jstring emptyString;
} gStringOffsets;

// nativeClassInit is not intended to be thread-safe: it runs before the other natives but nativeCreate.
void NativeClassInit(JNIEnv *env, jclass) {
    jclass sensorClass = reinterpret_cast<jclass>(MakeGlobalRefOrDie(env, FindClassOrDie(env, "android/hardware/Sensor")));
    gSensorOffsets.clazz = sensorClass;
    gSensorOffsets.name = GetFieldIDOrDie(env, sensorClass, "mName", "Ljava/lang/String;");
    gSensorOffsets.vendor = GetFieldIDOrDie(env, sensorClass, "mVendor", "Ljava/lang/String;");
    gSensorOffsets.version = GetFieldIDOrDie(env, sensorClass, "mVersion", "I");
    gSensorOffsets.handle = GetFieldIDOrDie(env, sensorClass, "mHandle", "I");
    gSensorOffsets.range = GetFieldIDOrDie(env, sensorClass, "mMaxRange", "F");
    gSensorOffsets.resolution = GetFieldIDOrDie(env, sensorClass, "mResolution", "F");
    gSensorOffsets.power = GetFieldIDOrDie(env, sensorClass, "mPower", "F");
    gSensorOffsets.minDelay = GetFieldIDOrDie(env, sensorClass, "mMinDelay", "I");
    gSensorOffsets.fifoReservedEventCount = GetFieldIDOrDie(env, sensorClass, "mFifoReservedEventCount", "I");
    gSensorOffsets.fifoMaxEventCount = GetFieldIDOrDie(env, sensorClass, "mFifoMaxEventCount", "I");
    gSensorOffsets.stringType = GetFieldIDOrDie(env, sensorClass, "mStringType", "Ljava/lang/String;");
    gSensorOffsets.requiredPermission = GetFieldIDOrDie(env, sensorClass, "mRequiredPermission", "Ljava/lang/String;");
    gSensorOffsets.maxDelay = GetFieldIDOrDie(env, sensorClass, "mMaxDelay", "I");
    gSensorOffsets.flags = GetFieldIDOrDie(env, sensorClass, "mFlags", "I");
    gSensorOffsets.setType = GetMethodIDOrDie(env, sensorClass, "setType", "(I)Z");
    gSensorOffsets.setId = GetMethodIDOrDie(env, sensorClass, "setId", "(I)V");
    gSensorOffsets.setUuid = GetMethodIDOrDie(env, sensorClass, "setUuid", "(JJ)V");
    gSensorOffsets.init = GetMethodIDOrDie(env, sensorClass, "<init>", "()V");

    jclass listClass = reinterpret_cast<jclass>(MakeGlobalRefOrDie(env, FindClassOrDie(env, "java/util/List")));
    gListOffsets.clazz = listClass;
    gListOffsets.add = GetMethodIDOrDie(env, listClass, "add", "(Ljava/lang/Object;)Z");

    gStringOffsets.clazz = MakeGlobalRefOrDie(env, FindClassOrDie(env, "java/lang/String"));
    gStringOffsets.intern = GetMethodIDOrDie(env, gStringOffsets.clazz, "intern", "()Ljava/lang/String;");
    ScopedLocalRef<jstring> empty(env, env->NewStringUTF(""));
    gStringOffsets.emptyString = reinterpret_cast<jstring>(
            MakeGlobalRefOrDie(env, env->CallObjectMethod(empty.get(), gStringOffsets.intern)));
}

// The types the NDK's enum does not name (hardware/sensors.h).
constexpr int kSensorTypeMetaData = 0;
constexpr int kSensorTypeOrientation = 3;

// What nativeCreate hands SystemSensorManager: the hub's ASensorManager, or 1 without a hub (never read then).
constexpr jlong kNoHub = 1;

jlong NativeCreate(JNIEnv *env, jclass, jstring opPackageName) {
    if (!hub()) {
        ALOGW("No sensors: %s", libandroid().missing);
        return kNoHub;
    }
    ScopedUtfChars name(env, opPackageName);
    ASensorManager *manager = libandroid().ASensorManager_getInstanceForPackage(name.c_str());
    return manager == nullptr ? kNoHub : reinterpret_cast<jlong>(manager);
}

ASensorManager *Manager(jlong handle) {
    return handle == kNoHub || handle == 0 ? nullptr : reinterpret_cast<ASensorManager *>(handle);
}

jstring InternedString(JNIEnv *env, const char *text) {
    if (text == nullptr || text[0] == '\0') return gStringOffsets.emptyString;
    ScopedLocalRef<jstring> string(env, env->NewStringUTF(text));
    return reinterpret_cast<jstring>(env->CallObjectMethod(string.get(), gStringOffsets.intern));
}

// As AOSP's translateNativeSensorToJavaSensor, over the hub's table. The id is the handle, and the UUID is zero, as the
// HAL's sensors without a UUID have. Sensor.mFlags holds the reporting mode in bits 1 to 3 (Sensor.REPORTING_MODE_SHIFT).
jobject ToJavaSensor(JNIEnv *env, jobject sensor, const ASensor *native) {
    apkrt_sensor_info info;
    if (libandroid().minato_ASensor_getInfo(native, &info) != 0) return nullptr;
    if (sensor == nullptr) {
        sensor = env->NewObject(gSensorOffsets.clazz, gSensorOffsets.init);
    }
    if (sensor == nullptr) return nullptr;
    env->SetObjectField(sensor, gSensorOffsets.name, InternedString(env, info.name));
    env->SetObjectField(sensor, gSensorOffsets.vendor, InternedString(env, info.vendor));
    env->SetIntField(sensor, gSensorOffsets.version, info.version);
    env->SetIntField(sensor, gSensorOffsets.handle, info.type);
    env->SetFloatField(sensor, gSensorOffsets.range, info.max_range);
    env->SetFloatField(sensor, gSensorOffsets.resolution, info.resolution);
    env->SetFloatField(sensor, gSensorOffsets.power, info.power_ma);
    env->SetIntField(sensor, gSensorOffsets.minDelay, info.min_delay_us);
    env->SetIntField(sensor, gSensorOffsets.fifoReservedEventCount, 0);
    env->SetIntField(sensor, gSensorOffsets.fifoMaxEventCount, 0);
    env->SetObjectField(sensor, gSensorOffsets.requiredPermission, InternedString(env, info.required_permission));
    env->SetIntField(sensor, gSensorOffsets.maxDelay, info.max_delay_us);
    env->SetIntField(sensor, gSensorOffsets.flags, info.flags | (info.reporting_mode << 1));
    if (env->CallBooleanMethod(sensor, gSensorOffsets.setType, info.type) == JNI_FALSE) {
        env->SetObjectField(sensor, gSensorOffsets.stringType, InternedString(env, info.string_type));
    }
    env->CallVoidMethod(sensor, gSensorOffsets.setId, info.type);
    env->CallVoidMethod(sensor, gSensorOffsets.setUuid, static_cast<jlong>(0), static_cast<jlong>(0));
    return sensor;
}

jboolean NativeGetSensorAtIndex(JNIEnv *env, jclass, jlong manager, jobject sensor, jint index) {
    ASensorManager *mgr = Manager(manager);
    if (mgr == nullptr || index < 0) return JNI_FALSE;
    ASensorList list = nullptr;
    const int count = libandroid().ASensorManager_getSensorList(mgr, &list);
    if (count <= index || list == nullptr) return JNI_FALSE;
    return ToJavaSensor(env, sensor, list[index]) != nullptr ? JNI_TRUE : JNI_FALSE;
}

void NativeGetSensors(JNIEnv *, jclass, jlong, jobject) {}

void NativeGetRuntimeSensors(JNIEnv *, jclass, jlong, jint, jobject) {}

jboolean NativeIsInjectionEnabled(JNIEnv *, jclass, jlong) {
    return JNI_FALSE;
}

jint NativeCreateDirectChannel(JNIEnv *, jclass, jlong, jint, jlong, jint, jint, jobject) {
    return -ENODEV;
}

void NativeDestroyDirectChannel(JNIEnv *, jclass, jlong, jint) {}

jint NativeConfigDirectChannel(JNIEnv *, jclass, jlong, jint, jint, jint) {
    return -ENODEV;
}

jint NativeSetOperationParameter(JNIEnv *, jclass, jlong, jint, jint, jfloatArray, jintArray) {
    return -ENODEV;
}

// One event queue: the hub's queue, watched on the listener's looper.
class Receiver {
public:
    Receiver(ASensorManager *manager, ASensorEventQueue *queue, ALooper *looper, jobject receiverWeak, JNIEnv *env)
          : mManager(manager),
            mQueue(queue),
            mLooper(looper),
            mReceiverWeakGlobal(env->NewGlobalRef(receiverWeak)),
            mFloatScratch(reinterpret_cast<jfloatArray>(env->NewGlobalRef(ScopedLocalRef<jobject>(env, env->NewFloatArray(16)).get()))),
            mIntScratch(reinterpret_cast<jintArray>(env->NewGlobalRef(ScopedLocalRef<jobject>(env, env->NewIntArray(16)).get()))) {
        libandroid().ALooper_acquire(mLooper);
    }

    bool watch() {
        return libandroid().ALooper_addFd(mLooper, libandroid().minato_ASensorEventQueue_getFd(mQueue),
                                          ALOOPER_POLL_CALLBACK, ALOOPER_EVENT_INPUT, HandleEvent, this) == 1;
    }

    ASensorEventQueue *queue() const { return mQueue; }
    ASensorManager *manager() const { return mManager; }

    // nativeDestroySensorEventQueue: the watch ends and the hub's queue goes; the receiver goes once no round runs.
    void destroy() {
        if (!mDestroyed) {
            mDestroyed = true;
            libandroid().ALooper_removeFd(mLooper, libandroid().minato_ASensorEventQueue_getFd(mQueue));
            libandroid().ASensorManager_destroyEventQueue(mManager, mQueue);
            mQueue = nullptr;
        }
        release();
    }

    void hold() { ++mHolds; }
    void release() {
        if (--mHolds == 0) delete this;
    }

    bool destroyed() const { return mDestroyed; }

private:
    ~Receiver() {
        JNIEnv *env = AndroidRuntime::getJNIEnv();
        if (env != nullptr) {
            env->DeleteGlobalRef(mReceiverWeakGlobal);
            env->DeleteGlobalRef(mFloatScratch);
            env->DeleteGlobalRef(mIntScratch);
        }
        libandroid().ALooper_release(mLooper);
    }

    static int HandleEvent(int, int events, void *data) {
        Receiver *receiver = static_cast<Receiver *>(data);
        receiver->hold();
        const int keep = receiver->consume(events);
        receiver->release();
        return keep;
    }

    // AOSP's Receiver::handleEvent over the NDK's queue: every whole event waiting, to the Java queue.
    int consume(int events) {
        if ((events & ALOOPER_EVENT_INPUT) == 0 || mDestroyed) return mDestroyed ? 0 : 1;
        JNIEnv *env = AndroidRuntime::getJNIEnv();
        ScopedLocalRef<jobject> receiverObj(env, GetReferent(env, mReceiverWeakGlobal));
        ASensorEvent buffer[16];
        ssize_t n;
        while (!mDestroyed && (n = libandroid().ASensorEventQueue_getEvents(mQueue, buffer, 16)) > 0) {
            for (ssize_t i = 0; i < n && !mDestroyed; i++) {
                const ASensorEvent &event = buffer[i];
                if (event.type == ASENSOR_TYPE_STEP_COUNTER) {
                    // the step counter is a uint64, the Java API deals with floats only
                    const float value = static_cast<float>(event.u64.step_counter);
                    env->SetFloatArrayRegion(mFloatScratch, 0, 1, &value);
                } else if (event.type == ASENSOR_TYPE_ADDITIONAL_INFO) {
                    env->SetIntArrayRegion(mIntScratch, 0, 14, event.additional_info.data_int32);
                    env->SetFloatArrayRegion(mFloatScratch, 0, 14, event.additional_info.data_float);
                } else {
                    env->SetFloatArrayRegion(mFloatScratch, 0, 16, event.data);
                }
                if (receiverObj.get() != nullptr) {
                    if (event.type == kSensorTypeMetaData) {
                        // a flush complete event
                        env->CallVoidMethod(receiverObj.get(), gBaseEventQueueClassInfo.dispatchFlushCompleteEvent,
                                            event.meta_data.sensor);
                    } else if (event.type == ASENSOR_TYPE_ADDITIONAL_INFO) {
                        env->CallVoidMethod(receiverObj.get(), gBaseEventQueueClassInfo.dispatchAdditionalInfoEvent,
                                            event.sensor, event.additional_info.type, event.additional_info.serial,
                                            mFloatScratch, mIntScratch, event.timestamp);
                    } else {
                        int8_t status = ASENSOR_STATUS_ACCURACY_HIGH;
                        switch (event.type) {
                            case kSensorTypeOrientation:
                            case ASENSOR_TYPE_MAGNETIC_FIELD:
                            case ASENSOR_TYPE_ACCELEROMETER:
                            case ASENSOR_TYPE_GYROSCOPE:
                            case ASENSOR_TYPE_GRAVITY:
                            case ASENSOR_TYPE_LINEAR_ACCELERATION:
                                status = event.vector.status;
                                break;
                            default:
                                break;
                        }
                        env->CallVoidMethod(receiverObj.get(), gBaseEventQueueClassInfo.dispatchSensorEvent,
                                            event.sensor, mFloatScratch, static_cast<jint>(status), event.timestamp);
                    }
                }
                if (env->ExceptionCheck()) {
                    ALOGE("Exception dispatching a sensor event.");
                    return 1;  // MessageQueue's poll rethrows it
                }
            }
        }
        return mDestroyed ? 0 : 1;
    }

    ASensorManager *const mManager;
    ASensorEventQueue *mQueue;
    ALooper *const mLooper;
    jobject mReceiverWeakGlobal;
    jfloatArray mFloatScratch;
    jintArray mIntScratch;
    bool mDestroyed = false;
    int mHolds = 1;
};

jlong NativeInitBaseEventQueue(JNIEnv *env, jclass, jlong manager, jobject eventQWeak, jobject messageQueue,
                               jstring, jint, jstring, jstring) {
    ASensorManager *mgr = Manager(manager);
    if (mgr == nullptr) {
        jniThrowRuntimeException(env, "Cannot construct native SensorEventQueue.");
        return 0;
    }
    ALooper *looper = android_os_MessageQueue_getALooper(env, messageQueue);
    if (looper == nullptr) {
        jniThrowRuntimeException(env, "MessageQueue is not initialized.");
        return 0;
    }
    ASensorEventQueue *queue = libandroid().minato_ASensorManager_createQueue(mgr);
    if (queue == nullptr) {
        jniThrowRuntimeException(env, "Cannot construct native SensorEventQueue.");
        return 0;
    }
    auto *receiver = new Receiver(mgr, queue, looper, eventQWeak, env);
    if (!receiver->watch()) {
        receiver->destroy();
        jniThrowRuntimeException(env, "Cannot construct native SensorEventQueue.");
        return 0;
    }
    return reinterpret_cast<jlong>(receiver);
}

Receiver *ReceiverOf(jlong eventQ) {
    return reinterpret_cast<Receiver *>(eventQ);
}

// The sensor of `handle`, found in the hub's list as the NDK has no lookup by handle.
const ASensor *SensorOfHandle(ASensorManager *manager, jint handle) {
    ASensorList list = nullptr;
    const int count = libandroid().ASensorManager_getSensorList(manager, &list);
    for (int i = 0; i < count && list != nullptr; i++) {
        if (libandroid().ASensor_getHandle(list[i]) == handle) return list[i];
    }
    return nullptr;
}

jint NativeEnableSensor(JNIEnv *, jclass, jlong eventQ, jint handle, jint rateUs, jint maxBatchReportLatencyUs) {
    Receiver *receiver = ReceiverOf(eventQ);
    if (receiver->destroyed()) return -EINVAL;
    ASensorManager *manager = receiver->manager();
    const ASensor *sensor = SensorOfHandle(manager, handle);
    return sensor == nullptr ? -EINVAL
                             : libandroid().ASensorEventQueue_registerSensor(receiver->queue(), sensor, rateUs,
                                                                             maxBatchReportLatencyUs);
}

jint NativeDisableSensor(JNIEnv *, jclass, jlong eventQ, jint handle) {
    Receiver *receiver = ReceiverOf(eventQ);
    if (receiver->destroyed()) return -EINVAL;
    const ASensor *sensor = SensorOfHandle(receiver->manager(), handle);
    return sensor == nullptr ? -EINVAL : libandroid().ASensorEventQueue_disableSensor(receiver->queue(), sensor);
}

void NativeDestroySensorEventQueue(JNIEnv *, jclass, jlong eventQ) {
    ReceiverOf(eventQ)->destroy();
}

jint NativeFlushSensor(JNIEnv *, jclass, jlong eventQ) {
    Receiver *receiver = ReceiverOf(eventQ);
    return receiver->destroyed() ? -EINVAL : libandroid().minato_ASensorEventQueue_flush(receiver->queue());
}

jint NativeInjectSensorData(JNIEnv *, jclass, jlong, jint, jfloatArray, jint, jlong) {
    return -ENODEV;
}

const JNINativeMethod gSystemSensorManagerMethods[] = {
        {"nativeClassInit", "()V", reinterpret_cast<void *>(NativeClassInit)},
        {"nativeCreate", "(Ljava/lang/String;)J", reinterpret_cast<void *>(NativeCreate)},
        {"nativeGetSensorAtIndex", "(JLandroid/hardware/Sensor;I)Z", reinterpret_cast<void *>(NativeGetSensorAtIndex)},
        {"nativeGetDefaultDeviceSensorAtIndex", "(JLandroid/hardware/Sensor;I)Z",
         reinterpret_cast<void *>(NativeGetSensorAtIndex)},
        {"nativeGetDynamicSensors", "(JLjava/util/List;)V", reinterpret_cast<void *>(NativeGetSensors)},
        {"nativeGetRuntimeSensors", "(JILjava/util/List;)V", reinterpret_cast<void *>(NativeGetRuntimeSensors)},
        {"nativeIsDataInjectionEnabled", "(J)Z", reinterpret_cast<void *>(NativeIsInjectionEnabled)},
        {"nativeIsReplayDataInjectionEnabled", "(J)Z", reinterpret_cast<void *>(NativeIsInjectionEnabled)},
        {"nativeIsHalBypassReplayDataInjectionEnabled", "(J)Z", reinterpret_cast<void *>(NativeIsInjectionEnabled)},
        {"nativeCreateDirectChannel", "(JIJIILandroid/hardware/HardwareBuffer;)I",
         reinterpret_cast<void *>(NativeCreateDirectChannel)},
        {"nativeDestroyDirectChannel", "(JI)V", reinterpret_cast<void *>(NativeDestroyDirectChannel)},
        {"nativeConfigDirectChannel", "(JIII)I", reinterpret_cast<void *>(NativeConfigDirectChannel)},
        {"nativeSetOperationParameter", "(JII[F[I)I", reinterpret_cast<void *>(NativeSetOperationParameter)},
};

const JNINativeMethod gBaseEventQueueMethods[] = {
        {"nativeInitBaseEventQueue",
         "(JLjava/lang/ref/WeakReference;Landroid/os/MessageQueue;Ljava/lang/String;ILjava/lang/String;"
         "Ljava/lang/String;)J",
         reinterpret_cast<void *>(NativeInitBaseEventQueue)},
        {"nativeEnableSensor", "(JIII)I", reinterpret_cast<void *>(NativeEnableSensor)},
        {"nativeDisableSensor", "(JI)I", reinterpret_cast<void *>(NativeDisableSensor)},
        {"nativeDestroySensorEventQueue", "(J)V", reinterpret_cast<void *>(NativeDestroySensorEventQueue)},
        {"nativeFlushSensor", "(J)I", reinterpret_cast<void *>(NativeFlushSensor)},
        {"nativeInjectSensorData", "(JI[FIJ)I", reinterpret_cast<void *>(NativeInjectSensorData)},
};

}  // namespace

int register_android_hardware_SensorManager(JNIEnv *env) {
    RegisterMethodsOrDie(env, "android/hardware/SystemSensorManager", gSystemSensorManagerMethods,
                         NELEM(gSystemSensorManagerMethods));
    RegisterMethodsOrDie(env, "android/hardware/SystemSensorManager$BaseEventQueue", gBaseEventQueueMethods,
                         NELEM(gBaseEventQueueMethods));
    gBaseEventQueueClassInfo.clazz = FindClassOrDie(env, "android/hardware/SystemSensorManager$BaseEventQueue");
    gBaseEventQueueClassInfo.dispatchSensorEvent =
            GetMethodIDOrDie(env, gBaseEventQueueClassInfo.clazz, "dispatchSensorEvent", "(I[FIJ)V");
    gBaseEventQueueClassInfo.dispatchFlushCompleteEvent =
            GetMethodIDOrDie(env, gBaseEventQueueClassInfo.clazz, "dispatchFlushCompleteEvent", "(I)V");
    gBaseEventQueueClassInfo.dispatchAdditionalInfoEvent =
            GetMethodIDOrDie(env, gBaseEventQueueClassInfo.clazz, "dispatchAdditionalInfoEvent", "(III[F[I)V");
    return 0;
}

}  // namespace android
