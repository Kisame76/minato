// The sensor hub (ADR-0061 step 1), driven with a fake host: no CoreMotion and no guest syscalls. The hub's own C++ face
// (sensor_hub.h) opens queues and configures them, apkrt_sensor_set_host/apkrt_sensor_push (apkrt.h) stand in for the
// host, and the test reads whole ASensorEvent records straight from a queue's pipe read end. This covers the
// protocol, the fan-out, the rate merge, the timestamp shift and the concurrency; the NDK and the Java side read the
// same records over the transport, tested elsewhere.

#include "sensor_hub.h"

#include <errno.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <thread>
#include <vector>

#include "APKRuntime/apkrt.h"
#include "check.h"

using namespace std::chrono_literals;

namespace {

// A fake CoreMotion host. It records every start (the type and the period it was asked for, a re-rate included) and
// every stop in order, tracks which types have updates on, and can make its stop wait for a push to run — so a bug
// that held a hub lock across the callback would hang rather than return.
struct FakeHost {
    std::mutex mutex;
    std::vector<std::pair<std::int32_t, std::int64_t>> starts;  // (type, period_ns), re-rates included
    std::vector<std::int32_t> stops;                            // types stopped, in order
    std::set<std::int32_t> on;                                  // types with updates on now
    std::map<std::int32_t, std::int64_t> period;                // last period asked per started type

    std::atomic<bool> block_stop{false};
    std::atomic<bool> push_ran{false};
    std::atomic<int> fail_next_start{0};  // make this many start calls return -1 (the host could not start)
};

FakeHost host;

int fake_start(void *context, std::int32_t type, std::int64_t period_ns) {
    auto *self = static_cast<FakeHost *>(context);
    const std::lock_guard<std::mutex> hold(self->mutex);
    self->starts.push_back({type, period_ns});  // every call, a rejected one included
    if (self->fail_next_start > 0) {
        --self->fail_next_start;
        return -1;  // the host could not start this one: it is not "on"
    }
    self->on.insert(type);
    self->period[type] = period_ns;
    return 0;
}

void fake_stop(void *context, std::int32_t type) {
    auto *self = static_cast<FakeHost *>(context);
    if (self->block_stop.load()) {
        for (int i = 0; i < 2000 && !self->push_ran.load(); ++i) std::this_thread::sleep_for(1ms);
    }
    const std::lock_guard<std::mutex> hold(self->mutex);
    self->stops.push_back(type);
    self->on.erase(type);
    self->period.erase(type);
}

// Installs the fake host with `available` as the sensors CoreMotion can feed, and clears its record.
void install(std::initializer_list<std::int32_t> available) {
    apkrt::sensors::reset();
    {
        const std::lock_guard<std::mutex> hold(host.mutex);
        host.starts.clear();
        host.stops.clear();
        host.on.clear();
        host.period.clear();
    }
    host.block_stop.store(false);
    host.push_ran.store(false);
    host.fail_next_start.store(0);
    const std::vector<std::int32_t> list(available);
    const apkrt_sensor_host interface{&host, fake_start, fake_stop};
    apkrt_sensor_set_host(&interface, list.data(), list.size());
}

bool started(std::int32_t type) {
    const std::lock_guard<std::mutex> hold(host.mutex);
    return host.on.count(type) != 0;
}

int starts_of(std::int32_t type) {
    const std::lock_guard<std::mutex> hold(host.mutex);
    return static_cast<int>(std::count_if(host.starts.begin(), host.starts.end(),
                                          [&](auto &s) { return s.first == type; }));
}

int stops_of(std::int32_t type) {
    const std::lock_guard<std::mutex> hold(host.mutex);
    return static_cast<int>(std::count(host.stops.begin(), host.stops.end(), type));
}

std::int64_t period_of(std::int32_t type) {
    const std::lock_guard<std::mutex> hold(host.mutex);
    const auto it = host.period.find(type);
    return it == host.period.end() ? -1 : it->second;
}

// One whole event record from a queue's read end, or nothing when the queue is empty (EAGAIN) or a read is short.
std::optional<apkrt::sensors::Event> read_event(int fd) {
    apkrt::sensors::Event event{};
    const ssize_t got = ::read(fd, &event, sizeof event);
    if (got == static_cast<ssize_t>(sizeof event)) return event;
    return std::nullopt;
}

// Darwin's CLOCK_MONOTONIC is the guest's CLOCK_BOOTTIME (runs through sleep); CLOCK_UPTIME_RAW is the guest's
// CLOCK_MONOTONIC and CoreMotion's base (stops in sleep). The hub shifts a host time onto the boottime base by this
// offset, steady for a test's length.
std::int64_t boot_ns() { return static_cast<std::int64_t>(clock_gettime_nsec_np(CLOCK_MONOTONIC)); }
std::int64_t uptime_ns() { return static_cast<std::int64_t>(clock_gettime_nsec_np(CLOCK_UPTIME_RAW)); }

constexpr std::int32_t ACCEL = 1;       // ASENSOR_TYPE_ACCELEROMETER, 3 values
constexpr std::int32_t MAG = 2;         // ASENSOR_TYPE_MAGNETIC_FIELD, 3 values
constexpr std::int32_t GYRO = 4;        // ASENSOR_TYPE_GYROSCOPE, 3 values
constexpr std::int32_t ROTATION = 11;   // ASENSOR_TYPE_ROTATION_VECTOR, 5 values
constexpr std::int32_t MAG_UNCAL = 14;  // ASENSOR_TYPE_MAGNETIC_FIELD_UNCALIBRATED, 6 values
constexpr std::int32_t HIGH = apkrt::sensors::status_accuracy_high;               // 3
constexpr std::int32_t META_DATA_TYPE = apkrt::sensors::meta_type;                // 0
constexpr std::int32_t META_FLUSH_COMPLETE = apkrt::sensors::meta_flush_complete;  // 1

// The guest's request in ns and its clamp.
constexpr std::int64_t min_period_ns = apkrt::sensors::sensor_min_delay_us * 1000LL;  // 1e7 (100 Hz)
constexpr std::int64_t max_period_ns = apkrt::sensors::sensor_max_delay_us * 1000LL;  // 1e9 (1 Hz)

// Closes a queue (its hub write end) and the read end the test holds.
void shut(const apkrt::sensors::OpenQueue &q) {
    apkrt::sensors::close_queue(q.id);
    ::close(q.read_fd);
}

}  // namespace

