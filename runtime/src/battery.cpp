#include "battery.h"

#include <cstdint>
#include <mutex>

#include "APKRuntime/apkrt.h"
#include "APKRuntime/guest_interface.h"
#include "syscalls.h"

namespace apkrt::battery {
namespace {

namespace linux_errno = sys::linux_errno;

std::mutex mutex;
bool have_value = false;                             // a value has been set; GET is -ENODATA until then
std::int32_t level_percent = 0;                      // 0..100, or -1 when unknown
std::int32_t state = APKRT_BATTERY_STATE_UNKNOWN;
bool posted = false;                                 // a value has been announced; the sentinel until the first post
std::int32_t posted_level = 0;
std::int32_t posted_state = 0;

/// The current value packed for APKRT_BATTERY_GET: the level in the low 32 bits (0xFFFFFFFF when unknown), the state
/// in the high 32. The level is at most 0xFFFFFFFF and the state is small, so the result is always non-negative and
/// never aliases a negative errno.
long packed() {
    return static_cast<long>(static_cast<std::uint64_t>(static_cast<std::uint32_t>(level_percent)) |
                             (static_cast<std::uint64_t>(static_cast<std::uint32_t>(state)) << 32));
}

}  // namespace

long call(std::uint64_t op) {
    if (op != APKRT_BATTERY_GET) return -linux_errno::einval;
    const std::lock_guard<std::mutex> hold(mutex);
    if (!have_value) return -linux_errno::enodata;
    return packed();
}

void reset() {
    const std::lock_guard<std::mutex> hold(mutex);
    have_value = false;
    posted = false;
    level_percent = 0;
    state = APKRT_BATTERY_STATE_UNKNOWN;
    posted_level = 0;
    posted_state = 0;
}

}  // namespace apkrt::battery

extern "C" void apkrt_battery_set(int32_t new_level, int32_t new_state) {
    const std::lock_guard<std::mutex> hold(apkrt::battery::mutex);
    apkrt::battery::have_value = true;
    apkrt::battery::level_percent = new_level;
    apkrt::battery::state = new_state;
    // Announce only on a change, and only while a session runs. apkrt_session_post returns 0 when no session is
    // begun, so a value set before the session is stored (GET reads it) but never announced. The last-posted value
    // is updated only on a post that went through, so the first post after a session begins always goes (the
    // sentinel), as Android re-sends the sticky ACTION_BATTERY_CHANGED to a new receiver.
    if (apkrt::battery::posted && new_level == apkrt::battery::posted_level &&
        new_state == apkrt::battery::posted_state) {
        return;  // unchanged since the last post: Android broadcasts nothing on no change
    }
    const std::uint64_t a0 = static_cast<std::uint64_t>(static_cast<std::int64_t>(new_level));  // -1 -> all ones
    const std::uint64_t a1 = static_cast<std::uint64_t>(static_cast<std::uint32_t>(new_state));
    if (apkrt_session_post(APKRT_EVENT_BATTERY, a0, a1, 0) != 0) {
        apkrt::battery::posted = true;
        apkrt::battery::posted_level = new_level;
        apkrt::battery::posted_state = new_state;
    }
}
