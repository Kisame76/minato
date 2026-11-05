// Regression for the exit-time abort (ADR-0035 landing, the host-callback-during-exit fix): the host calls
// apkrt_network_set (and the other apkrt_*_set/push entry points) from an OS callback that can run while exit()
// destroys the static objects. A namespace-scope std::mutex was destroyed first, so locking it aborted the process
// ("mutex lock failed: Invalid argument") about one run in four under load, and potentially at any app exit on the
// iPhone. A function with __attribute__((destructor)) runs after the C++ static destructors -- the moment a plain
// namespace-scope mutex would already be gone -- so this reproduces the abort deterministically, with no thread race:
// before the fix the call locks a destroyed mutex and aborts; with the mutex made immortal it just sets the state and
// the process exits cleanly (status 0). The network setter stands in for the states that hold nothing but plain
// data (battery, audio); the sensor and location hubs also hold std::vector statics under their immortal mutexes, so
// a call after the destructors would write into freed memory: they are called as well, with vectors that have
// content (the available sensor types, a pending one-shot location). Run it under AddressSanitizer (APK_SANITIZE),
// which reports the heap-use-after-free that an immortal mutex alone leaves.
#include "APKRuntime/apkrt.h"
#include "location_hub.h"
#include "permissions.h"

namespace {
// Before main: another translation unit's constructor reaches apkrt_network_set before network.cpp's own
// initializer, as graphics-run does through minato_graphics_set_providers. A dynamically initialized mutex reference
// is still null here and the lock binds to null (UBSan: reference binding to null, abort). Constant initialization is
// ready; this must not abort.
__attribute__((constructor)) void a_host_callback_before_main() {
    apkrt_network_set(1);
}

// After the C++ static destructors: a plain namespace-scope mutex is already destroyed and the lock aborts. An
// immortal mutex must not abort.
__attribute__((destructor)) void a_host_callback_after_the_static_destructors() {
    apkrt_network_set(0);
    // The hubs' vectors are not allowed to be destroyed: a call that writes to one (a guest thread still running while
    // exit() runs, the host registering its sensors again) would write into the memory it freed, which a destroyed
    // vector still points at, and an immortal mutex lets that call through. Reads of the emptied vector are harmless
    // with libc++, so the writes are what the test needs.
    const std::int32_t available[2] = {1, 4};
    static const apkrt_sensor_host sensors{};
    apkrt_sensor_set_host(&sensors, available, 2);
    apkrt::location::request_single(8);
    const float values[3] = {0.0f, 9.8f, 0.0f};
    apkrt_sensor_push(1, values, 3, 1.0, 3);
    apkrt_location_fix fix{};
    fix.latitude = 52.5;
    fix.longitude = 13.4;
    fix.horizontal_accuracy_m = 10.0f;
    fix.valid = 2;  // APKRT_LOCATION_HAS_HORIZONTAL_ACCURACY
    apkrt_location_push_single(7, &fix);
    // The permission store's mutex is a function-local static, destroyed at exit as a namespace-scope one is; the
    // location hub reads it at every delivery. The audio hub's slots hold a mutex each.
    apkrt::permissions::set(APKRT_PERMISSION_LOCATION, APKRT_PERMISSION_DENIED);
    apkrt_audio_disconnected(1);
}
}  // namespace

int main() {
    // apkrt_network_set is also exercised normally before exit, so the state and lock work during the run.
    apkrt_network_set(0);
    // Content for the hubs' vectors to hold when exit() destroys them: one available sensor type (1, the
    // accelerometer) and one pending one-shot location request (token 7), under a granted location permission.
    static const apkrt_sensor_host sensors{};
    const std::int32_t available[1] = {1};
    apkrt_sensor_set_host(&sensors, available, 1);
    apkrt::permissions::set(APKRT_PERMISSION_LOCATION, APKRT_PERMISSION_GRANTED);
    apkrt::location::request_single(7);
    return 0;
}
