// The x18 islands of the loaded containers (ADR-0020 X4). A thread that a signal stops inside an
// island is sent back to the guest before any handler sees it; for that, the signal handler looks
// up the pc here.
#pragma once

#include "APKRuntime/container_format.h"
#include "guest_context.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace apkrt::islands {

/// A loaded container. Its metadata and island table lie in its __TEXT, which stays mapped for the
/// life of the process: containers are never unloaded (ADR-0003).
struct Container {
    std::uint64_t header;            // address of the Mach-O header
    const apk_container_info *info;  // its __apk_info
    char name[64];                   // the file name without ".dylib", for messages
    const Container *next;
};

/// Registers a container dyld has loaded, with `info` its own __apk_info; loader::load calls it.
/// The entry is never removed. Allocates, so not for signal handlers.
void add(const void *header, const apk_container_info &info, std::string_view name);

/// The container whose stub area holds `pc`, or null. Safe in signal handlers: no locks.
const Container *container_at(std::uint64_t pc);

/// The x18 island of `container` that holds `pc`, or null: the stub area holds thread pointer
/// islands and syscall veneers as well. Safe in signal handlers.
const apk_island *island_at(const Container &container, std::uint64_t pc);

/// The (c)-function R-ranges of `container` (ADR-0020 must-have 6), or empty for a container below
/// APK_CONTAINER_RANGES_VERSION, which has none. A signal whose pc is in one of these ranges finds the guest x18 in
/// that range's register; the signal path reads them to hand a handler the right x18. Safe in signal handlers.
std::span<const apk_c_range> c_ranges(const Container &container);

/// A thread's state after it left an island, and the x18 it has then.
struct Settled {
    GuestContext context;
    std::uint64_t x18;
};

/// Pure: the guest state of a thread stopped at `context.pc` inside `island` of the container whose
/// Mach-O header is at `header`, given the two red zone words the island saved (sp - 128 and
/// sp - 120) and its current x18. See struct apk_island for the rule. `resumable` is whether the
/// container is APK_CONTAINER_RESUMABLE_VERSION or newer: only then does an EXCLUSIVE window use the
/// per-word resumable map; an older container's EXCLUSIVE window is restartable, like any other island.
Settled settle(const apk_island &island, std::uint64_t header, const GuestContext &context,
               std::uint64_t saved_value, std::uint64_t saved_base, std::uint64_t x18, bool resumable);

/// When `context` stopped inside an x18 island, turns it and `x18` (the thread's slot 11) into the
/// guest's state and returns true. Reads the red zone below context.sp, which the island has
/// written, so the thread must be stopped: a signal handler on the thread itself, or the stepper.
/// No locks, no allocation.
bool leave(GuestContext &context, std::uint64_t &x18);

/// When `context.pc` is a guest instruction inside the body of a relocated exclusive window, past its
/// load-exclusive (ADR-0020 X4h), redirects it to the matching word of the island and puts `x18` (the
/// thread's slot 11) into the island's value register, rebuilding the red zone the first signal frame
/// clobbered, so a thread resuming there re-enters the island -- where x18 lives in a preserved
/// register -- instead of the original body, which iOS has left running with x18 cleared to 0. Returns
/// true when it redirected. Call it on the resume paths (rt_sigreturn and the fault retry), never for
/// the handler's own view of the guest state, which stays the exact interrupted pc. No locks.
bool reenter(GuestContext &context, std::uint64_t x18);

/// A jump table island that found no landing pad for the target the guest computed (ADR-0020 X2d).
struct JumpTableTrap {
    const Container *container;
    const apk_island *island;
    std::uint64_t target;  // the guest's x18
};

/// The trap when `pc` is a `brk #0x18` of a jump table island, else nullopt. Safe in signal handlers.
std::optional<JumpTableTrap> jump_table_trap(std::uint64_t pc, std::uint64_t x18);

/// "apkrt: br x18 at libgame.so+0x9ab704 jumps to 0x... (libgame.so+0x9ab800), which its jump table
/// does not hold (ADR-0020)\n" into `buffer`, without allocating, cut to `size`; returns the length.
/// The target's ELF address comes only when it lies in the image.
std::size_t describe(const JumpTableTrap &trap, char *buffer, std::size_t size);

/// The buffer the fault handler gives describe(): the longest message, with two names of 63
/// characters and three addresses of 16 hexadecimal digits, has 261 bytes.
inline constexpr std::size_t message_capacity = 272;

}  // namespace apkrt::islands
