#ifndef APKRT_BATTERY_H
#define APKRT_BATTERY_H

// The battery transport (App's gap-1 proposal, 2026-10-29). The host reports the battery with apkrt_battery_set
// (apkrt.h); this module stores it and, while a session runs, posts APKRT_EVENT_BATTERY on a change (the dedup lives
// here, because apkrt_session_post writes every call it is given once a session is begun). The GET syscall
// (APKRT_NR_BATTERY) reads the current value synchronously, for a newly-registered receiver before any event. No
// permission is needed (Android's battery broadcast needs none). The Java BatteryManager and the sticky
// ACTION_BATTERY_CHANGED are the framework's (App); this is only the transport.
//
// This header is the runtime-internal C++ face, used by the syscall dispatcher (syscalls.cpp), the host entry point
// (battery.cpp), and battery_transport_test.cpp.

#include <cstdint>

namespace apkrt::battery {

/// APKRT_NR_BATTERY: `op` is APKRT_BATTERY_GET (0), which returns the current battery packed as
/// (level & 0xFFFFFFFF) | (state << 32) — always non-negative — or -ENODATA before the first apkrt_battery_set. Any
/// other op is -EINVAL.
long call(std::uint64_t op);

/// Forgets the stored value and the last-posted value — for tests between cases.
void reset();

}  // namespace apkrt::battery

#endif  // APKRT_BATTERY_H
