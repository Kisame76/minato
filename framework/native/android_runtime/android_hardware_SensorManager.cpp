// Ours (ADR-0045 decision 3): SystemSensorManager's natives, which on Android stand on libsensor's SensorManager and
// the sensor service. Minato has no sensors (decision 4), so they answer as a device without any: no sensor at any
// index, no dynamic or runtime sensor, no data injection. Creating a direct channel, setting an operation parameter
// and every call of an event queue fail with -ENODEV, which SystemSensorManager reports as AOSP's does a refusal of
// the service; no app reaches an event queue, since registering needs a sensor. The names and signatures are those of
// AOSP's android_hardware_SensorManager.cpp at android-15.0.0_r1, registered at its place in gRegJNI.
#include <errno.h>
#include <jni.h>
#include <nativehelper/JNIHelp.h>

#include "core_jni_helpers.h"

namespace android {
namespace {

// What nativeCreate hands SystemSensorManager for its later calls: never 0, never read.
constexpr jlong kSensorManager = 1;

void NativeClassInit(JNIEnv *, jclass) {}

jlong NativeCreate(JNIEnv *, jclass, jstring) {
    return kSensorManager;
}

jboolean NativeGetSensorAtIndex(JNIEnv *, jclass, jlong, jobject, jint) {
    return JNI_FALSE;
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

jlong NativeInitBaseEventQueue(JNIEnv *, jclass, jlong, jobject, jobject, jstring, jint, jstring, jstring) {
    return 0;
}

jint NativeEnableSensor(JNIEnv *, jclass, jlong, jint, jint, jint) {
    return -ENODEV;
}

jint NativeDisableSensor(JNIEnv *, jclass, jlong, jint) {
    return -ENODEV;
}

void NativeDestroySensorEventQueue(JNIEnv *, jclass, jlong) {}

jint NativeFlushSensor(JNIEnv *, jclass, jlong) {
    return -ENODEV;
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
    return RegisterMethodsOrDie(env, "android/hardware/SystemSensorManager$BaseEventQueue", gBaseEventQueueMethods,
                                NELEM(gBaseEventQueueMethods));
}

}  // namespace android
