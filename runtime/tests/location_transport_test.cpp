// The location transport (ADR-0063): APKRT_NR_LOCATION through the real dispatcher, with a session and a fake host. It
// checks what the hub's own test does not reach — the grant gate (every data op is -EACCES without a location grant,
// and the gate grants only on outcomes 1 and 2 whatever int the host stored), the guest pointer and scalar checks, and
// that a fix is announced through the session event stream as APKRT_EVENT_LOCATION and then fetched with the GET
// syscall. A process has one session, so the tests share it; each seeds its own grant.

#include "location_hub.h"
#include "location_fudge.h"
#include "permissions.h"
#include "syscall_frame.h"

#include <errno.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

#include "APKRuntime/apkrt.h"
#include "APKRuntime/guest_interface.h"
#include "check.h"

namespace {

constexpr long efault = 14, einval = 22, eacces = 13, eexist = 17, enoent = 2;
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

long location(std::uint64_t op, std::uint64_t argument = 0, std::uint64_t sequence = 0) {
    return call(APKRT_NR_LOCATION, op, argument, sequence);
}

// A fake host: records the one-shot tokens asked, so a test can answer them with apkrt_location_push_single.
struct FakeHost {
    std::mutex mutex;
    std::vector<std::uint64_t> requested;
};
FakeHost host;

int fake_start(void *, std::int32_t, std::int32_t) { return 0; }
void fake_stop(void *) {}
int fake_request(void *context, std::uint64_t token) {
    auto *self = static_cast<FakeHost *>(context);
    const std::lock_guard<std::mutex> hold(self->mutex);
    self->requested.push_back(token);
    return 0;
}
void fake_cancel(void *, std::uint64_t) {}

void on_end(void *, int) {}

int events = -1;  // the event-pipe descriptor from SESSION_CONNECT

// Begins the process's session and connects to the event pipe, once. Also registers the fake location host.
void setup() {
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
    const apkrt_location_host interface{&host, fake_start, fake_stop, fake_request, fake_cancel};
    apkrt_location_set_host(&interface);
}

// Reads one event from the pipe; fails the test if none is there.
apkrt_event read_event() {
    apkrt_event record{};
    const long got = call(nr_read, static_cast<std::uint64_t>(events), address(&record), sizeof record);
    CHECK(got == static_cast<long>(sizeof record));
    return record;
}

void grant(std::int32_t outcome) {
    apkrt::permissions::reset();
    apkrt::permissions::set(APKRT_PERMISSION_LOCATION, outcome);
    {
        const std::lock_guard<std::mutex> hold(host.mutex);
        host.requested.clear();
    }
    apkrt::location::reset();  // but the host must stay registered
    const apkrt_location_host interface{&host, fake_start, fake_stop, fake_request, fake_cancel};
    apkrt_location_set_host(&interface);
}

apkrt_location_fix a_fix(double lat, double lon) {
    apkrt_location_fix fix{};
    fix.latitude = lat;
    fix.longitude = lon;
    fix.horizontal_accuracy_m = 8.0f;
    fix.time_ms = 1'700'000'000'000;
    fix.elapsed_realtime_ns = 42;
    fix.provider = 3;
    fix.valid = APKRT_LOCATION_HAS_HORIZONTAL_ACCURACY;
    return fix;
}

}  // namespace

TEST(without_a_grant_every_data_op_is_eacces_but_stop_is_allowed) {
    setup();
    apkrt::permissions::reset();  // no answer yet
    apkrt_location_control control{APKRT_LOCATION_ACCURACY_FINE, 0};
    apkrt_location_fix fix{};
    CHECK(location(APKRT_LOCATION_START, address(&control)) == -eacces);
    CHECK(location(APKRT_LOCATION_CURRENT, 7) == -eacces);
    CHECK(location(APKRT_LOCATION_LAST, address(&fix)) == -eacces);
    CHECK(location(APKRT_LOCATION_GET, address(&fix), 1) == -eacces);
    CHECK(location(APKRT_LOCATION_STOP) == 0);  // never gated
}

TEST(the_gate_grants_only_on_outcomes_one_and_two) {
    setup();
    apkrt_location_control control{APKRT_LOCATION_ACCURACY_FINE, 0};
    for (const std::int32_t bad : {APKRT_PERMISSION_DENIED, -1, 3, 7, 100}) {
        grant(bad);
        CHECK(location(APKRT_LOCATION_START, address(&control)) == -eacces);
    }
    grant(APKRT_PERMISSION_GRANTED);
    CHECK(location(APKRT_LOCATION_START, address(&control)) == 0);
    grant(APKRT_PERMISSION_GRANTED_COARSE);
    CHECK(location(APKRT_LOCATION_START, address(&control)) == 0);
}

TEST(a_stored_int_from_permission_answer_still_gates) {
    setup();
    apkrt::location::reset();
    const apkrt_location_host interface{&host, fake_start, fake_stop, fake_request, fake_cancel};
    apkrt_location_set_host(&interface);
    apkrt::permissions::reset();
    apkrt_permission_answer(0, APKRT_PERMISSION_LOCATION, 7);  // the host passes a nonsense int; it is stored verbatim
    read_event();                                             // drains the APKRT_EVENT_PERMISSION_RESULT this posted
    apkrt_location_control control{APKRT_LOCATION_ACCURACY_FINE, 0};
    CHECK(location(APKRT_LOCATION_START, address(&control)) == -eacces);  // 7 is not a grant
}

