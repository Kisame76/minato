// The battery transport (App's gap-1 proposal, 2026-10-29): APKRT_NR_BATTERY (GET) through the real dispatcher, and
// apkrt_battery_set announcing APKRT_EVENT_BATTERY through the session event stream. GET reads the current value and
// is -ENODATA before the first set; a set is announced only on a change and only while a session runs, so a value
// set before the session is stored (GET sees it) but not announced. The state values are UIDevice.batteryState raw,
// passed through unchanged. A process has one session, so only the lifecycle test begins it.

#include "battery.h"
#include "syscall_frame.h"

#include <cstdint>

#include "APKRuntime/apkrt.h"
#include "APKRuntime/guest_interface.h"
#include "check.h"

namespace {

constexpr long enodata = 61, einval = 22;
constexpr std::uint64_t nr_read = 63;

template <typename T>
std::uint64_t address(T *pointer) {
    return reinterpret_cast<std::uint64_t>(pointer);
}

long call(std::uint64_t number, std::uint64_t a0 = 0, std::uint64_t a1 = 0, std::uint64_t a2 = 0) {
    apkrt_syscall_frame frame{};
    frame.x[0] = a0;
    frame.x[1] = a1;
    frame.x[2] = a2;
    frame.x[8] = number;
    apkrt_syscall_dispatch(&frame);
    return static_cast<long>(frame.x[0]);
}

long battery(std::uint64_t op = APKRT_BATTERY_GET) { return call(APKRT_NR_BATTERY, op); }

/// The current value packed as APKRT_BATTERY_GET returns it: the level in the low 32 bits, the state in the high 32.
std::uint64_t packed(std::int32_t level, std::int32_t state) {
    return static_cast<std::uint64_t>(static_cast<std::uint32_t>(level)) |
           (static_cast<std::uint64_t>(static_cast<std::uint32_t>(state)) << 32);
}

int events = -1;  // the event-pipe descriptor from SESSION_CONNECT

void on_end(void *, int) {}

// Begins the process's session and connects the event pipe, once. Called only by the lifecycle test, after its
// pre-session checks, so that no session is begun while those run.
void begin_session() {
    static bool done = false;
    if (done) return;
    done = true;
    static apkrt_session session{};
    session.end = on_end;
    session.configuration.scale = 3.0;
    session.configuration.width = 1179;
    session.configuration.height = 2556;
    session.configuration.orientation = 1;
    CHECK(apkrt_session_begin(&session) == 0);
    apkrt_configuration received{};
    const long descriptor = call(APKRT_NR_SESSION_CONNECT, address(&received));
    CHECK(descriptor >= 0);
    events = static_cast<int>(descriptor);
}

apkrt_event read_event() {
    apkrt_event record{};
    const long got = call(nr_read, static_cast<std::uint64_t>(events), address(&record), sizeof record);
    CHECK(got == static_cast<long>(sizeof record));
    return record;
}

}  // namespace

TEST(battery_get_is_enodata_before_a_set) {
    apkrt::battery::reset();
    CHECK(battery() == -enodata);
}

TEST(battery_get_packs_the_level_and_state) {
    apkrt::battery::reset();
    apkrt_battery_set(80, APKRT_BATTERY_STATE_CHARGING);
    CHECK(battery() == static_cast<long>(packed(80, APKRT_BATTERY_STATE_CHARGING)));
    apkrt_battery_set(-1, APKRT_BATTERY_STATE_UNKNOWN);  // unknown level: 0xFFFFFFFF in the low half
    CHECK(battery() == static_cast<long>(packed(-1, APKRT_BATTERY_STATE_UNKNOWN)));
    CHECK(battery() >= 0);  // the packed value is always non-negative, never a false errno
}

TEST(a_bad_battery_op_is_einval) {
    CHECK(battery(1) == -einval);
    CHECK(battery(999) == -einval);
}

// The lifecycle: a set before the session is stored (GET sees it) but announces nothing; once a session runs the
// first set is announced (the sentinel, even when unchanged since before the session), an unchanged repeat is not,
// and a change is. This is the only test that begins the session, and it does so after its pre-session checks.
TEST(battery_announces_a_change_in_a_session_but_nothing_before_it) {
    apkrt::battery::reset();

    // Before the session: stored, but apkrt_session_post has no session to write to, so nothing is announced.
    apkrt_battery_set(55, APKRT_BATTERY_STATE_UNPLUGGED);
    CHECK(battery() == static_cast<long>(packed(55, APKRT_BATTERY_STATE_UNPLUGGED)));  // GET sees it

    begin_session();

    // The first set in the session is announced even though the value is unchanged since before it (the last-posted
    // sentinel was never set, because the pre-session post found no session).
    apkrt_battery_set(55, APKRT_BATTERY_STATE_UNPLUGGED);
    apkrt_event event = read_event();
    CHECK(event.type == APKRT_EVENT_BATTERY);
    CHECK(event.arguments[0] == 55);
    CHECK(event.arguments[1] == APKRT_BATTERY_STATE_UNPLUGGED);
    CHECK(event.arguments[2] == 0);

    // An unchanged repeat announces nothing; a change does. Reading the next event and finding the changed value
    // proves the repeat announced none — otherwise this read would return a second, stale 55.
    apkrt_battery_set(55, APKRT_BATTERY_STATE_UNPLUGGED);  // no event
    apkrt_battery_set(80, APKRT_BATTERY_STATE_CHARGING);   // one event
    event = read_event();
    CHECK(event.arguments[0] == 80);
    CHECK(event.arguments[1] == APKRT_BATTERY_STATE_CHARGING);

    // An unknown level is announced as all ones over the full 64-bit field.
    apkrt_battery_set(-1, APKRT_BATTERY_STATE_UNKNOWN);
    event = read_event();
    CHECK(event.arguments[0] == static_cast<std::uint64_t>(-1));
    CHECK(event.arguments[1] == APKRT_BATTERY_STATE_UNKNOWN);
}

int main() { return apk::testing::run_all(); }