TEST(without_a_host_the_hub_offers_no_sensor) {
    apkrt::sensors::reset();
    CHECK(apkrt::sensors::sensor_list(nullptr, 0) == 0);
    CHECK(!apkrt::sensors::sensor(ACCEL).has_value());
    const auto q = apkrt::sensors::open_queue();
    CHECK(q.has_value());
    if (q) {
        CHECK(apkrt::sensors::enable(q->id, ACCEL, min_period_ns) == -EINVAL);  // nothing available
        shut(*q);
    }
    const apkrt_sensor_host interface{&host, fake_start, fake_stop};
    apkrt_sensor_set_host(&interface, nullptr, 0);  // a host with no available sensors is the same as none
    CHECK(apkrt::sensors::sensor_list(nullptr, 0) == 0);
}

TEST(the_sensor_list_is_the_table_intersected_with_the_host) {
    install({ACCEL, GYRO, ROTATION, MAG_UNCAL});  // MAG (2) is offered by the table but not made available here
    apkrt::sensors::SensorInfo list[16];
    const std::size_t count = apkrt::sensors::sensor_list(list, 16);
    CHECK(count == 4);
    if (count == 4) {
        const std::int32_t expect_type[] = {ACCEL, GYRO, ROTATION, MAG_UNCAL};  // the table's order
        const std::int32_t expect_values[] = {3, 3, 5, 6};
        for (int i = 0; i < 4; ++i) {
            CHECK(list[i].type == expect_type[i]);
            CHECK(list[i].value_count == expect_values[i]);
            CHECK(list[i].type == expect_type[i] && list[i].name != nullptr && list[i].vendor != nullptr &&
                  list[i].string_type != nullptr && list[i].required_permission != nullptr);
            CHECK(list[i].min_delay_us == apkrt::sensors::sensor_min_delay_us);
            CHECK(list[i].max_delay_us == apkrt::sensors::sensor_max_delay_us);
            CHECK(list[i].reporting_mode == apkrt::sensors::reporting_continuous && list[i].flags == 0 &&
                  list[i].version == 1);
        }
    }
    const auto accel = apkrt::sensors::sensor(ACCEL);
    CHECK(accel.has_value() && accel->type == ACCEL && accel->value_count == 3);
    CHECK(!apkrt::sensors::sensor(MAG).has_value());    // offered but not available
    CHECK(!apkrt::sensors::sensor(0x999).has_value());  // not in the table
}

