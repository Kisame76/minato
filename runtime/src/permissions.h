// The runtime's record of the host's permission answers (ADR-0063 location, ADR-0064 microphone, ADR-0073 camera).
// The host sets a group's outcome through apkrt_permission_answer (the trusted side); the location syscalls read it to
// gate access, so a guest denied in Minato's sheet cannot reach the device by calling the syscalls directly. One
// record per process (one guest per process).
// Header-only (inline), so session.cpp and the location syscalls share the one store.
#pragma once

#include "APKRuntime/guest_interface.h"

#include <array>
#include <cstdint>
#include <mutex>

namespace apkrt::permissions {

/// The outcome of a group the host has not answered yet; distinct from APKRT_PERMISSION_DENIED, which is an answer.
constexpr int unknown = -1;

inline std::mutex &store_mutex() {
    static std::mutex mutex;
    return mutex;
}

/// The stored outcome per group, indexed by group - 1 (location 1, microphone 2, camera 3). `unknown` until the host
/// answers.
inline std::array<int, 3> &store() {
    static std::array<int, 3> outcomes{unknown, unknown, unknown};
    return outcomes;
}

inline bool known_group(std::int32_t group) {
    return group >= 1 && group <= static_cast<std::int32_t>(store().size());
}

/// Records the host's `value` (APKRT_PERMISSION_*) for `group`. An unknown group is ignored.
inline void set(std::int32_t group, std::int32_t value) {
    const std::lock_guard<std::mutex> hold(store_mutex());
    if (known_group(group)) store()[static_cast<std::size_t>(group - 1)] = value;
}

/// The stored outcome for `group`, or `unknown` before the first answer and for an unknown group.
inline int outcome(std::int32_t group) {
    const std::lock_guard<std::mutex> hold(store_mutex());
    return known_group(group) ? store()[static_cast<std::size_t>(group - 1)] : unknown;
}

/// Forgets every group, for tests.
inline void reset() {
    const std::lock_guard<std::mutex> hold(store_mutex());
    store().fill(unknown);
}

}  // namespace apkrt::permissions
