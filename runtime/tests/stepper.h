// Test harness for ADR-0020 X4: single-steps a call on another thread of this process and records
// the thread's state before each instruction, through a Mach exception port and MDSCR_EL1.SS
// (ARM_DEBUG_STATE64). No debugger is involved; a process may step its own threads on macOS.
#pragma once

#include <mach/mach.h>

#include <cstdint>
#include <functional>
#include <stdexcept>
#include <vector>

namespace apkrt::testing {

/// One recorded step of the stepped thread: its state before the instruction at pc.
struct Step {
    arm_thread_state64_t state;
    std::uint64_t red_zone[2];  // sp - 128 and sp - 120, read while the thread waits
    std::uint64_t x18;          // its Darwin TSD slot 11
};

/// A stretch the thread runs without stepping: from `from` to a hardware breakpoint at `to`. Every
/// step is an exception return, which clears the exclusive monitor, so a stepped store-exclusive
/// always fails; an LL/SC window runs free from its load-exclusive to the word after its commit.
struct FreeRun {
    std::uint64_t from;
    std::uint64_t to;
    unsigned after_visits = 0;  // the first visits of `from` are stepped, the later ones run free
};

/// Thrown when the thread ran instructions without a step: then the trace has a gap. XNU sometimes
/// runs a thread a few steps after a free run with the debug registers of that free run, single
/// stepping off (6 of 4,800 stepped calls of islands_test, macOS 27 on an M4 Pro). Under load it
/// comes in bursts: beside a whole CTest at -j10, 40 gaps in 150 runs of islands_test, all in the two
/// calls with free runs, and 5 gaps in a row in 8 of 965 runs. A second hardware breakpoint at the
/// call's return catches that; the caller runs the call again, after a wait that grows.
struct LostSteps : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/// Runs `function(argument)` on a new thread and single-steps it from its first instruction until it
/// returns, except in the free runs. Every step and every breakpoint hit is recorded, and `visit`,
/// when given, sees each one while the thread still waits in it, with its stack and TSD as they are
/// then. `result` gets what the function returned.
std::vector<Step> step_through(std::uint64_t (*function)(std::uint64_t), std::uint64_t argument,
                               const std::vector<FreeRun> &free_runs, std::uint64_t &result,
                               const std::function<void(const Step &)> &visit = {});

}  // namespace apkrt::testing
