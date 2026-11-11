#include "network.h"

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <string>

#include "APKRuntime/apkrt.h"
#include "APKRuntime/guest_interface.h"
#include "network_link.h"
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

long call(std::uint64_t op, std::uint64_t argument, std::uint64_t size) {
    if (op == APKRT_NETWORK_GET) {
        const std::lock_guard<std::mutex> hold(mutex);
        if (!have_state) return -linux_errno::enodata;
        return static_cast<long>(state);
    }
    if (op == APKRT_NETWORK_LINK) {
        {
            const std::lock_guard<std::mutex> hold(mutex);
            if (!have_state) return -linux_errno::enodata;  // no active network reported yet
        }
        const link::Snapshot snapshot = link::read();  // the one source; no network mutex (netlink has its own)
        if (!snapshot.have) return -linux_errno::enodata;
        const std::string blob = link::serialize(snapshot);
        const std::size_t fits = std::min<std::size_t>(blob.size(), size);
        if (fits > 0 && !sys::memory::copy_checked(argument, reinterpret_cast<std::uint64_t>(blob.data()), fits))
            return -linux_errno::efault;
        return static_cast<long>(blob.size());
    }
    return -linux_errno::einval;
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
