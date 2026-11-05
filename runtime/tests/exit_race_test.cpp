// Regression for the exit-time abort (ADR-0035 landing, the host-callback-during-exit fix): the host calls
// apkrt_network_set (and the other apkrt_*_set/push entry points) from an OS callback that can run while exit()
// destroys the static objects. A namespace-scope std::mutex was destroyed first, so locking it aborted the process
// ("mutex lock failed: Invalid argument") about one run in four under load, and potentially at any app exit on the
// iPhone. A function with __attribute__((destructor)) runs after the C++ static destructors -- the moment a plain
// namespace-scope mutex would already be gone -- so this reproduces the abort deterministically, with no thread race:
// before the fix the call locks a destroyed mutex and aborts; with the mutex made immortal it just sets the state and
// the process exits cleanly (status 0). The network setter stands in for all the host-callback states (battery,
// sensors, location, audio), which take the same immortal-mutex treatment.
#include "APKRuntime/apkrt.h"

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
}
}  // namespace

int main() {
    // apkrt_network_set is also exercised normally before exit, so the state and lock work during the run.
    apkrt_network_set(0);
    return 0;
}
