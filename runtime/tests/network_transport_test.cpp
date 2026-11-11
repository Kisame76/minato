// The network transport (ADR-0054 step d)): APKRT_NR_NETWORK (GET) through the real dispatcher, and apkrt_network_set
// announcing APKRT_EVENT_NETWORK through the session event stream. GET reads the current state and is -ENODATA before
// the first set; a set is announced only on a change and only while a session runs, so a state set before the session
// is stored (GET sees it) but not announced. A process has one session, so only the lifecycle test begins it.

#include "network.h"
#include "syscall_frame.h"

#include <cstdint>

#include "APKRuntime/apkrt.h"
#include "APKRuntime/guest_interface.h"
#include "netlink.h"
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

long network(std::uint64_t op = APKRT_NETWORK_GET) { return call(APKRT_NR_NETWORK, op); }

constexpr std::uint32_t wifi = APKRT_NETWORK_SATISFIED | APKRT_NETWORK_WIFI;
constexpr std::uint32_t cellular = APKRT_NETWORK_SATISFIED | APKRT_NETWORK_CELLULAR | APKRT_NETWORK_EXPENSIVE;

int events = -1;  // the event-pipe descriptor from SESSION_CONNECT

void on_end(void *, int) {}

// Begins the process's session and connects the event pipe, once, after the lifecycle test's pre-session checks.
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

TEST(network_get_is_enodata_before_a_set) {
    apkrt::network::reset();
    CHECK(network() == -enodata);
}

TEST(network_get_answers_the_state_last_set) {
    apkrt::network::reset();
    apkrt_network_set(wifi);
    CHECK(network() == static_cast<long>(wifi));
    apkrt_network_set(0);  // offline: a state too, not ENODATA
    CHECK(network() == 0);
}

TEST(a_bad_network_op_is_einval) {
    CHECK(network(2) == -einval);    // op 0 is GET, op 1 is APKRT_NETWORK_LINK; 2 and up are unused
    CHECK(network(999) == -einval);
}

TEST(link_before_a_set_is_enodata) {
    apkrt::network::reset();
    // APKRT_NETWORK_LINK is -ENODATA when no network has been set, before it touches the guest buffer, as GET is.
    CHECK(network(APKRT_NETWORK_LINK) == -enodata);
}

// A state set before the session is stored but announces nothing; in the session the first set is announced (even
// unchanged since before it), an unchanged repeat is not, and a change is.
TEST(network_announces_a_change_in_a_session_but_nothing_before_it) {
    apkrt::network::reset();
    apkrt_network_set(wifi);
    CHECK(network() == static_cast<long>(wifi));

    begin_session();

    apkrt_network_set(wifi);
    apkrt_event event = read_event();
    CHECK(event.type == APKRT_EVENT_NETWORK);
    CHECK(event.arguments[0] == wifi);
    CHECK(event.arguments[1] == 0);
    CHECK(event.arguments[2] == 0);

    // Reading the changed state next proves the repeat announced none.
    apkrt_network_set(wifi);
    apkrt_network_set(cellular);
    event = read_event();
    CHECK(event.type == APKRT_EVENT_NETWORK);
    CHECK(event.arguments[0] == cellular);

    apkrt_network_set(0);
    event = read_event();
    CHECK(event.arguments[0] == 0);
}

TEST(a_network_with_no_readable_interface_is_a_snapshot_not_enodata) {
    // -ENODATA means "no active network" (getActiveNetwork null -> getLinkProperties null). A network that is set but
    // whose interface cannot be read must still give a (possibly empty) snapshot, so getLinkProperties stays non-null,
    // as on a phone. Here a network is set but the host source has no interface (the fake path monitor, emptied).
    apkrt::network::reset();
    apkrt::netlink::set_fake_host({}, {});
    apkrt_network_set(wifi);
    CHECK(network(APKRT_NETWORK_LINK) > 0);   // a snapshot, not -ENODATA
    apkrt::network::reset();
    CHECK(network(APKRT_NETWORK_LINK) == -enodata);  // truly no network: -ENODATA (getLinkProperties null)
    apkrt::netlink::clear_fake_host();
}

int main() { return apk::testing::run_all(); }
