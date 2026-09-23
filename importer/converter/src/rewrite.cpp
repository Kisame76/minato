#include "apkconv/rewrite.h"

#include "apkconv/a64.h"

#include <cstring>
#include <format>

namespace apk::rewrite {
namespace {

// Patterns, with the register field (bits 4:0) cleared (Arm ARM, C6.2: MRS, MSR, SVC).
constexpr std::uint32_t register_mask = 0xffff'ffe0;
constexpr std::uint32_t mrs_tpidr_el0 = 0xd53b'd040;
constexpr std::uint32_t msr_tpidr_el0 = 0xd51b'd040;
constexpr std::uint32_t svc_mask = 0xffe0'001f;  // keeps the 16-bit immediate out
constexpr std::uint32_t svc = 0xd400'0001;

constexpr std::uint32_t nop = 0xd503'201f;
constexpr unsigned zero_register = 31;  // xzr as a data register, sp as a base register
constexpr unsigned sp = 31;
constexpr unsigned x16 = 16;
constexpr unsigned x17 = 17;
constexpr unsigned x30 = 30;

// Sizes of the stubs, in instructions.
constexpr std::size_t read_island_words = 4;   // mrs, bic, ldr, b
constexpr std::size_t write_island_words = 6;  // str, mrs, bic, str, ldr, b
constexpr std::size_t syscall_veneer_words = 6;  // stp, adrp, ldr, blr, ldp, b

// --- Encoders (Arm ARM, C6.2), for exactly the forms the stubs use -------------------------------

std::uint32_t reg(unsigned number, unsigned shift) {
    return static_cast<std::uint32_t>(number & 0x1f) << shift;
}

/// B: PC-relative, ±128 MiB.
std::uint32_t b(std::uint64_t from, std::uint64_t to) {
    const auto delta = static_cast<std::int64_t>(to - from);
    if (delta < -(std::int64_t{1} << 27) || delta >= (std::int64_t{1} << 27)) {
        throw RewriteError(std::format("a branch from {:#x} to {:#x} is beyond 128 MiB", from, to));
    }
    return 0x1400'0000 | (static_cast<std::uint32_t>(delta >> 2) & 0x03ff'ffff);
}

/// ADRP: the 4 KiB page of `to`, relative to the 4 KiB page of `from`, ±4 GiB. (The unit is 4 KiB
/// on every system, whatever its page size.)
std::uint32_t adrp(unsigned rd, std::uint64_t from, std::uint64_t to) {
    const auto delta = static_cast<std::int64_t>((to & ~std::uint64_t{0xfff}) - (from & ~std::uint64_t{0xfff})) >> 12;
    if (delta < -(std::int64_t{1} << 20) || delta >= (std::int64_t{1} << 20)) {
        throw RewriteError(std::format("the syscall slot at {:#x} is beyond 4 GiB of {:#x}", to, from));
    }
    const auto immediate = static_cast<std::uint32_t>(delta) & 0x1f'ffff;
    return 0x9000'0000 | ((immediate & 0x3) << 29) | ((immediate >> 2) << 5) | reg(rd, 0);
}

/// LDR/STR Xt, [Xn, #offset], unsigned offset scaled by 8.
std::uint32_t load_store_unsigned(std::uint32_t opcode, unsigned rt, unsigned rn, std::uint64_t offset) {
    if (offset % 8 != 0 || offset / 8 > 0xfff) {
        throw RewriteError(std::format("offset {:#x} is not an 8-byte multiple below 32 KiB", offset));
    }
    return opcode | (static_cast<std::uint32_t>(offset / 8) << 10) | reg(rn, 5) | reg(rt, 0);
}

std::uint32_t ldr_unsigned(unsigned rt, unsigned rn, std::uint64_t offset) {
    return load_store_unsigned(0xf940'0000, rt, rn, offset);
}

std::uint32_t str_unsigned(unsigned rt, unsigned rn, std::uint64_t offset) {
    return load_store_unsigned(0xf900'0000, rt, rn, offset);
}

/// STR Xt, [Xn, #offset]! and LDR Xt, [Xn], #offset: 9-bit signed byte offset.
std::uint32_t str_pre_index(unsigned rt, unsigned rn, std::int32_t offset) {
    return 0xf800'0c00 | ((static_cast<std::uint32_t>(offset) & 0x1ff) << 12) | reg(rn, 5) | reg(rt, 0);
}

std::uint32_t ldr_post_index(unsigned rt, unsigned rn, std::int32_t offset) {
    return 0xf840'0400 | ((static_cast<std::uint32_t>(offset) & 0x1ff) << 12) | reg(rn, 5) | reg(rt, 0);
}

/// STP Xt1, Xt2, [Xn, #offset]! and LDP Xt1, Xt2, [Xn], #offset: 7-bit signed offset scaled by 8.
std::uint32_t stp_pre_index(unsigned rt, unsigned rt2, unsigned rn, std::int32_t offset) {
    return 0xa980'0000 | ((static_cast<std::uint32_t>(offset / 8) & 0x7f) << 15) | reg(rt2, 10) | reg(rn, 5) |
           reg(rt, 0);
}

std::uint32_t ldp_post_index(unsigned rt, unsigned rt2, unsigned rn, std::int32_t offset) {
    return 0xa8c0'0000 | ((static_cast<std::uint32_t>(offset / 8) & 0x7f) << 15) | reg(rt2, 10) | reg(rn, 5) |
           reg(rt, 0);
}

std::uint32_t blr(unsigned rn) {
    return 0xd63f'0000 | reg(rn, 5);
}

std::uint32_t mrs_tpidrro_el0(unsigned rt) {
    return 0xd53b'd060 | reg(rt, 0);
}

/// BIC Xd, Xn, #7, which is AND Xd, Xn, #~7: the low bits of TPIDRRO_EL0 are not part of the TSD
/// base, libsyscall masks them the same way. Logical immediate N=1, immr=61, imms=60.
std::uint32_t bic_7(unsigned rd, unsigned rn) {
    return 0x9240'0000 | (61u << 16) | (60u << 10) | reg(rn, 5) | reg(rd, 0);
}

/// Appends instructions to a stub and checks it ends up exactly as large as stub_size() says.
class StubWriter {
public:
    StubWriter(std::span<std::byte> stub, std::uint64_t address) : stub_(stub), address_(address) {}

    std::uint64_t next_address() const { return address_ + 4 * count_; }

    void operator()(std::uint32_t instruction) {
        if (4 * (count_ + 1) > stub_.size()) throw RewriteError("the stub buffer is too small");
        std::memcpy(stub_.data() + 4 * count_, &instruction, sizeof instruction);
        ++count_;
    }

    void finish() const {
        if (4 * count_ != stub_.size()) throw RewriteError("the stub buffer has the wrong size");
    }

private:
    std::span<std::byte> stub_;
    std::uint64_t address_;
    std::size_t count_ = 0;
};

constexpr std::uint32_t x18 = 1u << 18;

std::uint32_t word_at(std::span<const std::byte> code, std::size_t offset) {
    std::uint32_t word;
    std::memcpy(&word, code.data() + offset, sizeof word);
    return word;
}

/// STP Xt, Xt+1, [Xn|SP, #8 * imm7] (signed offset), for the register dumps below.
std::uint32_t stp_pair(unsigned first, unsigned base, std::int32_t imm7) {
    return 0xa900'0000 | ((static_cast<std::uint32_t>(imm7) & 0x7f) << 15) | reg(first + 1, 10) | reg(base, 5) |
           reg(first, 0);
}

/// Whether the word at `offset` stores x18 as part of a dump of the whole register file:
/// stp x18, x19 with stp x16, x17 at 16 bytes below and stp x20, x21 at 16 bytes above as its
/// neighbours, in either order. libunwind's unw_getcontext and hwasan's error path do this. The
/// value only ever goes back into x18 (unw_resume) or into a report, so the read is harmless.
bool is_register_dump(std::span<const std::byte> code, std::size_t offset) {
    if (offset < 4 || offset + 8 > code.size()) return false;
    const auto word = word_at(code, offset);
    if ((word & 0xffc0'7c1f) != 0xa900'4c12) return false;  // stp x18, x19, [Xn|SP, #imm]
    const auto base = (word >> 5) & 0x1f;
    const auto imm7 = static_cast<std::int32_t>(word << 10) >> 25;
    if (imm7 < -62 || imm7 > 61) return false;  // the neighbours must not wrap around
    const auto below = stp_pair(16, base, imm7 - 2), above = stp_pair(20, base, imm7 + 2);
    const auto before = word_at(code, offset - 4), after = word_at(code, offset + 4);
    return (before == below && after == above) || (before == above && after == below);
}

}  // namespace

void scan(std::span<const std::byte> code, std::uint64_t address, Findings &findings) {
    if (address % 4 != 0 || code.size() % 4 != 0) {
        throw RewriteError(std::format("code at {:#x} with {:#x} bytes is not word aligned", address, code.size()));
    }
    for (std::size_t offset = 0; offset < code.size(); offset += 4) {
        std::uint32_t instruction;
        std::memcpy(&instruction, code.data() + offset, sizeof instruction);
        const std::uint64_t at = address + offset;
        const auto rt = static_cast<std::uint8_t>(instruction & 0x1f);

        if ((instruction & register_mask) == mrs_tpidr_el0) {
            findings.sites.push_back({at, SiteKind::thread_pointer_read, rt});
        } else if ((instruction & register_mask) == msr_tpidr_el0) {
            findings.sites.push_back({at, SiteKind::thread_pointer_write, rt});
        } else if ((instruction & svc_mask) == svc) {
            if (instruction == svc) {
                findings.sites.push_back({at, SiteKind::syscall, 0});
            } else {
                findings.other_syscalls.push_back(at);
            }
        }

        const auto use = a64::register_use(instruction);
        if (use.reads & x18) {
            (is_register_dump(code, offset) ? findings.x18_saves : findings.x18_reads).push_back(at);
        } else if (use.writes & x18) {
            findings.x18_writes.push_back(at);
        }
    }
}

std::size_t stub_size(const Site &site) {
    switch (site.kind) {
        case SiteKind::thread_pointer_read: return site.reg == zero_register ? 0 : 4 * read_island_words;
        case SiteKind::thread_pointer_write: return 4 * write_island_words;
        case SiteKind::syscall: return 4 * syscall_veneer_words;
    }
    throw RewriteError("unknown site kind");
}

std::uint32_t emit(const Site &site, std::uint64_t site_address, std::uint64_t stub_address,
                   std::uint64_t syscall_slot_address, std::span<std::byte> stub) {
    if (stub.size() != stub_size(site)) throw RewriteError("the stub buffer has the wrong size");
    StubWriter write(stub, stub_address);
    const std::uint64_t back = site_address + 4;

    switch (site.kind) {
        case SiteKind::thread_pointer_read:
            // A read into xzr has no effect at all, so it needs no island.
            if (site.reg == zero_register) return nop;
            write(mrs_tpidrro_el0(site.reg));
            write(bic_7(site.reg, site.reg));
            write(ldr_unsigned(site.reg, site.reg, guest_thread_pointer_offset));
            break;

        case SiteKind::thread_pointer_write: {
            // One scratch register, saved below the stack pointer. AArch64 has no red zone and keeps
            // sp 16-byte aligned, so a pre-indexed store of 16 bytes is always allowed.
            const unsigned scratch = site.reg == x16 ? x17 : x16;
            write(str_pre_index(scratch, sp, -16));
            write(mrs_tpidrro_el0(scratch));
            write(bic_7(scratch, scratch));
            write(str_unsigned(site.reg, scratch, guest_thread_pointer_offset));
            write(ldr_post_index(scratch, sp, 16));
            break;
        }

        case SiteKind::syscall: {
            // x16 and x30 are the only registers the veneer touches, so they are the only ones it
            // saves; the syscall entry of the runtime keeps everything else, flags included.
            if (syscall_slot_address % 8 != 0) {
                throw RewriteError(std::format("the syscall slot at {:#x} is not 8-byte aligned", syscall_slot_address));
            }
            write(stp_pre_index(x16, x30, sp, -16));
            write(adrp(x16, write.next_address(), syscall_slot_address));
            write(ldr_unsigned(x16, x16, syscall_slot_address & 0xfff));
            write(blr(x16));
            write(ldp_post_index(x16, x30, sp, 16));
            break;
        }
    }
    write(b(write.next_address(), back));
    write.finish();
    return b(site_address, stub_address);
}

}  // namespace apk::rewrite
