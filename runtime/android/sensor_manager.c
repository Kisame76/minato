// libandroid.so's sensor manager (the Unity test APK probe: Unity asks ASensorManager_getInstance at its start): a manager
// without sensors, as SystemSensorManager's natives answer in libandroid_runtime (a named difference: the iPhone has
// them, a later step). The manager is one for the process, as Android's per package is; its lists are empty and it
// has no default sensor; an event queue can be made and destroyed and never has an event; a call that names a sensor
// gets -EINVAL, as Android's for a null sensor (BAD_VALUE). The direct channels and ASensor_* stay stubs that
// stubgen.py generates. The answers of an empty queue are Android 15's.
// sensor.h includes math.h, whose INFINITY and NAN bionic's source defines without a guard, after clang 21's float.h
// has defined them for gnu17: they go first, and math.h's stand.
#include <float.h>
#undef INFINITY
#undef NAN
#include <android/sensor.h>
#include <errno.h>
#include <stdlib.h>

struct ASensorManager {
    int unused;
};

struct ASensorEventQueue {
    int unused;
};

static ASensorManager the_manager;
static ASensorRef const no_sensors[1] = {NULL};

ASensorManager* ASensorManager_getInstance(void) {
    return &the_manager;
}

ASensorManager* ASensorManager_getInstanceForPackage(const char* packageName) {
    (void)packageName;
    return &the_manager;
}

int ASensorManager_getSensorList(ASensorManager* manager, ASensorList* list) {
    if (manager == NULL || list == NULL) return -EINVAL;
    *list = no_sensors;
    return 0;
}

ssize_t ASensorManager_getDynamicSensorList(ASensorManager* manager, ASensorList* list) {
    if (manager == NULL || list == NULL) return -EINVAL;
    *list = no_sensors;
    return 0;
}

ASensor const* ASensorManager_getDefaultSensor(ASensorManager* manager, int type) {
    (void)manager;
    (void)type;
    return NULL;
}

ASensor const* ASensorManager_getDefaultSensorEx(ASensorManager* manager, int type, bool wakeUp) {
    (void)manager;
    (void)type;
    (void)wakeUp;
    return NULL;
}

ASensorEventQueue* ASensorManager_createEventQueue(ASensorManager* manager, ALooper* looper, int ident,
                                                   ALooper_callbackFunc callback, void* data) {
    (void)ident;
    (void)callback;
    (void)data;
    if (manager == NULL || looper == NULL) return NULL;
    return calloc(1, sizeof(ASensorEventQueue));
}

int ASensorManager_destroyEventQueue(ASensorManager* manager, ASensorEventQueue* queue) {
    if (manager == NULL || queue == NULL) return -EINVAL;
    free(queue);
    return 0;
}

int ASensorEventQueue_registerSensor(ASensorEventQueue* queue, ASensor const* sensor, int32_t samplingPeriodUs,
                                     int64_t maxBatchReportLatencyUs) {
    (void)samplingPeriodUs;
    (void)maxBatchReportLatencyUs;
    return queue == NULL || sensor == NULL ? -EINVAL : 0;
}

int ASensorEventQueue_enableSensor(ASensorEventQueue* queue, ASensor const* sensor) {
    return queue == NULL || sensor == NULL ? -EINVAL : 0;
}

int ASensorEventQueue_disableSensor(ASensorEventQueue* queue, ASensor const* sensor) {
    return queue == NULL || sensor == NULL ? -EINVAL : 0;
}

int ASensorEventQueue_setEventRate(ASensorEventQueue* queue, ASensor const* sensor, int32_t usec) {
    (void)usec;
    return queue == NULL || sensor == NULL ? -EINVAL : 0;
}

// -1 for a queue without events [verified: sensors-test on apk-runner-35].
int ASensorEventQueue_hasEvents(ASensorEventQueue* queue) {
    return queue == NULL ? -EINVAL : -1;
}

// 0 events for a queue without any [verified: sensors-test on apk-runner-35].
ssize_t ASensorEventQueue_getEvents(ASensorEventQueue* queue, ASensorEvent* events, size_t count) {
    (void)events;
    (void)count;
    return queue == NULL ? -EINVAL : 0;
}
