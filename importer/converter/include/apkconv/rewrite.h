// The rewriter: finds the instructions that cannot run unchanged on XNU and replaces each with a
// branch to a stub in the container (ADR-0010, ADR-0019). Only these exact patterns are touched:
//
//   mrs xN, TPIDR_EL0   XNU overwrites TPIDR_EL0 (F3). The island loads the guest thread pointer
//                       from Darwin TSD slot 6 instead: mrs xN, TPIDRRO_EL0; bic; ldr [xN, #48].
//   msr TPIDR_EL0, xN   Same slot, written; the island saves one scratch register on the stack.
//   svc #0              XNU takes the syscall number from x16, Linux from x8 (F5). The veneer calls
//                       the Linux syscall layer through a slot the loader fills in.
//   any use of x18      XNU sets x18 to zero at every context switch (F4). The island runs the
//                       instruction with a scratch register in place of x18, whose value lives in
//                       Darwin TSD slot 11 (ADR-0020). Forms that need more than that are listed as
//                       unsupported, and the converter refuses the file.
//
// Every other instruction stays as it is: no emulation, no trapping (ADR-0001).
#pragma once

#include "APKRuntime/container_format.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <stdexcept>
#include <vector>

namespace apk::rewrite {

enum class SiteKind : std::uint8_t {
    thread_pointer_read,   // mrs xN, TPIDR_EL0
    thread_pointer_write,  // msr TPIDR_EL0, xN
    syscall,               // svc #0
    x18,                   // an instruction that uses x18
};

struct Site {
    std::uint64_t address;  // of the instruction, in the address space given to scan()
    SiteKind kind;
    std::uint8_t reg;  // N of xN, 31 for xzr; 0 for syscalls; 18 for x18
    std::uint32_t instruction = 0;  // the instruction itself, for x18

    bool operator==(const Site &) const = default;
};

/// An instruction that uses x18 in a form no island handles (yet), and why.
struct Unsupported {
    std::uint64_t address;
    const char *reason;

    bool operator==(const Unsupported &) const = default;
};

/// A run from a load-exclusive to the store-exclusive or CLREX that closes it, with at least one
/// use of x18 inside (ADR-0020 X2c). No other memory access may fall between the two, so the whole
/// run is relocated as one island: x18 becomes a scratch register throughout, loaded from the TSD
/// once before the run and stored back once after, both outside the exclusive window. Only a window
/// that runs the same when it runs again from its load-exclusive is relocated: a signal inside the
/// island sends the thread back there (ADR-0020 X4).
struct ExclusiveRegion {
    std::uint64_t address;             // of the load-exclusive, in the address space given to scan()
    std::vector<std::uint32_t> words;  // the whole window, load-exclusive to close, inclusive
    bool reads_x18 = false;            // a path reads x18 before writing it -> load the scratch first
    bool writes_x18 = false;           // x18 is written -> store the scratch back after
    std::uint8_t scratch = 0;          // stands in for x18 across the window (the value)
    std::uint8_t base = 0;             // holds the TSD base for the store-back; used only when writing

    bool operator==(const ExclusiveRegion &) const = default;
};

/// A `br x18` that ends a jump table as compilers build it (ADR-0020 X2d):
///   cmp wI, #n; b.hi …; … ldrsw x18, [xB, xI, lsl #2]; add x18, x18, xB; br x18
/// with the table base from ADRP and ADD in xB. Its island reads the target the guest computed and
/// branches on through a landing pad, one per word of the targets' span, which restores the
/// scratch registers first; a target without one traps.
struct JumpTable {
    std::uint64_t address;               // of the br x18, in the address space given to scan()
    std::uint64_t table;                 // of the table: 32-bit entries, each an offset from here
    std::uint32_t entries = 0;           // from the bounds check
    std::vector<std::uint64_t> targets;  // the distinct targets, ascending

