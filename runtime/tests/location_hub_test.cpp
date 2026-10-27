// The location hub (ADR-0063): the latest-continuous slot with its sequence, the bounded one-shot table that a
// continuous fix never evicts, the one-shot lifetime (kept until GET or CANCEL, a no-fix dropped at once), the host
// start/stop/request/cancel calls, and the coarse fudge applied at delivery. Driven directly with a fake host, no
// session and no syscalls (the transport test covers those). The fudge is asserted against ranges, as the fudger's
// own test is, not against exact values.

#include "location_hub.h"
#include "location_fudge.h"
#include "permissions.h"

#include "APKRuntime/apkrt.h"
#include "APKRuntime/guest_interface.h"
#include "check.h"

#include <cmath>
#include <cstdint>
#include <mutex>
#include <vector>

namespace {

constexpr long eexist = 17, ebusy = 16, enoent = 2;
constexpr double cell_lat = apkrt::location::fudge_grid_meters / apkrt::location::meters_per_degree_latitude;

// A fake CoreLocation host: records the start arguments, the stop count, and the one-shot tokens asked and cancelled.
struct FakeHost {
    std::mutex mutex;
    int start_calls = 0;
    std::int32_t last_accuracy = 0;
    std::int32_t last_distance = 0;
    int stop_calls = 0;
    std::vector<std::uint64_t> requested;
    std::vector<std::uint64_t> cancelled;
};
FakeHost host;

int fake_start(void *context, std::int32_t accuracy, std::int32_t distance_m) {
    auto *self = static_cast<FakeHost *>(context);
    const std::lock_guard<std::mutex> hold(self->mutex);
    ++self->start_calls;
    self->last_accuracy = accuracy;
    self->last_distance = distance_m;
    return 0;
}
void fake_stop(void *context) {
    auto *self = static_cast<FakeHost *>(context);
    const std::lock_guard<std::mutex> hold(self->mutex);
    ++self->stop_calls;
}
int fake_request(void *context, std::uint64_t token) {
    auto *self = static_cast<FakeHost *>(context);
    const std::lock_guard<std::mutex> hold(self->mutex);
    self->requested.push_back(token);
    return 0;
}
void fake_cancel(void *context, std::uint64_t token) {
    auto *self = static_cast<FakeHost *>(context);
    const std::lock_guard<std::mutex> hold(self->mutex);
    self->cancelled.push_back(token);
}

// Resets the hub, the grant and the fake host, registers the host, and seeds the grant (fine unless a test overrides).
void install(std::int32_t grant = APKRT_PERMISSION_GRANTED) {
    apkrt::location::reset();
    apkrt::permissions::reset();
    apkrt::permissions::set(APKRT_PERMISSION_LOCATION, grant);
    {
        const std::lock_guard<std::mutex> hold(host.mutex);
        host.start_calls = 0;
        host.last_accuracy = 0;
        host.last_distance = 0;
        host.stop_calls = 0;
        host.requested.clear();
        host.cancelled.clear();
    }
    const apkrt_location_host interface{&host, fake_start, fake_stop, fake_request, fake_cancel};
    apkrt_location_set_host(&interface);
}

apkrt_location_fix a_fix(double lat, double lon) {
    apkrt_location_fix fix{};
    fix.latitude = lat;
    fix.longitude = lon;
    fix.altitude = 34.0;
    fix.horizontal_accuracy_m = 5.0f;
    fix.vertical_accuracy_m = 3.0f;
    fix.speed_mps = 1.5f;
    fix.speed_accuracy_mps = 0.5f;
    fix.bearing_deg = 90.0f;
    fix.bearing_accuracy_deg = 10.0f;
    fix.time_ms = 1'700'000'000'000;
    fix.elapsed_realtime_ns = 123'456'789;
    fix.provider = 3;
    fix.valid = APKRT_LOCATION_HAS_ALTITUDE | APKRT_LOCATION_HAS_HORIZONTAL_ACCURACY |
                APKRT_LOCATION_HAS_VERTICAL_ACCURACY | APKRT_LOCATION_HAS_SPEED | APKRT_LOCATION_HAS_SPEED_ACCURACY |
                APKRT_LOCATION_HAS_BEARING | APKRT_LOCATION_HAS_BEARING_ACCURACY;
    return fix;
}

bool near(double a, double b, double eps) { return std::fabs(a - b) < eps; }

}  // namespace

TEST(a_continuous_fix_is_stored_and_read_by_last_and_get) {
    install();
    apkrt_location_fix out{};
    CHECK(apkrt::location::last(out) == 0);  // none yet
    const std::uint64_t seq = apkrt::location::push_continuous(a_fix(52.52, 13.405));
    CHECK(seq != 0);
    CHECK(apkrt::location::last(out) == 1);
    CHECK(out.latitude == 52.52 && out.longitude == 13.405);
    apkrt_location_fix got{};
    CHECK(apkrt::location::get(seq, got) == 1);
    CHECK(got.latitude == 52.52 && got.longitude == 13.405);
}

