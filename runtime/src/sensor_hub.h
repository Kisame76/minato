#ifndef APKRT_SENSOR_HUB_H
#define APKRT_SENSOR_HUB_H

// The sensor hub (ADR-0061 step 1). One hub for the process. The host (App's CoreMotion side) registers itself and the
// sensors it can feed with apkrt_sensor_set_host, starts/stops them when the hub asks (never under the hub's lock, so a
// blocking stop cannot deadlock a push), and pushes samples with apkrt_sensor_push. The hub keeps a fixed table of the
// sensors Android knows (by TYPE_ number), a list of open queues each with a pipe whose read end the guest reads,
// and the started state and merged rate per type. A push fans the sample out as a whole ASensorEvent record into every
// queue that has the type enabled; the guest's ASensorEventQueue (the NDK side, sensor_manager.c) and the framework's
// Java SystemSensorManager read the same records. Nothing here is guest-reachable directly: the guest reaches the hub
// through the APKRT_NR_SENSOR_* syscalls, whose handlers call these functions; the host reaches it through apkrt.h's C
// API.
//
// This header is the runtime-internal C++ face of the hub, used by the syscall handlers, by apkrt.cpp's host entry
// points, and by sensor_hub_test.cpp (which drives it with a fake host, no CoreMotion and no syscalls).

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace apkrt::sensors {

/// A sensor the hub offers, filled from the fixed table. The metadata the NDK's ASensor_* lack and the Java Sensor needs
/// (minato_ASensor_getInfo marshals these to the framework's JNI). handle is the sensor's identity in every event and
/// equals the Android TYPE_ number. The char pointers are static, valid for the life of the process.
struct SensorInfo {
    std::int32_t type;           // Android ASENSOR_TYPE_ number, also the handle
    std::int32_t value_count;    // floats a sample carries (3, 4, 5 or 6)
    std::int32_t version;        // 1
    std::int32_t min_delay_us;   // the fastest period the guest may ask (sensor_min_delay_us)
    std::int32_t max_delay_us;   // the slowest (sensor_max_delay_us)
    std::int32_t reporting_mode; // AREPORTING_MODE_CONTINUOUS for all of ours
    std::int32_t flags;          // Android's Sensor flags word; 0 (continuous, non-wakeup, non-dynamic) for all of ours
    float max_range;             // maximum value a sample may reach, the sensor's unit
    float resolution;            // smallest change the sensor reports, the sensor's unit
    float power_ma;              // nominal current draw, mA
    const char *name;            // "Minato <sensor>"
    const char *vendor;          // "Minato"
    const char *string_type;     // "android.sensor.accelerometer" and so on
    const char *required_permission;  // "" (none of ours needs one)
};

/// The rate bounds and the accuracy the hub clamps to. A guest period below sensor_min_delay_us becomes it; above
/// sensor_max_delay_us becomes it; there is no batching (fifo 0).
inline constexpr std::int32_t sensor_min_delay_us = 10000;    // 100 Hz
inline constexpr std::int32_t sensor_max_delay_us = 1000000;  // 1 Hz
inline constexpr std::size_t sensor_value_max = 6;            // the widest sample (uncalibrated types)

// A few Android sensor ABI values the hub and whatever reads its records share, so the runtime needs none of the guest's
// <android/sensor.h> (the guest's sensor_manager.c uses the real header and asserts the two layouts agree).
inline constexpr std::int32_t reporting_continuous = 0;     // AREPORTING_MODE_CONTINUOUS
inline constexpr std::int32_t status_accuracy_high = 3;     // ASENSOR_STATUS_ACCURACY_HIGH
inline constexpr std::int32_t meta_type = 0;                // SENSOR_TYPE_META_DATA
inline constexpr std::int32_t meta_flush_complete = 1;      // META_DATA_FLUSH_COMPLETE

/// One ASensorEvent as <android/sensor.h> lays it out: 104 bytes, a backward-compatible ABI. The hub writes whole
/// records of this into each queue's pipe and the guest reads them as ASensorEvent, so the layout must match byte
/// for byte (guarded by static_assert here and against the real header in sensor_manager.c). Only the fields the hub
/// fills are named; the rest match the header's padding and reserved words.
struct Event {
    /// A 3-axis sensor's vector; `status` holds the accuracy.
    struct Vector {
        float x, y, z;
        std::int8_t status;
        std::uint8_t reserved[3];
    };
    /// A meta event: `what` is meta_flush_complete, `sensor` the flushed sensor's handle.
    struct Meta {
        std::int32_t what;
        std::int32_t sensor;
    };
    std::int32_t version;    // sizeof(Event)
    std::int32_t sensor;     // the sensor's handle (its Android TYPE_ number), as ASensor_getHandle
    std::int32_t type;       // an Android ASENSOR_TYPE_ number, or meta_type for a flush-complete
    std::int32_t reserved0;
    std::int64_t timestamp;  // nanoseconds on the guest's CLOCK_BOOTTIME / elapsedRealtimeNanos base
    // The structs are named, so the anonymous union holds no nested anonymous type (an extension, -Wnested-anon-types).
    union {
        float data[16];
        Vector vector;
        Meta meta_data;
    };
    std::uint32_t flags;
    std::int32_t reserved1[3];
};
static_assert(sizeof(Event) == 104, "ASensorEvent is a 104-byte ABI");
static_assert(offsetof(Event, timestamp) == 16 && offsetof(Event, flags) == 88, "ASensorEvent field offsets");

/// A newly opened queue: its id for the config and destroy calls, and the pipe read end (O_NONBLOCK) the guest
/// reads whole ASensorEvent records from. The hub keeps the write end.
struct OpenQueue {
    int id;
    int read_fd;
};

/// Opens a queue. A one-way pipe is made, both ends non-blocking and close-on-exec; the write end stays with the hub,
/// and the read end (read-only, so the guest cannot write or forge the channel) is the guest's. The id is monotonic
/// and never reused. std::nullopt (and nothing left open) when the pipe or a flag cannot be set.
std::optional<OpenQueue> open_queue();

/// Closes queue `id`: its write end is closed and the queue dropped so no later push writes to it, under the hub lock,
/// so a push on another thread cannot write to a closed or reused descriptor. A type this queue was the last to enable
/// is stopped on the host, outside the lock. Harmless for an id that is not open. The guest owns the read end and closes
/// it itself (the NDK destroy, or the framework after removeFd).
void close_queue(int id);

/// Enables `type` on queue `id` at `period_ns` (the guest's request, which the hub clamps to [min_delay, max_delay]).
/// Enabling an already-enabled type re-rates it. 0, or -EINVAL for an unknown id or a type the host did not offer. When
/// this is the first queue to want the type, or it changes the merged (fastest) rate, the host's start is called with
/// the new period — outside the hub lock.
int enable(int id, std::int32_t type, std::int64_t period_ns);

/// Disables `type` on queue `id`. 0, or -EINVAL for an unknown id (a type that was not enabled is a no-op, 0). When this
/// was the last queue wanting the type, the host's stop is called — outside the hub lock; otherwise the merged rate is
/// recomputed and the host re-rated if it fell.
int disable(int id, std::int32_t type);

/// Sets the rate of an already-enabled `type` on queue `id` to `period_ns`. 0, or -EINVAL for an unknown id or a type
/// not enabled on it. Re-rates the host when the merged rate changes, outside the lock.
int set_rate(int id, std::int32_t type, std::int64_t period_ns);

/// Writes one META_DATA flush-complete record (type 0, what 1, the sensor's handle) into queue `id` for each type it has
/// enabled, in the order the guest enabled them, as AOSP's SensorEventQueue::flush does for a sensor without a FIFO. 0,
/// or -EINVAL for an unknown id; 0 and nothing written when the queue has no type enabled.
int flush(int id);

/// The sensor of Android TYPE_ `type`, or std::nullopt when the hub does not offer it (not in the table, or the host did
/// not list it available). The event layout and the metadata come from here.
std::optional<SensorInfo> sensor(std::int32_t type);

/// Fills `out` (at least `max` entries) with the offered sensors (the table intersected with the host's available set),
/// and returns their number. Pass max 0 to learn the count. The order is the table's.
std::size_t sensor_list(SensorInfo *out, std::size_t max);

/// Forgets the host and every queue and stops every started type — for tests between cases, and the one process teardown
/// path. Not called in normal running (one game per process).
void reset();

}  // namespace apkrt::sensors

#endif  // APKRT_SENSOR_HUB_H
