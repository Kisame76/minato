// libandroid.so's sensor manager (ADR-0061): the NDK ASensor API on the runtime's sensor hub, reached through the
// APKRT_NR_SENSOR syscall. getInstance gives the one manager; the offered sensors are cached once from APKRT_SENSOR_LIST
// as ASensor objects (an ASensor const* points into the cached table, so the ASensor_* getters read it). An event queue
// is a guest descriptor the runtime hands back (the read end of the hub's pipe); the guest reads whole ASensorEvent
// records from it with read(), polls it on its looper, and closes it to stop its sensors. The private minato_* exports
// serve the Java SystemSensorManager (step 2): a queue without a looper, its descriptor, a flush, and the full info.
//
// sensor.h includes math.h, whose INFINITY and NAN bionic's source defines without a guard, after clang 21's float.h
// has defined them for gnu17: they go first, and math.h's stand.
#include <float.h>
#undef INFINITY
#undef NAN
#include <android/looper.h>
#include <android/sensor.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>

#include "APKRuntime/guest_interface.h"

// The hub writes its event records in the ASensorEvent layout; the guest reads them as ASensorEvent, so the two must
// agree byte for byte (the hub asserts its own 104-byte size in sensor_hub.h).
_Static_assert(sizeof(ASensorEvent) == 104, "ASensorEvent is a 104-byte ABI");

struct ASensorManager {
    int unused;
};

struct ASensorEventQueue {
    int fd;           // the read end of the hub's pipe, a guest descriptor
    ALooper* looper;  // the looper createEventQueue added the fd to; NULL for minato_ASensorManager_createQueue
};

static ASensorManager the_manager;

// The offered sensors, cached once; an ASensor const* is &table[i], and sensor_refs is the ASensorList of those.
static struct apkrt_sensor_info table[APKRT_SENSOR_MAX];
static ASensorRef sensor_refs[APKRT_SENSOR_MAX];
static int table_count = 0;
static pthread_once_t table_once = PTHREAD_ONCE_INIT;

static void load_table(void) {
    struct apkrt_sensor_list list;
    const long count = syscall(APKRT_NR_SENSOR, APKRT_SENSOR_LIST, &list);
    if (count < 0) return;  // no host, no sensors; table_count stays 0
    table_count = count < (long)APKRT_SENSOR_MAX ? (int)count : APKRT_SENSOR_MAX;
    for (int i = 0; i < table_count; ++i) {
        table[i] = list.sensors[i];
        sensor_refs[i] = (ASensorRef)&table[i];
    }
}

static void ensure_table(void) { pthread_once(&table_once, load_table); }

static const struct apkrt_sensor_info* info_of(ASensor const* sensor) {
    return (const struct apkrt_sensor_info*)sensor;
}

ASensorManager* ASensorManager_getInstance(void) { return &the_manager; }

ASensorManager* ASensorManager_getInstanceForPackage(const char* packageName) {
    (void)packageName;
    return &the_manager;
}

int ASensorManager_getSensorList(ASensorManager* manager, ASensorList* list) {
    if (manager == NULL || list == NULL) return -EINVAL;
    ensure_table();
    *list = sensor_refs;
    return table_count;
}

ssize_t ASensorManager_getDynamicSensorList(ASensorManager* manager, ASensorList* list) {
    if (manager == NULL || list == NULL) return -EINVAL;
    static ASensorRef const none[1] = {NULL};
    *list = none;
    return 0;  // the iPhone's sensors do not come and go
}

ASensor const* ASensorManager_getDefaultSensor(ASensorManager* manager, int type) {
    if (manager == NULL) return NULL;
    ensure_table();
    for (int i = 0; i < table_count; ++i) {
        if (table[i].type == type) return (ASensor const*)&table[i];
    }
    return NULL;
}

ASensor const* ASensorManager_getDefaultSensorEx(ASensorManager* manager, int type, bool wakeUp) {
    return wakeUp ? NULL : ASensorManager_getDefaultSensor(manager, type);  // none of ours is a wake-up sensor
}

ASensorEventQueue* ASensorManager_createEventQueue(ASensorManager* manager, ALooper* looper, int ident,
                                                   ALooper_callbackFunc callback, void* data) {
    if (manager == NULL || looper == NULL) return NULL;
    const long fd = syscall(APKRT_NR_SENSOR, APKRT_SENSOR_OPEN, 0);
    if (fd < 0) return NULL;
    ASensorEventQueue* queue = calloc(1, sizeof(ASensorEventQueue));
    if (queue == NULL) {
        close((int)fd);
        return NULL;
    }
    queue->fd = (int)fd;
    queue->looper = looper;
    ALooper_addFd(looper, queue->fd, ident, ALOOPER_EVENT_INPUT, callback, data);
    return queue;
}

int ASensorManager_destroyEventQueue(ASensorManager* manager, ASensorEventQueue* queue) {
    if (manager == NULL || queue == NULL) return -EINVAL;
    // Remove the fd from the looper before closing it, as AOSP does: otherwise the looper keeps the registration and a
    // later fd that reuses the number would fire this queue's freed callback and data (a use-after-free).
    if (queue->looper != NULL) ALooper_removeFd(queue->looper, queue->fd);
    close(queue->fd);  // the runtime's descriptor destructor drops the queue and stops the sensors it alone enabled
    free(queue);
    return 0;
}

// Begins or re-rates `sensor` on `queue` at `period_us` microseconds (<= 0 asks the hub's default); the request struct
// goes to the runtime, which clamps the rate and validates the type.
static int enable_at(ASensorEventQueue* queue, ASensor const* sensor, long period_us) {
    if (queue == NULL || sensor == NULL) return -EINVAL;
    struct apkrt_sensor_request request = {queue->fd, info_of(sensor)->type, period_us > 0 ? period_us * 1000 : 0};
    return (int)syscall(APKRT_NR_SENSOR, APKRT_SENSOR_ENABLE, &request);
}