TEST(a_newer_continuous_fix_replaces_the_older_sequence) {
    install();
    const std::uint64_t first = apkrt::location::push_continuous(a_fix(52.52, 13.405));
    const std::uint64_t second = apkrt::location::push_continuous(a_fix(48.137, 11.575));
    CHECK(first != second);
    apkrt_location_fix out{};
    CHECK(apkrt::location::get(first, out) == 0);   // replaced
    CHECK(apkrt::location::get(second, out) == 1);
    CHECK(out.latitude == 48.137);
    CHECK(apkrt::location::last(out) == 1 && out.latitude == 48.137);  // LAST is the newest
}

TEST(a_fix_with_negative_horizontal_accuracy_is_dropped) {
    install();
    apkrt_location_fix bad = a_fix(52.52, 13.405);
    bad.horizontal_accuracy_m = -1.0f;
    CHECK(apkrt::location::push_continuous(bad) == 0);
    apkrt_location_fix out{};
    CHECK(apkrt::location::last(out) == 0);  // nothing stored
}

TEST(a_continuous_push_without_a_start_still_stores_for_last_and_get) {
    // getLastKnownLocation must work off a fix even before an explicit START; the framework may read it first.
    install();
    const std::uint64_t seq = apkrt::location::push_continuous(a_fix(52.52, 13.405));
    CHECK(seq != 0);
    apkrt_location_fix out{};
    CHECK(apkrt::location::get(seq, out) == 1);
}

TEST(start_and_stop_reach_the_host_with_the_requested_accuracy) {
    install();
    CHECK(apkrt::location::start(APKRT_LOCATION_ACCURACY_FINE, 10) == 0);
    {
        const std::lock_guard<std::mutex> hold(host.mutex);
        CHECK(host.start_calls == 1 && host.last_accuracy == APKRT_LOCATION_ACCURACY_FINE && host.last_distance == 10);
    }
    CHECK(apkrt::location::stop() == 0);
    {
        const std::lock_guard<std::mutex> hold(host.mutex);
        CHECK(host.stop_calls == 1);
    }
}

TEST(a_coarse_grant_makes_start_ask_the_host_for_coarse) {
    install(APKRT_PERMISSION_GRANTED_COARSE);
    CHECK(apkrt::location::start(APKRT_LOCATION_ACCURACY_FINE, 0) == 0);  // the guest asked fine
    const std::lock_guard<std::mutex> hold(host.mutex);
    CHECK(host.last_accuracy == APKRT_LOCATION_ACCURACY_COARSE);  // but the host is asked coarse
}

TEST(a_one_shot_request_reaches_the_host) {
    install();
    CHECK(apkrt::location::request_single(7) == 0);
    const std::lock_guard<std::mutex> hold(host.mutex);
    CHECK(host.requested.size() == 1 && host.requested[0] == 7);
}

TEST(a_one_shot_fix_is_kept_until_its_get_then_the_token_frees) {
    install();
    CHECK(apkrt::location::request_single(7) == 0);
    const apkrt_location_fix fix = a_fix(52.52, 13.405);
    const std::uint64_t seq = apkrt::location::push_single(7, &fix);
    CHECK(seq != 0);
    apkrt_location_fix out{};
    CHECK(apkrt::location::get(seq, out) == 1);
    CHECK(out.latitude == 52.52);
    CHECK(apkrt::location::get(seq, out) == 0);          // dropped after the GET
    CHECK(apkrt::location::request_single(7) == 0);      // the token is free again
}

TEST(a_one_shot_survives_continuous_traffic) {
    install();
    CHECK(apkrt::location::request_single(7) == 0);
    const apkrt_location_fix single = a_fix(52.52, 13.405);
    const std::uint64_t one_shot_seq = apkrt::location::push_single(7, &single);
    CHECK(one_shot_seq != 0);
    for (int i = 0; i < 5; ++i) apkrt::location::push_continuous(a_fix(10.0 + i, 20.0 + i));  // a flood of continuous
    apkrt_location_fix out{};
    CHECK(apkrt::location::get(one_shot_seq, out) == 1);  // not evicted by the continuous fixes
    CHECK(out.latitude == 52.52);
}

TEST(a_no_fix_one_shot_drops_the_entry_at_once) {
    install();
    CHECK(apkrt::location::request_single(7) == 0);
    CHECK(apkrt::location::push_single(7, nullptr) == 0);  // no location
    CHECK(apkrt::location::cancel_single(7) == -enoent);   // already gone
    CHECK(apkrt::location::request_single(7) == 0);        // the token is free again
}