TEST(enabling_a_sensor_starts_the_host_at_the_clamped_rate) {
    install({ACCEL});
    const auto q = apkrt::sensors::open_queue();
    CHECK(q.has_value());
    if (!q) return;
    CHECK(apkrt::sensors::enable(q->id, ACCEL, 5000) == 0);  // 5000 ns is below the minimum
    CHECK(started(ACCEL) && starts_of(ACCEL) == 1 && period_of(ACCEL) == min_period_ns);  // clamped up
    CHECK(apkrt::sensors::enable(q->id, MAG, min_period_ns) == -EINVAL);  // MAG not available
    CHECK(starts_of(MAG) == 0);
    CHECK(apkrt::sensors::set_rate(q->id, ACCEL, 5LL * 1000 * 1000 * 1000) == 0);  // 5 s is above the maximum
    CHECK(period_of(ACCEL) == max_period_ns);                                      // clamped down
    shut(*q);
    CHECK(!started(ACCEL) && stops_of(ACCEL) == 1);  // the last (only) queue gone stops it
}

TEST(a_sample_arrives_as_a_whole_event_with_the_values_accuracy_and_a_shifted_timestamp) {
    install({ACCEL});
    const auto q = apkrt::sensors::open_queue();
    CHECK(q.has_value());
    if (!q) return;
    CHECK(apkrt::sensors::enable(q->id, ACCEL, min_period_ns) == 0);

    const float values[] = {1.5f, -2.5f, 9.80665f};
    const double host_time = static_cast<double>(uptime_ns()) / 1e9;
    const std::int64_t boot_before = boot_ns();
    apkrt_sensor_push(ACCEL, values, 3, host_time, HIGH);
    const std::int64_t boot_after = boot_ns();

    const auto event = read_event(q->read_fd);
    CHECK(event.has_value());
    if (event) {
        CHECK(event->version == static_cast<std::int32_t>(sizeof(apkrt::sensors::Event)));
        CHECK(event->sensor == ACCEL && event->type == ACCEL);
        CHECK(event->vector.x == 1.5f && event->vector.y == -2.5f && event->vector.z == 9.80665f);
        CHECK(event->vector.status == HIGH);  // the status byte carries the accuracy for a 3-axis sensor
        // The timestamp is the host time shifted onto the guest's CLOCK_BOOTTIME base: between the two boot readings
        // around the push (a few ms of slack for scheduling).
        CHECK(event->timestamp >= boot_before - 5'000'000 && event->timestamp <= boot_after + 5'000'000);
    }
    // Two samples half a second apart differ by exactly that in the guest clock, whatever the offset.
    apkrt_sensor_push(ACCEL, values, 3, 100.0, HIGH);
    apkrt_sensor_push(ACCEL, values, 3, 100.5, HIGH);
    const auto first = read_event(q->read_fd);
    const auto second = read_event(q->read_fd);
    CHECK(first.has_value() && second.has_value());
    if (first && second) {
        // 0.5 s apart in host time is 0.5 s apart in the guest clock, within the jitter of reading the two clocks that
        // make the offset (not an exact equality: the offset is sampled afresh per push).
        const std::int64_t gap = second->timestamp - first->timestamp;
        CHECK(gap > 499'000'000 && gap < 501'000'000);
    }
    shut(*q);
}

TEST(samples_outside_the_contract_are_dropped) {
    install({ACCEL});
    const auto q = apkrt::sensors::open_queue();
    CHECK(q.has_value());
    if (!q) return;
    const float values[] = {1, 2, 3, 4, 5, 6};
    apkrt_sensor_push(ACCEL, values, 3, 1.0, HIGH);  // not started yet
    CHECK(!read_event(q->read_fd).has_value());
    CHECK(apkrt::sensors::enable(q->id, ACCEL, min_period_ns) == 0);
    apkrt_sensor_push(ACCEL, values, 2, 1.0, HIGH);  // wrong count for ACCEL (3)
    CHECK(!read_event(q->read_fd).has_value());
    apkrt_sensor_push(ACCEL, values, 4, 1.0, HIGH);  // wrong count the other way
    CHECK(!read_event(q->read_fd).has_value());
    apkrt_sensor_push(MAG, values, 3, 1.0, HIGH);  // MAG not available
    CHECK(!read_event(q->read_fd).has_value());
    apkrt::sensors::disable(q->id, ACCEL);
    apkrt_sensor_push(ACCEL, values, 3, 1.0, HIGH);  // no longer started
    CHECK(!read_event(q->read_fd).has_value());
    shut(*q);
}

