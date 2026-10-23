#ifndef APKRT_SENSOR_SYSCALLS_H
#define APKRT_SENSOR_SYSCALLS_H

// The guest's side of the sensor hub (ADR-0061): the handler of APKRT_NR_SENSOR. It opens queues as guest descriptors,
// reads the guest's request structs through checked copies, and drives the hub (sensor_hub.h). Separate from the hub so
// the hub stays testable without the descriptor layer.

#include <cstdint>

namespace apkrt::sensors {

/// Handles APKRT_NR_SENSOR: `op` is an APKRT_SENSOR_ op, `argument` its x1 (a scalar or a guest address, per the op).
/// Returns the op's result or a negative Linux errno.
long call(std::uint64_t op, std::uint64_t argument);

}  // namespace apkrt::sensors

#endif  // APKRT_SENSOR_SYSCALLS_H