TEST(an_outstanding_token_is_unique_and_the_table_is_bounded) {
    install();
    CHECK(apkrt::location::request_single(1) == 0);
    CHECK(apkrt::location::request_single(1) == -eexist);  // outstanding already
    for (std::uint64_t t = 2; t <= APKRT_LOCATION_ONESHOT_MAX; ++t) CHECK(apkrt::location::request_single(t) == 0);
    CHECK(apkrt::location::request_single(APKRT_LOCATION_ONESHOT_MAX + 1) == -ebusy);  // full
    CHECK(apkrt::location::cancel_single(1) == 0);                                     // free one
    CHECK(apkrt::location::request_single(APKRT_LOCATION_ONESHOT_MAX + 1) == 0);       // now there is room
}

TEST(a_cancel_tells_the_host_only_for_a_known_token) {
    install();
    CHECK(apkrt::location::cancel_single(99) == -enoent);
    CHECK(apkrt::location::request_single(7) == 0);
    CHECK(apkrt::location::cancel_single(7) == 0);
    const std::lock_guard<std::mutex> hold(host.mutex);
    CHECK(host.cancelled.size() == 1 && host.cancelled[0] == 7);
}

TEST(a_coarse_grant_fudges_the_delivered_fix) {
    install(APKRT_PERMISSION_GRANTED_COARSE);
    const std::uint64_t seq = apkrt::location::push_continuous(a_fix(52.52, 13.405));
    apkrt_location_fix out{};
    CHECK(apkrt::location::get(seq, out) == 1);
    const double k = out.latitude / cell_lat;
    CHECK(near(k, std::round(k), 1e-6));                                      // snapped to the grid
    CHECK(near(out.latitude, 52.52, 3000.0 / apkrt::location::meters_per_degree_latitude));  // near the truth
    CHECK(out.horizontal_accuracy_m == apkrt::location::coarse_accuracy_m);   // 2000 m
    CHECK(out.valid == APKRT_LOCATION_HAS_HORIZONTAL_ACCURACY);               // altitude/speed/bearing stripped
    CHECK(out.time_ms == 1'700'000'000'000);                                  // time and provider kept
    CHECK(out.provider == 3);
}

TEST(a_resolved_one_shot_still_occupies_its_slot) {
    install();
    for (std::uint64_t t = 1; t <= APKRT_LOCATION_ONESHOT_MAX; ++t) CHECK(apkrt::location::request_single(t) == 0);
    const apkrt_location_fix fix = a_fix(52.52, 13.405);
    const std::uint64_t seq = apkrt::location::push_single(1, &fix);  // token 1 resolved with a fix, not fetched yet
    CHECK(seq != 0);
    // The held fix still fills a slot.
    CHECK(apkrt::location::request_single(APKRT_LOCATION_ONESHOT_MAX + 1) == -ebusy);
    apkrt_location_fix out{};
    CHECK(apkrt::location::get(seq, out) == 1);                                        // fetching it frees the slot
    CHECK(apkrt::location::request_single(APKRT_LOCATION_ONESHOT_MAX + 1) == 0);
}

TEST(a_grant_revoked_after_a_push_delivers_no_fix) {
    install(APKRT_PERMISSION_GRANTED);
    const std::uint64_t seq = apkrt::location::push_continuous(a_fix(52.52, 13.405));
    apkrt_location_fix out{};
    CHECK(apkrt::location::get(seq, out) == 1);  // granted: the fix comes through
    apkrt::permissions::set(APKRT_PERMISSION_LOCATION, APKRT_PERMISSION_DENIED);  // the host revokes mid-flight
    CHECK(apkrt::location::get(seq, out) == 0);   // fail closed: no fix at all, not even a coarse one
    CHECK(apkrt::location::last(out) == 0);
}

TEST(a_coarse_grant_fudges_a_one_shot_fix) {
    install(APKRT_PERMISSION_GRANTED_COARSE);
    CHECK(apkrt::location::request_single(7) == 0);
    const apkrt_location_fix fix = a_fix(52.52, 13.405);
    const std::uint64_t seq = apkrt::location::push_single(7, &fix);
    apkrt_location_fix out{};
    CHECK(apkrt::location::get(seq, out) == 1);
    CHECK(out.horizontal_accuracy_m == apkrt::location::coarse_accuracy_m);  // the one-shot path fudges too
    CHECK(out.valid == APKRT_LOCATION_HAS_HORIZONTAL_ACCURACY);
}

TEST(a_fine_grant_delivers_the_raw_fix) {
    install(APKRT_PERMISSION_GRANTED);
    const std::uint64_t seq = apkrt::location::push_continuous(a_fix(52.52, 13.405));
    apkrt_location_fix out{};
    CHECK(apkrt::location::get(seq, out) == 1);
    CHECK(out.latitude == 52.52 && out.longitude == 13.405);
    CHECK(out.horizontal_accuracy_m == 5.0f);                                 // unchanged
    CHECK((out.valid & APKRT_LOCATION_HAS_ALTITUDE) != 0);                    // nothing stripped
    CHECK((out.valid & APKRT_LOCATION_HAS_BEARING) != 0);
}

int main() { return apk::testing::run_all(); }
