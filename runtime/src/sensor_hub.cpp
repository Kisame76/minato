#include "sensor_hub.h"

#include <fcntl.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <vector>

#include "APKRuntime/apkrt.h"

namespace apkrt::sensors {
namespace {

// The fixed table of the sensors Minato offers, in the order ASensorManager_getSensorList returns them (the
// non-uncalibrated ones first, the two uncalibrated last). handle == the Android TYPE_ number. The ranges, resolutions
// and power are nominal for an iPhone fed by CoreMotion — plausible for an app that reads getMaximumRange, not measured.
const SensorInfo table[] = {
    {1, 3, 1, sensor_min_delay_us, sensor_max_delay_us, reporting_continuous, 0, 78.4532f, 0.0023927f, 0.23f,
     "Minato Accelerometer", "Minato", "android.sensor.accelerometer", ""},
    {4, 3, 1, sensor_min_delay_us, sensor_max_delay_us, reporting_continuous, 0, 34.906586f, 0.0012217f, 0.55f,
     "Minato Gyroscope", "Minato", "android.sensor.gyroscope", ""},
    {2, 3, 1, sensor_min_delay_us, sensor_max_delay_us, reporting_continuous, 0, 4912.0f, 0.15f, 0.5f,
     "Minato Magnetometer", "Minato", "android.sensor.magnetic_field", ""},
    {9, 3, 1, sensor_min_delay_us, sensor_max_delay_us, reporting_continuous, 0, 19.6133f, 0.0023927f, 0.23f,
     "Minato Gravity", "Minato", "android.sensor.gravity", ""},
    {10, 3, 1, sensor_min_delay_us, sensor_max_delay_us, reporting_continuous, 0, 78.4532f, 0.0023927f, 0.23f,
     "Minato Linear Acceleration", "Minato", "android.sensor.linear_acceleration", ""},
    {11, 5, 1, sensor_min_delay_us, sensor_max_delay_us, reporting_continuous, 0, 1.0f, 5.96e-8f, 0.55f,
     "Minato Rotation Vector", "Minato", "android.sensor.rotation_vector", ""},
    {15, 4, 1, sensor_min_delay_us, sensor_max_delay_us, reporting_continuous, 0, 1.0f, 5.96e-8f, 0.55f,
     "Minato Game Rotation Vector", "Minato", "android.sensor.game_rotation_vector", ""},
    {14, 6, 1, sensor_min_delay_us, sensor_max_delay_us, reporting_continuous, 0, 4912.0f, 0.15f, 0.5f,
     "Minato Magnetometer (uncalibrated)", "Minato", "android.sensor.magnetic_field_uncalibrated", ""},
    {16, 6, 1, sensor_min_delay_us, sensor_max_delay_us, reporting_continuous, 0, 34.906586f, 0.0012217f, 0.55f,
     "Minato Gyroscope (uncalibrated)", "Minato", "android.sensor.gyroscope_uncalibrated", ""},
};
// APKRT_SENSOR_LIST marshals the whole table into a fixed buffer of APKRT_SENSOR_MAX entries, so it must fit.
static_assert(sizeof(table) / sizeof(table[0]) <= APKRT_SENSOR_MAX, "the offered sensors must fit the LIST buffer");

// At most this many queues open at once. A guest that opened thousands would make every push do that many pipe writes
// on the host's CoreMotion thread; one game needs only a few (one per SensorManager queue).
constexpr std::size_t queue_limit = 64;

[[clang::no_destroy]] constinit std::mutex mutex;  // constant-init + immortal (host push, network.cpp); guards below

// Serializes the host's start/stop calls so their order matches the order the config ops decided them in, so no
// out-of-order notification occurs. A config op takes this first, then `mutex` for the decision, drops `mutex`, and
// makes the host call still holding this; apkrt_sensor_push takes only `mutex`, so a host stop that blocks
// on an in-flight push cannot deadlock. Never taken while `mutex` is held.
[[clang::no_destroy]] constinit std::mutex notify_mutex;  // constant-init + immortal (host push, network.cpp)

apkrt_sensor_host registered{};             // the host's context and start/stop
bool have_host = false;                     // whether a host with >= 1 available sensor is set
[[clang::no_destroy]] std::vector<std::int32_t> available_types;  // the TYPE_ numbers the host said CoreMotion can feed

struct Queue {
    int id;
    int write_fd;                       // the hub's end of the pipe; push and flush write whole records here
    std::vector<std::int32_t> enabled;  // the types enabled on this queue, in the order enabled (for flush)
};
[[clang::no_destroy]] std::vector<Queue> queues;
int next_queue_id = 1;

// One queue's request for a type: which queue, at what period, and its pipe write end (kept here so a push needs no
// per-queue lookup — it writes straight to each enabler's write_fd).
struct Request {
    int queue_id;
    std::int64_t period_ns;
    int write_fd;
};

// A type with at least one queue wanting it: who wants it, and the period last told to the host.
struct TypeState {
    std::int32_t type;
    std::vector<Request> requests;
    std::int64_t commanded = -1;
};
[[clang::no_destroy]] std::vector<TypeState> states;

const SensorInfo *table_entry(std::int32_t type) {
    for (const auto &s : table) {
        if (s.type == type) return &s;
    }
    return nullptr;
}

// The sensor of TYPE_ `type` the hub offers: in the table and listed available by the host. Caller holds the lock.
const SensorInfo *offered(std::int32_t type) {
    const SensorInfo *entry = table_entry(type);
    if (entry == nullptr) return nullptr;
    const bool ok = std::find(available_types.begin(), available_types.end(), type) != available_types.end();
    return ok ? entry : nullptr;
}

Queue *find_queue(int id) {
    for (auto &q : queues) {
        if (q.id == id) return &q;
    }
    return nullptr;
}

TypeState *find_state(std::int32_t type) {
    for (auto &s : states) {
        if (s.type == type) return &s;
    }
    return nullptr;
}

std::int64_t clamp_period(std::int64_t period_ns) {
    const std::int64_t lo = static_cast<std::int64_t>(sensor_min_delay_us) * 1000;
    const std::int64_t hi = static_cast<std::int64_t>(sensor_max_delay_us) * 1000;
    return period_ns < lo ? lo : (period_ns > hi ? hi : period_ns);
}

std::int64_t merged_period(const TypeState &s) {
    if (s.requests.empty()) return static_cast<std::int64_t>(sensor_max_delay_us) * 1000;  // no caller passes this
    std::int64_t best = s.requests.front().period_ns;
    for (const auto &r : s.requests) best = std::min(best, r.period_ns);
    return best;
}

// A host call to make once the lock is dropped (the hub never calls start/stop while holding the lock).
struct HostAction {
    enum Kind { None, Start, Stop } kind = None;
    std::int32_t type = 0;
    std::int64_t period = 0;
};

void run(const HostAction &action) {
    if (action.kind == HostAction::None) return;
    apkrt_sensor_host host;  // copied under the lock: set_host may run on another thread
    bool present;
    {
        const std::lock_guard<std::mutex> hold(mutex);
        present = have_host;
        host = registered;
    }
    if (!present) return;
    if (action.kind == HostAction::Start) {
        const int result = host.start != nullptr ? host.start(host.context, action.type, action.period) : 0;
        if (result < 0) {
            // The host could not start the sensor: forget the commanded rate so a later enable retries it, as apkrt.h
            // promises. (run runs under notify_mutex, not mutex, so re-taking mutex here is safe.)
            const std::lock_guard<std::mutex> hold(mutex);
            if (TypeState *s = find_state(action.type)) s->commanded = -1;
        }
    } else if (host.stop != nullptr) {
        host.stop(host.context, action.type);
    }
}

// Drops queue `id`'s request for `type` (caller holds the lock) and returns the host call its merged rate now needs.
HostAction forget_request(int id, std::int32_t type) {
    TypeState *s = find_state(type);
    if (s == nullptr) return {};
    s->requests.erase(std::remove_if(s->requests.begin(), s->requests.end(), [&](auto &r) { return r.queue_id == id; }),
                      s->requests.end());
    if (s->requests.empty()) {
        states.erase(std::remove_if(states.begin(), states.end(), [&](auto &e) { return e.type == type; }),
                     states.end());
        return {HostAction::Stop, type, 0};
    }
    const std::int64_t merged = merged_period(*s);
    if (merged != s->commanded) {
        s->commanded = merged;
        return {HostAction::Start, type, merged};
    }
    return {};
}

// The shift from CoreMotion's clock (Darwin CLOCK_UPTIME_RAW, no sleep) to the guest's CLOCK_BOOTTIME (Darwin
// CLOCK_MONOTONIC, through sleep), as syscalls.cpp maps the Linux clocks.
std::int64_t boot_minus_uptime() {
    return static_cast<std::int64_t>(clock_gettime_nsec_np(CLOCK_MONOTONIC)) -
           static_cast<std::int64_t>(clock_gettime_nsec_np(CLOCK_UPTIME_RAW));
}

}  // namespace

std::optional<OpenQueue> open_queue() {
    // A pipe, not a socketpair: it is one-way (the guest's end is read-only, so the guest cannot write into it), and a
    // write of one whole record (104 bytes <= PIPE_BUF) is atomic, so a record is never torn and a full pipe drops it
    // whole. ends[0] is the read end (the guest's), ends[1] the write end (the hub's).
    int ends[2];
    if (::pipe(ends) != 0) return std::nullopt;
    bool ok = true;
    for (const int end : ends) {
        ok = ok && ::fcntl(end, F_SETFD, FD_CLOEXEC) == 0 && ::fcntl(end, F_SETFL, O_NONBLOCK) == 0;
    }
    // The write end must not raise SIGPIPE when the guest has closed the read end, and both ends must be non-blocking:
    // a blocking write end would hang a push under `mutex`. A flag that would not set means the queue is not safe.
    ok = ok && ::fcntl(ends[1], F_SETNOSIGPIPE, 1) == 0;
    if (!ok) {
        ::close(ends[0]);
        ::close(ends[1]);
        return std::nullopt;
    }
    const std::lock_guard<std::mutex> hold(mutex);
    if (queues.size() >= queue_limit) {
        ::close(ends[0]);
        ::close(ends[1]);
        return std::nullopt;  // the transport answers -EMFILE
    }
    const int id = next_queue_id++;
    queues.push_back({id, ends[1], {}});
    return OpenQueue{id, ends[0]};
}

void close_queue(int id) {
    const std::lock_guard<std::mutex> serialize(notify_mutex);  // keep the host stops in decision order
    std::vector<HostAction> actions;
    {
        const std::lock_guard<std::mutex> hold(mutex);
        Queue *q = find_queue(id);
        if (q == nullptr) return;
        for (const std::int32_t type : q->enabled) actions.push_back(forget_request(id, type));
        // Closing the write end under the lock is safe: a push writes to it only while holding this same lock.
        ::close(q->write_fd);
        queues.erase(std::remove_if(queues.begin(), queues.end(), [&](auto &e) { return e.id == id; }), queues.end());
    }
    for (const auto &action : actions) run(action);
}

int enable(int id, std::int32_t type, std::int64_t period_ns) {
    const std::lock_guard<std::mutex> serialize(notify_mutex);  // keep the host start/stop in decision order
    HostAction action;
    {
        const std::lock_guard<std::mutex> hold(mutex);
        Queue *q = find_queue(id);
        if (q == nullptr) return -EINVAL;
        if (offered(type) == nullptr) return -EINVAL;
        period_ns = clamp_period(period_ns);
        if (std::find(q->enabled.begin(), q->enabled.end(), type) == q->enabled.end()) q->enabled.push_back(type);
        TypeState *s = find_state(type);
        if (s == nullptr) {
            states.push_back({type, {}, -1});
            s = &states.back();
        }
        bool updated = false;
        for (auto &r : s->requests) {
            if (r.queue_id == id) {
                r.period_ns = period_ns;
                updated = true;
            }
        }
        if (!updated) s->requests.push_back({id, period_ns, q->write_fd});
        const std::int64_t merged = merged_period(*s);
        if (merged != s->commanded) {
            s->commanded = merged;
            action = {HostAction::Start, type, merged};
        }
    }
    run(action);
    return 0;
}

int disable(int id, std::int32_t type) {
    const std::lock_guard<std::mutex> serialize(notify_mutex);  // keep the host start/stop in decision order
    HostAction action;
    {
        const std::lock_guard<std::mutex> hold(mutex);
        Queue *q = find_queue(id);
        if (q == nullptr) return -EINVAL;
        const auto at = std::find(q->enabled.begin(), q->enabled.end(), type);
        if (at == q->enabled.end()) return 0;  // not enabled here: a harmless no-op
        q->enabled.erase(at);
        action = forget_request(id, type);
    }
    run(action);
    return 0;
}

int set_rate(int id, std::int32_t type, std::int64_t period_ns) {
    const std::lock_guard<std::mutex> serialize(notify_mutex);  // keep the host start/stop in decision order
    HostAction action;
    {
        const std::lock_guard<std::mutex> hold(mutex);
        Queue *q = find_queue(id);
        if (q == nullptr) return -EINVAL;
        if (std::find(q->enabled.begin(), q->enabled.end(), type) == q->enabled.end()) return -EINVAL;
        period_ns = clamp_period(period_ns);
        TypeState *s = find_state(type);
        if (s == nullptr) return -EINVAL;
        for (auto &r : s->requests) {
            if (r.queue_id == id) r.period_ns = period_ns;
        }
        const std::int64_t merged = merged_period(*s);
        if (merged != s->commanded) {
            s->commanded = merged;
            action = {HostAction::Start, type, merged};
        }
    }
    run(action);
    return 0;
}

int flush(int id) {
    const std::lock_guard<std::mutex> hold(mutex);
    Queue *q = find_queue(id);
    if (q == nullptr) return -EINVAL;
    const std::int64_t now = static_cast<std::int64_t>(clock_gettime_nsec_np(CLOCK_MONOTONIC));
    for (const std::int32_t type : q->enabled) {
        Event event{};
        event.version = static_cast<std::int32_t>(sizeof(Event));
        event.sensor = type;
        event.type = meta_type;
        event.timestamp = now;
        event.meta_data.what = meta_flush_complete;
        event.meta_data.sensor = type;
        (void)::write(q->write_fd, &event, sizeof event);  // non-blocking; a full pipe drops the record
    }
    return 0;
}

std::optional<SensorInfo> sensor(std::int32_t type) {
    const std::lock_guard<std::mutex> hold(mutex);
    const SensorInfo *entry = offered(type);
    if (entry == nullptr) return std::nullopt;
    return *entry;
}

std::size_t sensor_list(SensorInfo *out, std::size_t max) {
    const std::lock_guard<std::mutex> hold(mutex);
    std::size_t count = 0;
    for (const auto &entry : table) {
        if (offered(entry.type) == nullptr) continue;
        if (out != nullptr && count < max) out[count] = entry;
        ++count;
    }
    return count;
}

void reset() {
    // Tests and the one process-teardown path: drop every queue and host. It makes no host stop call (a test's fake
    // host needs none), so it must not run while a guest holds a queue in normal use.
    const std::lock_guard<std::mutex> hold(mutex);
    for (const auto &q : queues) ::close(q.write_fd);
    queues.clear();
    states.clear();
    available_types.clear();
    registered = {};
    have_host = false;
    next_queue_id = 1;
}

}  // namespace apkrt::sensors

