// The rewriter: finds the instructions that cannot run unchanged on XNU and replaces each with a
// branch to a stub in the container (ADR-0010, ADR-0019). Only these exact patterns are touched:
//
//   mrs xN, TPIDR_EL0   XNU overwrites TPIDR_EL0 (F3). The island loads the guest thread pointer
//                       from Darwin TSD slot 6 instead: mrs xN, TPIDRRO_EL0; bic; ldr [xN, #48].
//   msr TPIDR_EL0, xN   Same slot, written; the island saves one scratch register on the stack.
//   svc #0              XNU takes the syscall number from x16, Linux from x8 (F5). The veneer calls
//                       the Linux syscall layer through a slot the loader fills in.
//
// Every other instruction stays as it is: no emulation, no trapping (ADR-0001).
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace apk::rewrite {

enum class SiteKind : std::uint8_t {
    thread_pointer_read,   // mrs xN, TPIDR_EL0
    thread_pointer_write,  // msr TPIDR_EL0, xN
    syscall,               // svc #0
};

struct Site {
    std::uint64_t address;  // of the instruction, in the address space given to scan()
    SiteKind kind;
    std::uint8_t reg;  // N of xN, 31 for xzr; 0 for syscalls

    bool operator==(const Site &) const = default;
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
};

class RewriteError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Offset of the guest thread pointer from the Darwin TSD base: slot 6 (ADR-0019), the same value
/// as APKRT_GUEST_TLS_OFFSET in the runtime.
inline constexpr std::uint32_t guest_thread_pointer_offset = 8 * 6;

/// Scans the instructions in `code`, which starts at `address`, and appends what it finds.
void scan(std::span<const std::byte> code, std::uint64_t address, Findings &findings);

/// Bytes the stub of `site` needs in the stub area; 0 when the site is replaced in place.
std::size_t stub_size(const Site &site);

/// Writes the stub of `site` into `stub` (exactly stub_size(site) bytes) and returns the
/// instruction that replaces the site. The addresses are all in the container: where the site is,
/// where its stub goes, and where the loader puts the address of the syscall entry.
std::uint32_t emit(const Site &site, std::uint64_t site_address, std::uint64_t stub_address,
                   std::uint64_t syscall_slot_address, std::span<std::byte> stub);

}  // namespace apk::rewrite