TEST(a_continuous_fix_is_announced_and_fetched_with_get) {
    setup();
    grant(APKRT_PERMISSION_GRANTED);
    apkrt_location_control control{APKRT_LOCATION_ACCURACY_FINE, 0};
    CHECK(location(APKRT_LOCATION_START, address(&control)) == 0);
    const apkrt_location_fix fix = a_fix(52.52, 13.405);
    apkrt_location_push(&fix);
    const apkrt_event event = read_event();
    CHECK(event.type == APKRT_EVENT_LOCATION);
    CHECK(event.arguments[0] != 0);                 // a sequence
    CHECK(event.arguments[1] == 0);                 // continuous, fine: no flags
    CHECK(event.arguments[2] == 0);                 // no token
    apkrt_location_fix got{};
    CHECK(location(APKRT_LOCATION_GET, address(&got), event.arguments[0]) == 1);
    CHECK(got.latitude == 52.52 && got.longitude == 13.405);
    CHECK(location(APKRT_LOCATION_LAST, address(&got)) == 1 && got.latitude == 52.52);
}

TEST(a_current_request_is_answered_as_a_one_shot_event) {
    setup();
    grant(APKRT_PERMISSION_GRANTED);
    CHECK(location(APKRT_LOCATION_CURRENT, 55) == 0);
    {
        const std::lock_guard<std::mutex> hold(host.mutex);
        CHECK(host.requested.size() == 1 && host.requested[0] == 55);
    }
    const apkrt_location_fix fix = a_fix(48.137, 11.575);
    apkrt_location_push_single(55, &fix);
    const apkrt_event event = read_event();
    CHECK(event.type == APKRT_EVENT_LOCATION);
    CHECK((event.arguments[1] & APKRT_LOCATION_EVENT_ONE_SHOT) != 0);
    CHECK((event.arguments[1] & APKRT_LOCATION_EVENT_NO_FIX) == 0);
    CHECK(event.arguments[2] == 55);  // the token
    apkrt_location_fix got{};
    CHECK(location(APKRT_LOCATION_GET, address(&got), event.arguments[0]) == 1);
    CHECK(got.latitude == 48.137);
}

TEST(a_no_fix_one_shot_announces_no_fix) {
    setup();
    grant(APKRT_PERMISSION_GRANTED);
    CHECK(location(APKRT_LOCATION_CURRENT, 56) == 0);
    apkrt_location_push_single(56, nullptr);  // the host found no location
    const apkrt_event event = read_event();
    CHECK(event.type == APKRT_EVENT_LOCATION);
    CHECK((event.arguments[1] & APKRT_LOCATION_EVENT_ONE_SHOT) != 0);
    CHECK((event.arguments[1] & APKRT_LOCATION_EVENT_NO_FIX) != 0);
    CHECK(event.arguments[2] == 56);
}

TEST(a_coarse_grant_marks_and_fudges_the_fix) {
    setup();
    grant(APKRT_PERMISSION_GRANTED_COARSE);
    apkrt_location_control control{APKRT_LOCATION_ACCURACY_FINE, 0};
    CHECK(location(APKRT_LOCATION_START, address(&control)) == 0);
    const apkrt_location_fix fix = a_fix(52.52, 13.405);
    apkrt_location_push(&fix);
    const apkrt_event event = read_event();
    CHECK((event.arguments[1] & APKRT_LOCATION_EVENT_COARSE) != 0);
    apkrt_location_fix got{};
    CHECK(location(APKRT_LOCATION_GET, address(&got), event.arguments[0]) == 1);
    CHECK(got.horizontal_accuracy_m == apkrt::location::coarse_accuracy_m);  // 2000 m, fudged
}

TEST(bad_pointers_scalars_and_ops_are_refused) {
    setup();
    grant(APKRT_PERMISSION_GRANTED);
    const auto page = static_cast<std::size_t>(::getpagesize());
    void *const gone = ::mmap(nullptr, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(gone != MAP_FAILED);
    CHECK(::munmap(gone, page) == 0);
    const auto bad = reinterpret_cast<std::uint64_t>(gone);
    CHECK(location(APKRT_LOCATION_START, bad) == -efault);
    CHECK(location(APKRT_LOCATION_LAST, bad) == -efault);
    CHECK(location(APKRT_LOCATION_GET, bad, 1) == -efault);
    apkrt_location_control bad_accuracy{999, 0};
    CHECK(location(APKRT_LOCATION_START, address(&bad_accuracy)) == -einval);
    CHECK(location(APKRT_LOCATION_CURRENT, 0) == -einval);  // token 0
    CHECK(location(999) == -einval);                        // not an op
}

TEST(a_current_token_is_unique_and_cancel_checks_it) {
    setup();
    grant(APKRT_PERMISSION_GRANTED);
    CHECK(location(APKRT_LOCATION_CURRENT, 70) == 0);
    CHECK(location(APKRT_LOCATION_CURRENT, 70) == -eexist);
    CHECK(location(APKRT_LOCATION_CANCEL, 0) == -einval);
    CHECK(location(APKRT_LOCATION_CANCEL, 999) == -enoent);
    CHECK(location(APKRT_LOCATION_CANCEL, 70) == 0);
}

int main() { return apk::testing::run_all(); }