    bool operator==(const JumpTable &) const = default;
};

struct Findings {
    std::vector<Site> sites;
    /// `svc #imm` with imm != 0. Left alone it would run whatever Darwin syscall x16 names (F5), and
    /// Android code has no reason to use one, so the converter refuses such a file.
    std::vector<std::uint64_t> other_syscalls;
    /// Instructions that read x18, whose value XNU sets to zero at every context switch (F4).
    std::vector<std::uint64_t> x18_reads;
    /// Instructions that write x18 and do not read it.
    std::vector<std::uint64_t> x18_writes;
    /// Stores of x18 in a dump of the whole register file (libunwind's unw_getcontext, hwasan):
    /// stp x18, x19 between stp x16, x17 and stp x20, x21 to the neighbouring slots.
    std::vector<std::uint64_t> x18_saves;
    /// Uses of x18 that no island handles; the converter refuses a file with any of them.
    std::vector<Unsupported> x18_unsupported;
    /// Load-/store-exclusive windows with x18 inside, each relocated as one island (ADR-0020 X2c).
    std::vector<ExclusiveRegion> exclusive_regions;
    /// Each `br x18` that ends a jump table, run in an island with landing pads (ADR-0020 X2d).
    std::vector<JumpTable> jump_tables;
};

/// `size` bytes of read-only data at `address` (in the address space given to scan()), for the
/// tables of jump tables. Empty when they are not all in data that is loaded but neither written
/// nor run: code could change a table there, or the rewriter could, taking it for instructions.
using ReadOnlyData = std::function<std::span<const std::byte>(std::uint64_t address, std::size_t size)>;

class RewriteError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Offset of the guest thread pointer from the Darwin TSD base: slot 6 (ADR-0019), the same value
/// as APKRT_GUEST_TLS_OFFSET in the runtime.
inline constexpr std::uint32_t guest_thread_pointer_offset = 8 * 6;

/// Offset of the guest's x18 from the Darwin TSD base: slot 11, the other slot Apple leaves free
/// (ADR-0019, ADR-0020); the same value as APKRT_GUEST_X18_OFFSET in the runtime.
inline constexpr std::uint32_t guest_x18_offset = 8 * 11;

/// Scans the instructions in `code`, which starts at `address`, and appends what it finds. Tables of
/// jump tables are read through `read_only_data`; without it, a br x18 is refused.
void scan(std::span<const std::byte> code, std::uint64_t address, Findings &findings,
          const ReadOnlyData &read_only_data = {});

/// Bytes the stub of `site` needs in the stub area; 0 when the site is replaced in place.
std::size_t stub_size(const Site &site);

/// Writes the stub of `site` into `stub` (exactly stub_size(site) bytes) and returns the
/// instruction that replaces the site. The addresses are all in the container: where the site is,
/// where its stub goes, and where the loader puts the address of the syscall entry.
std::uint32_t emit(const Site &site, std::uint64_t site_address, std::uint64_t stub_address,
                   std::uint64_t syscall_slot_address, std::span<std::byte> stub);

/// Bytes the relocated island of an exclusive region needs in the stub area.
std::size_t exclusive_region_stub_size(const ExclusiveRegion &region);

/// Writes the relocated island of `region` into `stub` (exactly exclusive_region_stub_size bytes)
/// and returns the branch that replaces the load-exclusive at `region_address`. `stub_address` is
/// where the island goes; both are container addresses.
std::uint32_t emit_exclusive_region(const ExclusiveRegion &region, std::uint64_t region_address,
                                    std::uint64_t stub_address, std::span<std::byte> stub);

/// Bytes the island of a jump table and its landing pads need in the stub area.
std::size_t jump_table_stub_size(const JumpTable &table);

/// Writes the island of `table` and its landing pads into `stub` (exactly jump_table_stub_size
/// bytes) and returns the branch that replaces the br at `branch_address`. `stub_address` is where
/// the island goes; both are container addresses, and each target keeps its distance to the br.
std::uint32_t emit_jump_table(const JumpTable &table, std::uint64_t branch_address, std::uint64_t stub_address,
                              std::span<std::byte> stub);

/// What the island table of the container records about one x18 island (ADR-0020 X4): the fields of
/// struct apk_island that do not depend on where the island and its site lie.
struct IslandLayout {
    std::uint8_t kind = 0;                       // APK_ISLAND_*
    std::uint32_t words = 0;                     // the island's length, landing pads included
    std::uint8_t value = 0;                      // the register that stands in for x18
    std::uint8_t base = APK_ISLAND_NO_REGISTER;  // the second scratch register
    std::uint8_t commit = APK_ISLAND_NO_COMMIT;  // the word that completes the guest instruction or window
    std::uint8_t store = 0;                      // the word that stores x18 to the TSD; 0 when there is none
    std::uint8_t resume = 0;                     // words from the site to where the thread goes on after the commit

    bool operator==(const IslandLayout &) const = default;
};

/// The layout of the island emit() writes for `site`, which must be an x18 site.
IslandLayout island_layout(const Site &site);

/// The layout of the island emit_exclusive_region() writes for `region`.
IslandLayout exclusive_region_layout(const ExclusiveRegion &region);

/// The layout of the island and the landing pads emit_jump_table() writes for `table`.
IslandLayout jump_table_layout(const JumpTable &table);

}  // namespace apk::rewrite
