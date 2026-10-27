#ifndef APKRT_LOCATION_SYSCALLS_H
#define APKRT_LOCATION_SYSCALLS_H

// The guest's side of the location hub (ADR-0063): the handler of APKRT_NR_LOCATION. It enforces the per-group grant
// the host recorded (every data op answers -EACCES without a location grant, so a guest denied in Minato's sheet
// cannot reach the device by calling the syscall directly), checks the guest's pointers and scalars, and drives the
// hub (location_hub.h). Separate from the hub so the hub stays testable without the syscall layer.

#include <cstdint>

namespace apkrt::location {

/// Handles APKRT_NR_LOCATION: `op` an APKRT_LOCATION_ op, `argument` its x1 (a scalar or a guest address per the op),
/// `sequence` its x2 (the fix sequence, for GET). Returns the op's result or a negative Linux errno.
long call(std::uint64_t op, std::uint64_t argument, std::uint64_t sequence);

}  // namespace apkrt::location

#endif  // APKRT_LOCATION_SYSCALLS_H