TEST(nan_and_infinities_pass_through) {
    install({ACCEL});
    const auto q = apkrt::sensors::open_queue();
    CHECK(q.has_value());
    if (!q) return;
    CHECK(apkrt::sensors::enable(q->id, ACCEL, min_period_ns) == 0);
    const float values[] = {std::nanf(""), HUGE_VALF, -HUGE_VALF};
    apkrt_sensor_push(ACCEL, values, 3, 1.0, HIGH);
    const auto event = read_event(q->read_fd);
    CHECK(event.has_value());
    if (event) {
        CHECK(std::isnan(event->vector.x));
        CHECK(std::isinf(event->vector.y) && event->vector.y > 0);
        CHECK(std::isinf(event->vector.z) && event->vector.z < 0);
    }
    shut(*q);
}

TEST(every_enabled_queue_gets_every_sample) {
    install({ACCEL});
    const auto a = apkrt::sensors::open_queue();
    const auto b = apkrt::sensors::open_queue();
    const auto c = apkrt::sensors::open_queue();  // never enables, so it hears nothing
    CHECK(a && b && c);
    if (!a || !b || !c) return;
    CHECK(apkrt::sensors::enable(a->id, ACCEL, min_period_ns) == 0);
    CHECK(apkrt::sensors::enable(b->id, ACCEL, min_period_ns) == 0);
    const float values[] = {7, 8, 9};
    apkrt_sensor_push(ACCEL, values, 3, 1.0, HIGH);
    const auto ea = read_event(a->read_fd);
    const auto eb = read_event(b->read_fd);
    CHECK(ea.has_value() && eb.has_value());
    if (ea && eb) CHECK(ea->vector.x == 7 && eb->vector.x == 7 && ea->timestamp == eb->timestamp);
    CHECK(!read_event(c->read_fd).has_value());
    shut(*a);
    shut(*b);
    shut(*c);
}

TEST(the_rate_is_the_fastest_any_queue_asks_and_the_last_disable_stops) {
    install({ACCEL});
    const auto a = apkrt::sensors::open_queue();
    const auto b = apkrt::sensors::open_queue();
    CHECK(a && b);
    if (!a || !b) return;
    CHECK(apkrt::sensors::enable(a->id, ACCEL, max_period_ns) == 0);  // slow
    CHECK(period_of(ACCEL) == max_period_ns && started(ACCEL));
    CHECK(apkrt::sensors::enable(b->id, ACCEL, min_period_ns) == 0);  // fast: re-rates to the fastest
    CHECK(period_of(ACCEL) == min_period_ns && stops_of(ACCEL) == 0);
    CHECK(apkrt::sensors::disable(b->id, ACCEL) == 0);  // back to the slow one, still no stop
    CHECK(period_of(ACCEL) == max_period_ns && started(ACCEL) && stops_of(ACCEL) == 0);
    CHECK(apkrt::sensors::disable(a->id, ACCEL) == 0);  // the last one gone
    CHECK(!started(ACCEL) && stops_of(ACCEL) == 1);
    shut(*a);
    shut(*b);
}

TEST(flush_writes_a_complete_event_per_enabled_sensor_in_order) {
    install({ACCEL, GYRO});
    const auto q = apkrt::sensors::open_queue();
    const auto empty = apkrt::sensors::open_queue();
    CHECK(q && empty);
    if (!q || !empty) return;
    CHECK(apkrt::sensors::enable(q->id, ACCEL, min_period_ns) == 0);
    CHECK(apkrt::sensors::enable(q->id, GYRO, min_period_ns) == 0);
    CHECK(apkrt::sensors::flush(q->id) == 0);
    const auto first = read_event(q->read_fd);
    const auto second = read_event(q->read_fd);
    CHECK(first.has_value() && second.has_value());
    if (first && second) {
        CHECK(first->type == META_DATA_TYPE && first->meta_data.what == META_FLUSH_COMPLETE && first->meta_data.sensor == ACCEL);
        CHECK(second->type == META_DATA_TYPE && second->meta_data.what == META_FLUSH_COMPLETE && second->meta_data.sensor == GYRO);
    }
    CHECK(!read_event(q->read_fd).has_value());  // only the two completes
    CHECK(apkrt::sensors::flush(empty->id) == 0);  // nothing enabled: nothing written
    CHECK(!read_event(empty->read_fd).has_value());
    shut(*q);
    shut(*empty);
}

TEST(closing_a_queue_stops_its_sensors_only_when_it_was_the_last) {
    install({ACCEL});
    const auto a = apkrt::sensors::open_queue();
    const auto b = apkrt::sensors::open_queue();
    CHECK(a && b);
    if (!a || !b) return;
    CHECK(apkrt::sensors::enable(a->id, ACCEL, min_period_ns) == 0);
    CHECK(apkrt::sensors::enable(b->id, ACCEL, min_period_ns) == 0);
    shut(*a);  // b still wants ACCEL
    CHECK(started(ACCEL) && stops_of(ACCEL) == 0);
    shut(*b);  // the last one
    CHECK(!started(ACCEL) && stops_of(ACCEL) == 1);
}

