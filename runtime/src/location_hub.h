#ifndef APKRT_LOCATION_HUB_H
#define APKRT_LOCATION_HUB_H

// The location hub (ADR-0063). One hub for the process, like the sensor hub, but it delivers through the session event
// stream (APKRT_EVENT_LOCATION) rather than a pipe, because the consumer is the Java ILocationManager pump. The host
// (App's CoreLocation side) registers with apkrt_location_set_host, begins/ends continuous updates and answers
// one-shot (getCurrentLocation) requests when the hub asks, and pushes fixes with apkrt_location_push /
// apkrt_location_push_single (apkrt.h). The hub keeps the latest continuous fix in one slot with a sequence, and each
// outstanding one-shot in its own bounded table keyed by token, which a continuous fix never evicts. It does NOT gate
// on the grant (that is the syscall layer's job, since a push is the trusted host's), but it does read the grant to
// decide precision: a fix handed out under a coarse grant is run through the fudger (location_fudge.h), so the guest
// never sees the precise position. The fudger runs here, in the runtime, never in the guest.
//
// This header is the runtime-internal C++ face, used by the syscall handlers (location_syscalls.cpp), by the host
// entry points (location_hub.cpp), and by location_hub_test.cpp (which drives it with a fake host, no CoreLocation
// and no syscalls).

#include <cstdint>

#include "APKRuntime/guest_interface.h"

namespace apkrt::location {

/// Sets the wanted continuous state and (re)configures the host. The host is asked for `accuracy`, except under a
/// coarse grant, where it is always asked for APKRT_LOCATION_ACCURACY_COARSE. Returns 0. The gate (-EACCES) and the
/// accuracy check are the syscall layer's, before this is reached.
int start(std::int32_t accuracy, std::int32_t distance_m);

/// Clears the wanted continuous state and stops the host. Returns 0.
int stop();

/// Registers an outstanding one-shot for `token` and asks the host for a single fix. 0; -EEXIST when `token` is
/// already outstanding (or holds an unfetched fix), -EBUSY past APKRT_LOCATION_ONESHOT_MAX.
int request_single(std::uint64_t token);

/// Drops the outstanding one-shot for `token` and tells the host to stop trying. 0; -ENOENT for a token that is not
/// outstanding.
int cancel_single(std::uint64_t token);

/// Copies the latest continuous fix into `out`, fudged when the grant is not fine. 1 when there is one, else 0.
int last(apkrt_location_fix &out);

/// Copies the fix of sequence `seq` into `out`, fudged when the grant is not fine: the latest continuous fix (kept
/// until a newer one replaces it) or a resolved one-shot (which is then dropped). 1 when `seq` is still available,
/// else 0 (gone or replaced).
int get(std::uint64_t seq, apkrt_location_fix &out);

/// The host pushes one continuous fix: stored as the latest, announced with APKRT_EVENT_LOCATION and a fresh
/// sequence. Dropped (returns 0) when no continuous updates are wanted or the fix is invalid (negative horizontal
/// accuracy). Returns the sequence it assigned, else 0.
std::uint64_t push_continuous(const apkrt_location_fix &fix);

/// The host answers a one-shot for `token`: `fix` the fix, or nullptr / an invalid fix for no location. Announces it
/// with APKRT_EVENT_LOCATION (bit 0, plus bit 2 for no-fix); a fix is kept until its get() or cancel_single(), a
/// no-fix drops the entry at once. An unknown or already-resolved token is ignored. Returns the sequence assigned to a
/// kept fix, else 0.
std::uint64_t push_single(std::uint64_t token, const apkrt_location_fix *fix);

/// Forgets the host, the continuous slot and every one-shot, and reseeds the fudger — for tests between cases and the
/// one process-teardown path.
void reset();

}  // namespace apkrt::location

#endif  // APKRT_LOCATION_HUB_H
