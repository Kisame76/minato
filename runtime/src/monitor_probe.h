// Whether this core keeps the local exclusive monitor across a memory access to another granule (ADR-0020 X7 c2).
//
// An x18 island whose own accesses (the load of TSD slot 11, the saves in the red zone, the store back to the slot) lie
// between a guest's load- and store-exclusive does not endanger the loop's atomicity, which the monitor guards, only its
// progress: where the core clears the monitor for such an access, the guest's retry loop would never end. The container
// marks a library with such islands (APK_CONTAINER_MONITOR_INSIDE) and the loader asks here before it loads one.
//
// The probe is C3b's method (c3b-monitor-probe): one LL/SC increment whose load-exclusive and store-exclusive have one
// extra instruction between them, tried up to a cap of times per pass; a pass that fails the cap's every try has livelocked,
// which is what a deterministic clear looks like, and an asynchronous clear (an interrupt) only costs a retry. Four
// variants: nothing (the control that must work), CLREX (the control that must livelock, so the probe can see a clear),
// a load from another granule and a store to another granule (the islands' accesses).
#pragma once

#include <cstdint>
#include <optional>

namespace apkrt::monitor {

enum class Variant { clean, clrex, load, store };

struct Result {
    std::uint64_t passes = 0;       // the passes made
    std::uint64_t first_try = 0;    // passes whose store-exclusive succeeded at once
    std::uint64_t retried = 0;      // passes that needed more than one try and succeeded within the cap
    std::uint64_t livelocked = 0;   // passes that failed every one of the cap's tries
};

/// Runs `passes` passes of one variant on a cell of this module's own.
Result run(Variant variant, std::uint64_t passes);

/// Whether the core keeps the monitor for the islands' accesses: the probe ran once (it is cached), its controls behaved
/// (the clean variant never livelocks, the CLREX variant always does) and neither the load nor the store livelocked. A probe
/// whose controls misbehave answers false: nothing is proven.
bool kept();

/// The answer a test wants from kept() in place of the probe's; nullopt gives the probe's back.
void force_for_testing(std::optional<bool> answer);

}  // namespace apkrt::monitor
