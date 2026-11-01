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
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

namespace apk::rewrite {

enum class SiteKind : std::uint8_t {
    thread_pointer_read,   // mrs xN, TPIDR_EL0
    thread_pointer_write,  // msr TPIDR_EL0, xN
    syscall,               // svc #0
    x18,                   // an instruction that uses x18
    x18_nosave,            // an in-sequence x18 use (inside a live LL/SC), islanded over a register proven dead at
                           // the use, touching only TSD slot 11, with no red-zone save (ADR-0020 C3b, C7)
};

struct Site {
    std::uint64_t address;  // of the instruction, in the address space given to scan()
    SiteKind kind;
    std::uint8_t reg;  // N of xN, 31 for xzr; 0 for syscalls; 18 for x18; the value register Xd for x18_nosave
    std::uint32_t instruction = 0;  // the instruction itself, for x18 and x18_nosave
    std::uint8_t base = APK_ISLAND_NO_REGISTER;  // x18_nosave only: the second dead register, the TSD base for a write
                                                 // use's store-back; APK_ISLAND_NO_REGISTER for a read use

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
    bool loads_x18 = false;            // a path reads x18 before writing it or leaves it unwritten -> load it first
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
    /// `svc #imm` (imm != 0) that is neither proven data nor a proven Linux syscall: the converter refuses such a
    /// file. Linux takes the syscall number from x8 and ignores the immediate, so a reachable one is a syscall and
    /// goes to `sites` (SiteKind::syscall); a data word that decodes as svc #imm goes to `data_syscalls`.
    std::vector<std::uint64_t> other_syscalls;
    /// `svc #imm` kept verbatim as data: a word in read-only data, or an LDR-literal target. Not rewritten and not
    /// refused; counted in the import report so a later case is visible.
    std::vector<std::uint64_t> data_syscalls;
    /// `mrs xN, TPIDRRO_EL0` (any destination, x18 included). It reads the Darwin TSD base, where the x18
    /// islands keep the guest's x18 (slot 11, ADR-0020). XNU and Linux leave TPIDRRO_EL0 zero at EL0 and Android
    /// userspace never reads it, so Android code has no reason to; a guest that did could name an island's TSD
    /// granule as an exclusive target and clear its monitor, so the converter refuses such a file.
    std::vector<std::uint64_t> tpidrro_reads;
    /// Instructions that read x18, whose value XNU sets to zero at every context switch (F4).
    std::vector<std::uint64_t> x18_reads;
    /// Instructions that write x18 and do not read it.
    std::vector<std::uint64_t> x18_writes;
    /// Stores of x18 in a dump of the whole register file (libunwind's unw_getcontext, hwasan):
    /// stp x18, x19 between stp x16, x17 and stp x20, x21 to the neighbouring slots.
    std::vector<std::uint64_t> x18_saves;
    /// Uses of x18 that no island handles; the converter refuses a file with any of them.
    std::vector<Unsupported> x18_unsupported;
    /// Of the refused uses that sit in a load-exclusive construct (a relocated-or-refused window, or a lone,
    /// unclosed or in-sequence one), how the guest x18 reaches each one, for the ADR-0020 (b)/(c) split:
    /// `x18_defined_in_loop` = x18 is written in the construct before any read, so a context switch that zeroes it
    /// is undone and a retry recomputes it (option (b) could cover it); `x18_live_in` = x18 is live into the
    /// construct, so zeroing corrupts the result and no retry helps (only option (c) could). Counts only -- the
    /// sites are proprietary. A refused use in no exclusive construct (a register branch, the thread pointer through
    /// x18) is in neither count.
    std::size_t x18_defined_in_loop = 0;
    std::size_t x18_live_in = 0;
    /// Load-/store-exclusive windows with x18 inside, each relocated as one island (ADR-0020 X2c).
    std::vector<ExclusiveRegion> exclusive_regions;
    /// Each `br x18` that ends a jump table, run in an island with landing pads (ADR-0020 X2d).
    std::vector<JumpTable> jump_tables;
    /// Load-exclusives of windows that use x18 only as a temporary and run as they are, with x18 in the register
    /// (ADR-0020, the Unity test APK step B): a pass that loses x18 to a context switch fails its store-exclusive.
    std::vector<std::uint64_t> in_place_windows;
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

/// The general-purpose register a whole-function x18 rename (ADR-0020 option c) may use in `code`: the first one
/// unused across `code`, the temporaries x9-x15 first, then -- only when `code` is a leaf (no BL/BLR/BR and no
/// branch whose target leaves `code`) -- the argument registers x2-x7. In a non-leaf the arguments are off limits: a
/// callee, or the target of a tail branch, reads x0-x7, and the function may pass one it never touches through
/// untouched. Never x0 or x1 (a caller reads them after RET as a pass-through return), never x8, never x16/x17
/// (linker call veneers clobber them), never x18, a callee-saved register x19-x28 (an unused one is not saved, so
/// renaming it would clobber the caller's value), x29 or x30. Nothing when every eligible register is used.
std::optional<unsigned> free_caller_saved_register(std::span<const std::byte> code);

/// Whether a direct branch or a computed address from outside the function at offsets [begin, end) in `code` (the
/// executable section, whose first byte is at virtual address `base`) reaches its interior -- any word but `begin`,
/// its only entry (ADR-0020 option c must-have 3): a renamed function loads its register at `begin`, so an entry
/// into the middle would run renamed code with it unset. Checks the direct branches (B, BL, B.cond, CBZ/CBNZ,
/// TBZ/TBNZ), ADR, and an adjacent ADRP+ADD (which needs `base`, as ADRP masks the low 12 bits of the PC). A word
/// that is really data but decodes as one of these only makes it refuse more, which is safe. Indirect branches
/// (BR/BLR) and a non-adjacent ADRP+ADD cannot be seen -- a named limit.
bool has_external_entry(std::span<const std::byte> code, std::uint64_t base, std::size_t begin, std::size_t end);

/// Whether a non-call direct branch inside the function at offsets [begin, end) in `code` targets `begin` (ADR-0020
/// option c). `begin`'s instruction becomes a branch to the entry island, which reloads the rename register from TSD
/// slot 11; a branch back to begin would rerun that reload mid-function and clobber the live value, as stores happen
/// only at exits and calls. A BL to begin is excluded -- it is a call, so its island stores the register first,
/// leaving slot 11 current for the reload. Indirect branches (BR/BLR) cannot be seen -- a named limit.
bool has_internal_branch_to_begin(std::span<const std::byte> code, std::size_t begin, std::size_t end);

/// Rewrites every instruction in `code` that uses x18 to use `reg` instead -- a whole-function rename (ADR-0020
/// option c), where `reg` must be free across the function and not x18. Returns false and leaves `code` untouched
/// when some instruction uses x18 with no register field to redirect (a CASP pair, an ST64B block), so the function
/// cannot be renamed this way.
bool rename_x18(std::span<std::byte> code, unsigned reg);

/// The entry island of a renamed function (ADR-0020 option c). Bytes it needs in the stub area.
std::size_t c_entry_stub_size(std::uint32_t first);

/// Writes the entry island of a renamed function into `stub` (exactly c_entry_stub_size bytes): it loads the guest
/// x18 from TSD slot 11 into `reg` (the register the rename uses for x18 across the function), runs the function's
/// first instruction `first` (already renamed) in the island, and branches back to the instruction after it. A
/// direct-branch `first` keeps its target; a pc-relative `first` (ADR/ADRP/an LDR literal) throws RewriteError, as
/// running it in the island would change its PC -- the gate refuses such a function. Returns the branch that
/// replaces `first` at `begin_address`. `begin_address` and `stub_address` are container addresses.
std::uint32_t emit_c_entry(unsigned reg, std::uint32_t first, std::uint64_t begin_address, std::uint64_t stub_address,
                           std::span<std::byte> stub);

/// An exit island of a renamed function (ADR-0020 option c), for a return or an indirect tail branch. Bytes it
/// needs in the stub area.
std::size_t c_exit_stub_size(std::uint32_t exit);

/// Writes the exit island of a renamed function into `stub` (exactly c_exit_stub_size bytes): it stores `reg` (the
/// register the rename uses for x18) back to TSD slot 11, so the guest's architectural x18 is current when control
/// leaves, then runs the function's original `exit` (a RET, ERET, or an indirect BR) unchanged. x16 holds the TSD
/// base and is preserved around the store with a push/pop, so even a `br x16` is carried through correctly. A direct
/// branch (a tail B or a conditional) has a displacement that must be relocated, which this island does not do, so
/// emit throws RewriteError for one -- those take the conditional/tail-branch exit island. Returns the branch that
/// replaces `exit` at `site_address`. `site_address` and `stub_address` are container addresses.
std::uint32_t emit_c_exit(unsigned reg, std::uint32_t exit, std::uint64_t site_address, std::uint64_t stub_address,
                          std::span<std::byte> stub);

/// A branch exit island of a renamed function (ADR-0020 option c), for a direct branch that leaves the function: an
/// unconditional tail B, or a conditional (B.cond, CBZ/CBNZ, TBZ/TBNZ, CMPBR) whose target is outside it. Bytes it
/// needs in the stub area (more for a conditional, which also carries a fall-through back into the function).
std::size_t c_branch_exit_stub_size(std::uint32_t branch);

/// Writes the branch exit island into `stub` (exactly c_branch_exit_stub_size bytes): it stores `reg` (the register
/// the rename uses for x18) back to TSD slot 11, then takes the function's `branch` to its original target. For an
/// unconditional tail B the store is followed by the relocated branch. For a conditional, the store runs on both
/// paths (harmless when not taken, as `reg` is unchanged), then the conditional -- retargeted two words ahead -- and,
/// not taken, a branch back to the instruction after it; taken, a wide branch to the original target. x16 (the TSD
/// base) is preserved around the store with a push/pop, so the branch sees every register unchanged. Throws
/// RewriteError when `branch` is not a direct branch, or is a BL (a call, not an exit). Returns the branch that
/// replaces `branch` at `site_address`. `site_address` and `stub_address` are container addresses.
std::uint32_t emit_c_branch_exit(unsigned reg, std::uint32_t branch, std::uint64_t site_address,
                                 std::uint64_t stub_address, std::span<std::byte> stub);

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
