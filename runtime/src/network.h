#ifndef APKRT_NETWORK_H
#define APKRT_NETWORK_H

// The network transport (ADR-0054 step d)). The host reports its network with apkrt_network_set (apkrt.h); this module
// stores it and, while a session runs, posts APKRT_EVENT_NETWORK on a change. The GET syscall (APKRT_NR_NETWORK) reads
// the current state synchronously, for ConnectivityManager's answers before any event. ConnectivityManager checks
// ACCESS_NETWORK_STATE itself, as Android's ConnectivityService does; this is only the transport.
//
// This header is the runtime-internal C++ face, used by the syscall dispatcher (syscalls.cpp), the host entry point
// (network.cpp), and network_transport_test.cpp.

#include <cstdint>

namespace apkrt::network {

/// APKRT_NR_NETWORK: `op` APKRT_NETWORK_GET (0) returns the current APKRT_NETWORK_* flags, or -ENODATA before the
/// first apkrt_network_set. `op` APKRT_NETWORK_LINK (1) writes the active network's LinkProperties snapshot
/// (network_link.h) to the guest buffer at `argument` of `size` bytes and returns the byte count it needs, or
/// -ENODATA for no active network. Any other op is -EINVAL. GET ignores `argument`/`size`.
long call(std::uint64_t op, std::uint64_t argument, std::uint64_t size);

/// Forgets the stored and the last-posted state — for tests between cases.
void reset();

}  // namespace apkrt::network

#endif  // APKRT_NETWORK_H