extern "C" {

void apkrt_sensor_set_host(const apkrt_sensor_host *host, const int32_t *available, size_t available_count) {
    const std::lock_guard<std::mutex> hold(apkrt::sensors::mutex);
    apkrt::sensors::available_types.clear();
    if (host == nullptr || available == nullptr || available_count == 0) {
        apkrt::sensors::registered = {};
        apkrt::sensors::have_host = false;
        return;
    }
    apkrt::sensors::registered = *host;
    apkrt::sensors::have_host = true;
    apkrt::sensors::available_types.assign(available, available + available_count);
}

void apkrt_sensor_push(int32_t type, const float *values, size_t count, double host_time_seconds, int32_t accuracy) {
    const std::lock_guard<std::mutex> hold(apkrt::sensors::mutex);
    const apkrt::sensors::SensorInfo *entry = apkrt::sensors::offered(type);
    if (entry == nullptr || values == nullptr || static_cast<std::size_t>(entry->value_count) != count) return;
    const apkrt::sensors::TypeState *s = apkrt::sensors::find_state(type);
    if (s == nullptr || s->requests.empty()) return;  // no queue wants it: nothing started, drop the sample
    if (!std::isfinite(host_time_seconds)) return;    // a non-finite host time would make llround undefined; drop it

    apkrt::sensors::Event event{};
    event.version = static_cast<std::int32_t>(sizeof(apkrt::sensors::Event));
    event.sensor = type;
    event.type = type;
    event.timestamp =
        static_cast<std::int64_t>(std::llround(host_time_seconds * 1e9)) + apkrt::sensors::boot_minus_uptime();
    for (std::size_t i = 0; i < count && i < apkrt::sensors::sensor_value_max; ++i) event.data[i] = values[i];
    if (count <= 3) event.vector.status = static_cast<std::int8_t>(accuracy);  // the status byte for a 3-axis sensor

    // Each enabler's write end is kept in its request, so the fan-out needs no per-queue lookup.
    for (const apkrt::sensors::Request &request : s->requests) {
        (void)::write(request.write_fd, &event, sizeof event);  // non-blocking; a full pipe drops it
    }
}

}  // extern "C"
