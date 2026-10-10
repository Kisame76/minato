// sensors-test: libandroid's sensor manager as an app first meets it (the Unity test APK probe: Unity asks for the manager
// at its start): the manager, the same for a package, an event queue on the thread's looper, and the answers of a
// queue that has no sensor, then its destroy. What the device has is not shown: the emulator has 18 sensors, an
// accelerometer among them, and Minato none yet.
#include <android/looper.h>
#include <android/sensor.h>
#include <stdio.h>

int main(void) {
    ASensorManager* manager = ASensorManager_getInstance();
    printf("manager: %s\n", manager != NULL ? "yes" : "no");
    ASensorManager* for_package = ASensorManager_getInstanceForPackage("io.github.kisame76.minato.test");
    printf("manager for a package: %s\n", for_package != NULL ? "yes" : "no");
    ASensorList list = NULL;
    const int count = ASensorManager_getSensorList(manager, &list);
    printf("sensor list: %s\n", count >= 0 && list != NULL ? "given" : "missing");
    ALooper* looper = ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);
    ASensorEventQueue* queue = ASensorManager_createEventQueue(manager, looper, 3, NULL, NULL);
    printf("event queue: %s\n", queue != NULL ? "yes" : "no");
    if (queue == NULL) return 1;
    ASensorEvent events[4];
    printf("events of an empty queue: %zd\n", ASensorEventQueue_getEvents(queue, events, 4));
    printf("has events: %d\n", ASensorEventQueue_hasEvents(queue));
    printf("register a null sensor: %d\n", ASensorEventQueue_registerSensor(queue, NULL, 20000, 0));
    printf("enable a null sensor: %d\n", ASensorEventQueue_enableSensor(queue, NULL));
    printf("disable a null sensor: %d\n", ASensorEventQueue_disableSensor(queue, NULL));
    printf("rate of a null sensor: %d\n", ASensorEventQueue_setEventRate(queue, NULL, 20000));
    printf("destroy: %d\n", ASensorManager_destroyEventQueue(manager, queue));
    return 0;
}
