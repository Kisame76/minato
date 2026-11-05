#include "network.h"

#include <cstdint>
#include <mutex>

#include "APKRuntime/apkrt.h"
#include "APKRuntime/guest_interface.h"
#include "syscalls.h"

namespace apkrt::network {
namespace {

namespace linux_errno = sys::linux_errno;

// Constant-initialized and never destroyed: the host calls apkrt_network_set from a callback (NWPathMonitor on a
// dispatch thread) that can run both before main -- another translation unit's constructor reaches it, as graphics-run
// does through minato_graphics_set_providers -- and while exit() runs the static destructors. constinit gives it
// constant (static) initialization, so it is ready before any constructor runs (std::mutex's constructor is
// constexpr); [[clang::no_destroy]] keeps it past the destructors, so a late lock never hits a destroyed mutex. A
// dynamically initialized reference (*new std::mutex) binds to null for a pre-main caller, and a plain object is
// destroyed at exit -- this avoids both.
[[clang::no_destroy]] constinit std::mutex mutex;
bool have_state = false;  // a state has been set; GET is -ENODATA until then
std::uint32_t state = 0;
bool posted = false;  // a state has been announced; the sentinel until the first post
std::uint32_t posted_state = 0;

}  // namespace

long call(std::uint64_t op) {
    if (op != APKRT_NETWORK_GET) return -linux_errno::einval;
    const std::lock_guard<std::mutex> hold(mutex);
    if (!have_state) return -linux_errno::enodata;
    return static_cast<long>(state);
}

void reset() {
    const std::lock_guard<std::mutex> hold(mutex);
    have_state = false;
    state = 0;
    posted = false;
    posted_state = 0;
}

}  // namespace apkrt::network

extern "C" void apkrt_network_set(uint32_t new_state) {
    const std::lock_guard<std::mutex> hold(apkrt::network::mutex);
    apkrt::network::have_state = true;
    apkrt::network::state = new_state;
    // Announced only on a change and only while a session runs, as the battery's (battery.cpp): apkrt_session_post
    // returns 0 without a session, so the first post in a session always goes, and the guest reads the state with GET
    // before any event.
    if (apkrt::network::posted && new_state == apkrt::network::posted_state) return;
    if (apkrt_session_post(APKRT_EVENT_NETWORK, new_state, 0, 0) != 0) {
        apkrt::network::posted = true;
        apkrt::network::posted_state = new_state;
    }
}