TEST(a_push_racing_a_stop_does_not_deadlock_and_is_dropped) {
    install({ACCEL});
    const auto q = apkrt::sensors::open_queue();
    CHECK(q.has_value());
    if (!q) return;
    CHECK(apkrt::sensors::enable(q->id, ACCEL, min_period_ns) == 0);
    host.block_stop.store(true);  // the host's stop waits for a push to run

    const auto start = std::chrono::steady_clock::now();
    std::thread disabler([&] { apkrt::sensors::disable(q->id, ACCEL); });  // marks disabled, then stop() outside the lock
    std::thread pusher([&] {
        std::this_thread::sleep_for(20ms);  // let the disabler reach the blocking stop
        const float values[] = {1, 2, 3};
        apkrt_sensor_push(ACCEL, values, 3, 1.0, HIGH);  // needs the hub lock; must not be blocked by the stop
        host.push_ran.store(true);
    });
    disabler.join();
    pusher.join();
    const auto elapsed = std::chrono::steady_clock::now() - start;

    // If the hub held a lock across the host callback, the push could not take it, stop would wait its full 2 s, and
    // this would run that long. Fast means no lock was held across stop.
    CHECK(elapsed < 1s);
    CHECK(stops_of(ACCEL) == 1);
    CHECK(!read_event(q->read_fd).has_value());  // the racing push saw ACCEL already disabled and was dropped
    shut(*q);
}

// Two threads configuring the same type at once must leave the host in the state the hub commands — no Start landing
// after the final Stop (the security review's out-of-order-notification finding, fixed by notify_mutex).
TEST(concurrent_config_keeps_the_host_in_the_commanded_state) {
    install({ACCEL});
    const auto a = apkrt::sensors::open_queue();
    const auto b = apkrt::sensors::open_queue();
    CHECK(a && b);
    if (!a || !b) return;
    for (int round = 0; round < 300; ++round) {
        std::thread t1([&] { apkrt::sensors::enable(a->id, ACCEL, min_period_ns); });
        std::thread t2([&] { apkrt::sensors::enable(b->id, ACCEL, min_period_ns); });
        t1.join();
        t2.join();
        CHECK(started(ACCEL));  // both enabled it, so the host must be started
        std::thread t3([&] { apkrt::sensors::disable(a->id, ACCEL); });
        std::thread t4([&] { apkrt::sensors::disable(b->id, ACCEL); });
        t3.join();
        t4.join();
        CHECK(!started(ACCEL));  // both disabled it, so the host must be stopped
    }
    shut(*a);
    shut(*b);
}

// A guest that never reads its queue must not hang a push: the write end is non-blocking, so a full pipe drops the
// record (deleting the write-end O_NONBLOCK in open_queue would hang here forever).
TEST(push_does_not_block_when_the_pipe_is_full) {
    install({ACCEL});
    const auto q = apkrt::sensors::open_queue();
    CHECK(q.has_value());
    if (!q) return;
    CHECK(apkrt::sensors::enable(q->id, ACCEL, min_period_ns) == 0);
    const float values[] = {1, 2, 3};
    for (int i = 0; i < 4000; ++i) apkrt_sensor_push(ACCEL, values, 3, 1.0, HIGH);  // far more than the pipe holds
    CHECK(read_event(q->read_fd).has_value());  // reaching here = nothing blocked; some records are in the pipe
    shut(*q);
}

// A start the host rejects is retried on the next enable, even at the same rate (apkrt.h's contract) — the hub forgets
// the commanded rate on a failure.
TEST(a_failed_start_is_retried_on_the_next_enable) {
    install({ACCEL});
    host.fail_next_start.store(1);
    const auto q = apkrt::sensors::open_queue();
    CHECK(q.has_value());
    if (!q) return;
    CHECK(apkrt::sensors::enable(q->id, ACCEL, min_period_ns) == 0);  // start #1 is rejected
    CHECK(!started(ACCEL) && starts_of(ACCEL) == 1);
    CHECK(apkrt::sensors::enable(q->id, ACCEL, min_period_ns) == 0);  // same rate, but the hub retries the start
    CHECK(started(ACCEL) && starts_of(ACCEL) == 2);
    shut(*q);
}

int main() { return apk::testing::run_all(); }