int ASensorEventQueue_registerSensor(ASensorEventQueue* queue, ASensor const* sensor, int32_t samplingPeriodUs,
                                     int64_t maxBatchReportLatencyUs) {
    (void)maxBatchReportLatencyUs;  // no batching: the hub reports fifoMaxEventCount 0
    if (samplingPeriodUs < 0) return -EINVAL;  // a negative rate is BAD_VALUE, as on Android
    return enable_at(queue, sensor, samplingPeriodUs);
}

int ASensorEventQueue_enableSensor(ASensorEventQueue* queue, ASensor const* sensor) {
    return enable_at(queue, sensor, 0);  // the hub's default rate (its minimum delay)
}

int ASensorEventQueue_disableSensor(ASensorEventQueue* queue, ASensor const* sensor) {
    if (queue == NULL || sensor == NULL) return -EINVAL;
    struct apkrt_sensor_request request = {queue->fd, info_of(sensor)->type, 0};
    return (int)syscall(APKRT_NR_SENSOR, APKRT_SENSOR_DISABLE, &request);
}

int ASensorEventQueue_setEventRate(ASensorEventQueue* queue, ASensor const* sensor, int32_t usec) {
    if (queue == NULL || sensor == NULL || usec < 0) return -EINVAL;
    struct apkrt_sensor_request request = {queue->fd, info_of(sensor)->type, (int64_t)usec * 1000};
    return (int)syscall(APKRT_NR_SENSOR, APKRT_SENSOR_RATE, &request);
}

// 1 when events wait, -1 when none do (Android's libandroid answers -1 whenever the poll's revents is not POLLIN, an
// empty queue included, as the AVD's sensors-test shows), a negative errno when the descriptor cannot be polled.
int ASensorEventQueue_hasEvents(ASensorEventQueue* queue) {
    if (queue == NULL) return -EINVAL;
    struct pollfd entry = {queue->fd, POLLIN, 0};
    const int ready = poll(&entry, 1, 0);
    if (ready < 0) return -errno;
    if (entry.revents != POLLIN) return -1;
    return ready == 0 ? 0 : 1;
}

// The whole ASensorEvent records waiting, up to `count`; 0 when none, a negative errno on a read error.
ssize_t ASensorEventQueue_getEvents(ASensorEventQueue* queue, ASensorEvent* events, size_t count) {
    if (queue == NULL || events == NULL) return -EINVAL;
    if (count > 4096) count = 4096;  // cap so count * sizeof cannot overflow; the pipe holds far fewer anyway
    const ssize_t got = read(queue->fd, events, count * sizeof(ASensorEvent));
    if (got >= 0) return got / (ssize_t)sizeof(ASensorEvent);  // whole records: the hub writes them atomically
    return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -errno;
}

// The ASensor_* getters read the cached record the ASensor const* points into (ADR-0061). handle == type.
int ASensor_getType(ASensor const* sensor) { return sensor == NULL ? ASENSOR_TYPE_INVALID : info_of(sensor)->type; }
int ASensor_getHandle(ASensor const* sensor) { return sensor == NULL ? 0 : info_of(sensor)->type; }
const char* ASensor_getName(ASensor const* sensor) { return sensor == NULL ? "" : info_of(sensor)->name; }
const char* ASensor_getVendor(ASensor const* sensor) { return sensor == NULL ? "" : info_of(sensor)->vendor; }
const char* ASensor_getStringType(ASensor const* sensor) { return sensor == NULL ? "" : info_of(sensor)->string_type; }
int ASensor_getMinDelay(ASensor const* sensor) { return sensor == NULL ? 0 : info_of(sensor)->min_delay_us; }
float ASensor_getResolution(ASensor const* sensor) { return sensor == NULL ? 0.0f : info_of(sensor)->resolution; }
int ASensor_getReportingMode(ASensor const* sensor) {
    return sensor == NULL ? AREPORTING_MODE_INVALID : info_of(sensor)->reporting_mode;
}

// Private exports for the Java SystemSensorManager (ADR-0061 step 2, stubgen's APKRT_PRIVATE list).
int minato_ASensor_getInfo(ASensor const* sensor, struct apkrt_sensor_info* out) {
    if (sensor == NULL || out == NULL) return -EINVAL;
    *out = *info_of(sensor);
    return 0;
}

// A queue without a looper: the Java side adds the descriptor to its own MessageQueue Looper and reads with the public
// ASensorEventQueue_getEvents. Destroyed by the public ASensorManager_destroyEventQueue.
ASensorEventQueue* minato_ASensorManager_createQueue(ASensorManager* manager) {
    if (manager == NULL) return NULL;
    const long fd = syscall(APKRT_NR_SENSOR, APKRT_SENSOR_OPEN, 0);
    if (fd < 0) return NULL;
    ASensorEventQueue* queue = calloc(1, sizeof(ASensorEventQueue));
    if (queue == NULL) {
        close((int)fd);
        return NULL;
    }
    queue->fd = (int)fd;
    return queue;
}

int minato_ASensorEventQueue_getFd(ASensorEventQueue* queue) { return queue == NULL ? -EINVAL : queue->fd; }

int minato_ASensorEventQueue_flush(ASensorEventQueue* queue) {
    if (queue == NULL) return -EINVAL;
    return (int)syscall(APKRT_NR_SENSOR, APKRT_SENSOR_FLUSH, queue->fd);
}
