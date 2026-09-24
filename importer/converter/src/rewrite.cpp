#include "apkconv/rewrite.h"

#include "apkconv/a64.h"

#include <algorithm>
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
constexpr std::size_t branch_island_words = 9;   // save, mrs, bic, ldr, branch, restore+b (twice)

/// CBZ, CBNZ (bits 30:25 = 011010) and TBZ, TBNZ (011011): a conditional branch that tests one
/// register (Rt, bits 4:0) and has a taken and a not-taken exit. The rest of the branch group
/// takes no register (B, B.cond) or one no plain island can follow (BR, BLR, RET, CMPBR).
constexpr bool is_conditional_branch(std::uint32_t instruction) {
    const auto kind = (instruction >> 25) & 0x3f;
    return kind == 0b011010 || kind == 0b011011;
}

constexpr bool is_test_bit_branch(std::uint32_t instruction) {
    return ((instruction >> 25) & 0x3f) == 0b011011;  // TBZ, TBNZ: a 14-bit displacement, not 19
}

/// The byte displacement a conditional branch encodes, sign-extended (imm19 or imm14, times 4).
std::int64_t conditional_branch_displacement(std::uint32_t instruction) {
    if (is_test_bit_branch(instruction)) {
        return static_cast<std::int64_t>(static_cast<std::int32_t>(instruction << 13) >> 18) * 4;
    }
    return static_cast<std::int64_t>(static_cast<std::int32_t>(instruction << 8) >> 13) * 4;
}

/// `instruction` (a conditional branch) with its displacement set to `delta` bytes.
std::uint32_t with_branch_displacement(std::uint32_t instruction, std::int64_t delta) {
    const auto encoded = static_cast<std::uint32_t>(delta / 4);
    if (is_test_bit_branch(instruction)) {
        return (instruction & ~(0x3fffu << 5)) | ((encoded & 0x3fff) << 5);
    }
    return (instruction & ~(0x7ffffu << 5)) | ((encoded & 0x7ffff) << 5);
}

/// Words an island needs to compute a PC-relative instruction's address for its own place: ADRP
/// (1), ADR as ADRP and ADD (2), a load from a literal as ADRP, ADD and the load (3). ADR and the
/// literal loads reach only 1 MiB, and the islands lie before the image. 0 for anything else.
constexpr std::size_t pc_relative_words(std::uint32_t instruction) {
    if ((instruction & 0x9f00'0000) == 0x9000'0000) return 1;  // ADRP
    if ((instruction & 0x9f00'0000) == 0x1000'0000) return 2;  // ADR
    // LDR W, LDR X, LDRSW (literal) by opc, bits 31:30; opc 11 is PRFM, which names no register
    if ((instruction & 0x3f00'0000) == 0x1800'0000 && (instruction >> 30) != 0b11) return 3;
    return 0;
}

/// The address a PC-relative instruction at `at` computes: the page for ADRP, the byte for ADR,
/// the literal for a load.
std::uint64_t pc_relative_target(std::uint32_t instruction, std::uint64_t at) {
    if (pc_relative_words(instruction) == 3) {
        const auto imm19 = static_cast<std::int32_t>(instruction << 8) >> 13;  // bits 23:5
        return at + static_cast<std::uint64_t>(static_cast<std::int64_t>(imm19) * 4);
    }
    const auto imm21 = (((instruction >> 5) & 0x7'ffff) << 2) | ((instruction >> 29) & 0x3);  // immhi:immlo
    const auto imm = static_cast<std::int64_t>(static_cast<std::int32_t>(imm21 << 11) >> 11);
    if (pc_relative_words(instruction) == 1) return (at & ~std::uint64_t{0xfff}) + static_cast<std::uint64_t>(imm * 4096);
    return at + static_cast<std::uint64_t>(imm);
}

// An x18 island: save the value scratch, optionally the base scratch, read x18 from the TSD when
// the instruction reads it, run it, write x18 back when it writes it, restore the scratch, branch.
// A PC-relative instruction becomes one to three words; a conditional branch gets a two-way island
// instead (see emit).
std::size_t x18_island_words(std::uint32_t instruction) {
    if (is_conditional_branch(instruction)) return branch_island_words;
    const auto use = a64::register_use(instruction);
    const bool reads = (use.reads & (1u << 18)) != 0, writes = (use.writes & (1u << 18)) != 0;
    const auto body = std::max<std::size_t>(pc_relative_words(instruction), 1);
    std::size_t words = 1 /* save value */ + body + 1 /* restore value */ + 1 /* branch */;
    if (reads) words += 3;   // mrs, bic, ldr
    if (writes) words += 5;  // save base, mrs, bic, str, restore base
    return words;
}

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
        throw RewriteError(std::format("the page of {:#x} is beyond 4 GiB of {:#x}", to, from));
    }
    const auto immediate = static_cast<std::uint32_t>(delta) & 0x1f'ffff;
    return 0x9000'0000 | ((immediate & 0x3) << 29) | ((immediate >> 2) << 5) | reg(rd, 0);
}

/// ADD Xd, Xn, #imm12, for the offset within the page that ADRP leaves out.
std::uint32_t add_immediate(unsigned rd, unsigned rn, std::uint64_t imm12) {
    return 0x9100'0000 | (static_cast<std::uint32_t>(imm12 & 0xfff) << 10) | reg(rn, 5) | reg(rd, 0);
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

/// STUR/LDUR Xt, [Xn, #imm9]: unscaled signed offset, no writeback, so sp does not move.
std::uint32_t stur(unsigned rt, unsigned rn, std::int32_t offset) {
    return 0xf800'0000 | ((static_cast<std::uint32_t>(offset) & 0x1ff) << 12) | reg(rn, 5) | reg(rt, 0);
}

std::uint32_t ldur(unsigned rt, unsigned rn, std::int32_t offset) {
    return 0xf840'0000 | ((static_cast<std::uint32_t>(offset) & 0x1ff) << 12) | reg(rn, 5) | reg(rt, 0);
}

std::uint32_t mrs_tpidrro_el0(unsigned rt) {
    return 0xd53b'd060 | reg(rt, 0);
}

/// SUB Xd, Xn, Xm.
std::uint32_t sub_register(unsigned rd, unsigned rn, unsigned rm) {
    return 0xcb00'0000 | reg(rm, 16) | reg(rn, 5) | reg(rd, 0);
}

/// ADD Xd, Xn, Xm, LSL #shift.
std::uint32_t add_shifted(unsigned rd, unsigned rn, unsigned rm, unsigned shift) {
    return 0x8b00'0000 | reg(rm, 16) | ((shift & 0x3f) << 10) | reg(rn, 5) | reg(rd, 0);
}

/// AND Xd, Xn, #mask, the mask with bits 0, 1 and `bits` to 63 set: nonzero exactly for an offset
/// that is negative, at least 2^bits or not a multiple of 4. Its ones form one run that wraps from
/// bit 63 to bit 0, 66 - bits of them, rotated right by 64 - bits (logical immediate with N = 1).
std::uint32_t and_outside_span(unsigned rd, unsigned rn, unsigned bits) {
    return 0x9240'0000 | ((64 - bits) << 16) | ((65 - bits) << 10) | reg(rn, 5) | reg(rd, 0);
}

/// The byte distance from `from` to `to` for a branch or ADR that reaches ±1 MiB.
std::int64_t near_distance(std::uint64_t from, std::uint64_t to) {
    const auto delta = static_cast<std::int64_t>(to - from);
    if (delta < -(std::int64_t{1} << 20) || delta >= (std::int64_t{1} << 20)) {
        throw RewriteError(std::format("{:#x} is beyond 1 MiB of {:#x}", to, from));
    }
    return delta;
}

/// CBNZ Xt: PC-relative, ±1 MiB.
std::uint32_t cbnz(unsigned rt, std::uint64_t from, std::uint64_t to) {
    return 0xb500'0000 | ((static_cast<std::uint32_t>(near_distance(from, to) >> 2) & 0x7'ffff) << 5) | reg(rt, 0);
}

/// ADR Xd: PC-relative, ±1 MiB.
std::uint32_t adr(unsigned rd, std::uint64_t from, std::uint64_t to) {
    const auto immediate = static_cast<std::uint32_t>(near_distance(from, to)) & 0x1f'ffff;
    return 0x1000'0000 | ((immediate & 0x3) << 29) | ((immediate >> 2) << 5) | reg(rd, 0);
}

std::uint32_t br(unsigned rn) {
    return 0xd61f'0000 | reg(rn, 5);
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

constexpr unsigned guest_x18 = 18;
constexpr std::uint32_t x18_bit = 1u << guest_x18;
// The two scratch slots in the red zone: 128 bytes below sp, which XNU keeps free on a signal
// (C_64_REDZONE_LEN) and Linux code never uses (ADR-0020).
constexpr std::int32_t scratch_slot_0 = -128;
constexpr std::int32_t scratch_slot_1 = -120;

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

/// The two registers of an x18 island: `value` stands in for x18, `base` holds the TSD base when
/// the new value is stored. Both are the lowest of x0 to x28 the instruction does not use; the
/// frame pointer and the link register stay out, unwinders read them. No instruction names so many
/// registers that two are not free, so `found` is only a guard.
struct Scratch {
    unsigned value = 0;
    unsigned base = 0;
    bool found = false;
};

Scratch scratch_registers(std::uint32_t instruction) {
    const auto use = a64::register_use(instruction);
    const std::uint32_t taken = use.reads | use.writes | x18_bit;
    Scratch scratch;
    unsigned count = 0;
    for (unsigned candidate = 0; candidate < 29 && count < 2; ++candidate) {
        if ((taken & (1u << candidate)) == 0) (count++ == 0 ? scratch.value : scratch.base) = candidate;
    }
    scratch.found = count == 2;
    return scratch;
}

// --- Forms of x18 use that an island cannot run (ADR-0020), by encoding (Arm ARM, C4.1) ---------

constexpr bool bit(std::uint32_t word, unsigned number) {
    return ((word >> number) & 1) != 0;
}

constexpr unsigned rd_field(std::uint32_t word) { return word & 0x1f; }
constexpr unsigned rn_field(std::uint32_t word) { return (word >> 5) & 0x1f; }

/// Loads and stores (op1 = x1x0). Their base register is Rn, where 31 always means sp.
constexpr bool is_load_store(std::uint32_t word) {
    return (word & 0x0a00'0000) == 0x0800'0000;
}

/// The group op1 = 101x without the system instructions (bits 31:24 = 0xd5): of the rest, only
/// CBZ, CBNZ, TBZ, TBNZ, CB<cc> and the branches to a register can use x18.
constexpr bool is_branch(std::uint32_t word) {
    return (word & 0x1c00'0000) == 0x1400'0000 && (word >> 24) != 0xd5;
}

/// LDXR, STXR and their acquire, release and pair forms (bits 29:23 = 0010000, where o1 set with
/// bit 31 clear is CASP instead), and LDTXR, STTXR (LSUI, bits 29:23 = 0010010).
constexpr bool is_exclusive(std::uint32_t word) {
    if ((word & 0x3f80'0000) == 0x0800'0000) return !bit(word, 21) || bit(word, 31);
    return (word & 0x3f80'0000) == 0x0900'0000;
}

constexpr bool is_load_exclusive(std::uint32_t word) {
    return is_exclusive(word) && bit(word, 22);
}

/// CLREX #imm: gives up the exclusive monitor, and with it the LL/SC sequence.
constexpr bool is_clrex(std::uint32_t word) {
    return (word & 0xffff'f0ff) == 0xd503'305f;
}

/// Whether a load or store writes its base register back (pre- and post-index forms). The decoder
/// knows every such form but leaves register 31 out, so the base is renamed to a free one first.
bool writes_back_base(std::uint32_t word) {
    const auto use = a64::register_use(word);
    unsigned spare = 0;
    while (((use.reads | use.writes) & (1u << spare)) != 0) ++spare;
    const auto renamed = a64::register_use((word & ~(0x1fu << 5)) | (spare << 5));
    return (renamed.writes & (1u << spare)) != 0;
}

/// Whether `word` moves sp. Register 31 is sp in some forms and xzr in others (Arm ARM C6.2), and
/// the decoder leaves it out. sp is written by these forms with Rd = 31: ADD and SUB (immediate,
/// extended register) without S, ADDG, SUBG, AND, ORR, EOR (immediate), IRG, ADDPT, SUBPT; and by
/// the loads and stores that write back to sp as their base.
bool writes_stack_pointer(std::uint32_t word) {
    if (is_load_store(word)) return rn_field(word) == sp && writes_back_base(word);
    if (rd_field(word) != sp) return false;
    if ((word & 0x1f00'0000) == 0x1100'0000) {  // ADD, SUB (immediate), ADDG, SUBG; not SMAX and the like (CSSC)
        return !bit(word, 29) && (word & 0x00c0'0000) != 0x00c0'0000;
    }
    if ((word & 0x1f20'0000) == 0x0b20'0000) return !bit(word, 29);                   // ADD, SUB (extended register)
    if ((word & 0x1f80'0000) == 0x1200'0000) return (word & 0x6000'0000) != 0x6000'0000;  // not ANDS
    return (word & 0xffe0'fc00) == 0x9ac0'1000 || (word & 0xbfe0'e000) == 0x9a00'2000;  // IRG; ADDPT, SUBPT
}

/// Whether a load or store with base sp reaches below it, without writeback: the forms with a
/// signed offset, when it is negative. There the island keeps its registers; Linux code never goes
/// there, it has no red zone.
bool reaches_below_stack_pointer(std::uint32_t word) {
    if (!is_load_store(word) || rn_field(word) != sp) return false;
    if ((word & 0x3b20'0400) == 0x3800'0000) return bit(word, 20);  // STUR, LDUR, STTR, LDTR and the like (imm9)
    if ((word & 0x3b20'0400) == 0x3820'0400) return bit(word, 22);  // LDRAA, LDRAB (S:imm9)
    if ((word & 0x3880'0000) == 0x2800'0000) return bit(word, 21);  // STP, LDP, STNP, LDNP, STGP (imm7)
    if ((word & 0x3f20'0c00) == 0x1900'0000) return bit(word, 20);  // STLUR, LDAPUR (imm9)
    if ((word & 0xff20'0000) == 0xd920'0000) return bit(word, 20);  // STG, LDG and the other tag forms (imm9)
    return false;
}

constexpr const char *inside_exclusive_sequence =
    "it lies between a load- and a store-exclusive, where the island's memory accesses could break the sequence";

/// Why an x18 instruction cannot run in an island, or nullptr when it can. The island saves two
/// scratch registers in the red zone, loads the guest x18 from the TSD, runs the instruction with
/// the value register in place of x18, stores the value back and branches to the next instruction;
/// a conditional branch leaves by two exits, a PC-relative form is computed for the island's place.
/// That leaves out: the thread pointer moved through x18, register branches and compare-and-branch,
/// exclusives (scan() adds what lies between them), sp moved or addressed below, and x18 without a
/// register field of its own.
const char *unsupported_reason(std::uint32_t instruction) {
    const auto system_register = instruction & register_mask;
    if (system_register == mrs_tpidr_el0 || system_register == msr_tpidr_el0) {
        return "it moves the thread pointer through x18";
    }
    if (is_branch(instruction) && !is_conditional_branch(instruction)) {
        return "it is a register branch or a compare-and-branch, which needs a landing pad the island has not yet";
    }
    if (is_exclusive(instruction)) {
        return "it is an exclusive access, and the island's memory accesses could break its LL/SC sequence";
    }
    if (writes_stack_pointer(instruction)) {
        return "it moves the stack pointer, below which the island keeps its registers";
    }
    if (reaches_below_stack_pointer(instruction)) {
        return "it reaches below the stack pointer, where the island keeps its registers";
    }
    const auto scratch = scratch_registers(instruction);
    if (!scratch.found) return "it leaves no free scratch register";
    if (!a64::substitute(instruction, guest_x18, scratch.value)) return "x18 has no register field of its own in it";
    return nullptr;
}

/// Whether the word at `offset` stores x18 as part of a dump of the whole register file:
/// stp x18, x19 with stp x16, x17 at 16 bytes below and stp x20, x21 at 16 bytes above as its
/// neighbours, in either order. libunwind's unw_getcontext and hwasan's error path do this. The
/// value only ever goes back into x18 (unw_resume) or into a report; the store runs in an island
/// like any other use and is only counted apart.
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

constexpr bool is_store_exclusive(std::uint32_t word) {
    return is_exclusive(word) && !is_load_exclusive(word);
}

/// The signed byte displacement of any PC-relative branch, or nullopt when `word` is none: B, BL
/// (imm26), B.cond, BC.cond (imm19), and the conditional branches CBZ, CBNZ, TBZ, TBNZ.
std::optional<std::int64_t> branch_displacement(std::uint32_t word) {
    if ((word & 0x7c00'0000) == 0x1400'0000) {  // B, BL
        return static_cast<std::int64_t>(static_cast<std::int32_t>(word << 6) >> 6) * 4;
    }
    if (((word >> 24) & 0xff) == 0x54) {  // B.cond, BC.cond
        return static_cast<std::int64_t>(static_cast<std::int32_t>(word << 8) >> 13) * 4;
    }
    if (is_conditional_branch(word)) return conditional_branch_displacement(word);
    return std::nullopt;
}

/// The end offset (inclusive) of the exclusive window that opens at `open` (a load-exclusive): the
/// first store-exclusive, plus a CLREX right behind it, or the first CLREX. Nullopt when the window
/// nests another load-exclusive or finds no close within a small cap.
std::optional<std::size_t> exclusive_window_end(std::span<const std::byte> code, std::size_t open) {
    constexpr std::size_t cap = 32 * 4;
    for (std::size_t j = open + 4; j < code.size() && j - open <= cap; j += 4) {
        const auto word = word_at(code, j);
        if (is_load_exclusive(word)) return std::nullopt;
        if (is_store_exclusive(word)) {
            return (j + 4 < code.size() && is_clrex(word_at(code, j + 4))) ? j + 4 : j;
        }
        if (is_clrex(word)) return j;
    }
    return std::nullopt;
}

/// Builds the relocatable region for the window [open, end] when it is simple enough (ADR-0020
/// X2c): it uses x18, keeps a scratch register free, moves the stack pointer nowhere, substitutes
/// x18 in every word that uses it, and its branches stay inside the window when x18 is written (so
/// the store-back after the window is never skipped). Nullopt otherwise, so the x18 uses inside are
/// refused instead.
std::optional<ExclusiveRegion> build_exclusive_region(std::span<const std::byte> code, std::uint64_t address,
                                                      std::size_t open, std::size_t end) {
    ExclusiveRegion region;
    region.address = address + open;
    std::uint32_t used = 0;
    bool any_x18 = false, seen_x18 = false;
    for (std::size_t j = open; j <= end; j += 4) {
        const auto word = word_at(code, j);
        if (writes_stack_pointer(word)) return std::nullopt;
        const auto use = a64::register_use(word);
        used |= use.reads | use.writes;
        if ((use.reads | use.writes) & x18_bit) {
            if (!seen_x18) region.reads_x18 = (use.reads & x18_bit) != 0;
            seen_x18 = true;
            any_x18 = true;
            if (use.writes & x18_bit) region.writes_x18 = true;
        }
        region.words.push_back(word);
    }
    if (!any_x18) return std::nullopt;  // an exclusive loop without x18 needs no rewrite

    // A branch that leaves the window must not skip the store-back, so refuse one when x18 is written.
    for (std::size_t k = 0; region.writes_x18 && k < region.words.size(); ++k) {
        if (const auto displacement = branch_displacement(region.words[k])) {
            const auto target = static_cast<std::int64_t>(open + 4 * k) + *displacement;
            if (target < static_cast<std::int64_t>(open) || target > static_cast<std::int64_t>(end)) {
                return std::nullopt;
            }
        }
    }

    // Two free registers: the value in place of x18, and the TSD base for the store-back. Both may
    // hold live values around the loop, so the island saves them in the red zone.
    unsigned found = 0;
    for (unsigned candidate = 0; candidate < 29 && found < 2; ++candidate) {
        if (candidate == guest_x18 || (used & (1u << candidate)) != 0) continue;
        (found++ == 0 ? region.scratch : region.base) = static_cast<std::uint8_t>(candidate);
    }
    if (found < 2) return std::nullopt;  // not enough free registers
    for (const auto word : region.words) {
        const auto use = a64::register_use(word);
        if (((use.reads | use.writes) & x18_bit) && !a64::substitute(word, guest_x18, region.scratch)) {
            return std::nullopt;
        }
    }
    return region;
}

/// Whether a direct branch anywhere in `code` lands strictly inside the window (open, end], which
/// would enter the relocated island in the middle. Indirect branches cannot be checked; the
/// compiler-built LL/SC loops of the test APK have none into their own body (ADR-0020 X2c).
bool branch_enters_window(std::span<const std::byte> code, std::size_t open, std::size_t end) {
    for (std::size_t j = 0; j + 4 <= code.size(); j += 4) {
        if (j > open && j <= end) continue;  // branches inside the window are its own business
        if (const auto displacement = branch_displacement(word_at(code, j))) {
            const auto target = static_cast<std::int64_t>(j) + *displacement;
            if (target > static_cast<std::int64_t>(open) && target <= static_cast<std::int64_t>(end)) return true;
        }
    }
    return false;
}

// --- Jump tables through x18 (ADR-0020 X2d) ------------------------------------------------------

constexpr std::uint32_t br_x18 = 0xd61f'0240;
constexpr std::uint32_t jump_table_trap = 0xd420'0300;  // BRK #0x18: a target without a landing pad
constexpr std::uint64_t max_jump_table_span = 64 * 1024;

// The island (see emit_jump_table): 13 words up to its BR, then the trap of 3; each landing pad has 4.
constexpr std::size_t jump_trap_word = 13;
constexpr std::size_t jump_island_words = 16;
constexpr std::size_t landing_pad_words = 4;

/// B.cond (not BC.cond): the condition in bits 3:0.
constexpr bool is_branch_on_flags(std::uint32_t word) {
    return (word & 0xff00'0010) == 0x5400'0000;
}
constexpr unsigned condition_hs = 0b0010;  // unsigned >=, also named CS
constexpr unsigned condition_hi = 0b1000;  // unsigned >

/// Whether `word` leaves NZCV alone, of the forms a compiler puts between a bounds check and its
/// branch: ADD and SUB (immediate, shifted or extended register) and the logical operations
/// without S, the wide moves, ADR, ADRP, loads and stores. Anything else counts as setting them.
bool leaves_flags(std::uint32_t word) {
    if (is_load_store(word)) return true;
    const auto operation = (word >> 29) & 0x3;
    if ((word & 0x1f00'0000) == 0x1100'0000 || (word & 0x1f00'0000) == 0x0b00'0000) return !bit(word, 29);
    if ((word & 0x1f00'0000) == 0x0a00'0000 || (word & 0x1f80'0000) == 0x1200'0000) return operation != 0b11;  // not ANDS
    if ((word & 0x1f80'0000) == 0x1280'0000) return true;  // MOVN, MOVZ, MOVK
    return (word & 0x1f00'0000) == 0x1000'0000;           // ADR, ADRP
}

struct BoundsCheck {
    std::size_t compare;    // offset of the CMP
    std::uint32_t entries;  // table entries the index can reach
};

/// The bounds check of the index register before the table load at `load`: CMP wI|xI, #n, then
/// B.HI (n + 1 entries) or B.HS (n). Between the CMP and the branch only words that leave the flags
/// and the index alone; between the branch and the load none that writes the index.
std::optional<BoundsCheck> bounds_check(std::span<const std::byte> code, std::size_t load, unsigned index) {
    constexpr std::size_t reach = 16 * 4;
    std::optional<unsigned> condition;
    for (std::size_t offset = load; offset >= 4 && load - offset < reach;) {
        offset -= 4;
        const auto word = word_at(code, offset);
        const bool writes_index = (a64::register_use(word).writes & (1u << index)) != 0;
        if (!condition) {
            if (is_branch_on_flags(word)) {
                condition = word & 0xf;
                if (*condition != condition_hi && *condition != condition_hs) return std::nullopt;
            } else if (writes_index) {
                return std::nullopt;
            }
            continue;
        }
        if ((word & 0x7fc0'001f) == 0x7100'001f && rn_field(word) == index) {  // SUBS (immediate) into the zero register
            const auto bound = (word >> 10) & 0xfff;
            const auto entries = *condition == condition_hi ? bound + 1 : bound;
            if (entries == 0) return std::nullopt;
            return BoundsCheck{offset, entries};
        }
        if (writes_index || !leaves_flags(word)) return std::nullopt;
    }
    return std::nullopt;
}

/// The table base in register `base` at the load at `load`: the last write of it before must be
/// ADD base, base, #low, and the last one before that ADRP base. Only the straight line back is
/// read; a loop that set the base anew further down would go unseen, and the island's check would
/// then stop the program rather than let it branch anywhere else (ADR-0020 X2d).
std::optional<std::uint64_t> table_base(std::span<const std::byte> code, std::uint64_t address, std::size_t load,
                                        unsigned base) {
    constexpr std::size_t reach = 1024 * 4;
    std::optional<std::uint64_t> low;
    for (std::size_t offset = load; offset >= 4 && load - offset < reach;) {
        offset -= 4;
        const auto word = word_at(code, offset);
        if ((a64::register_use(word).writes & (1u << base)) == 0) continue;
        if (!low) {
            if ((word & 0xffc0'0000) != 0x9100'0000 || rd_field(word) != base || rn_field(word) != base) return std::nullopt;
            low = (word >> 10) & 0xfff;  // ADD (immediate), 64-bit, unshifted
            continue;
        }
        if ((word & 0x9f00'0000) != 0x9000'0000 || rd_field(word) != base) return std::nullopt;  // ADRP
        return pc_relative_target(word, address + offset) + *low;
    }
    return std::nullopt;
}

constexpr const char *not_a_jump_table = "it is a register branch that ends no jump table the rewriter knows";

/// The jump table that the `br x18` at `offset` ends, filled into `table`, or why there is none the
/// island can take (ADR-0020 X2d).
const char *find_jump_table(std::span<const std::byte> code, std::uint64_t address, std::size_t offset,
                            const ReadOnlyData &read_only_data, JumpTable &table) {
    if (offset < 8) return not_a_jump_table;
    // ADD x18, x18, xB (or xB, x18): ADD (shifted register), 64-bit, unshifted.
    const auto add = word_at(code, offset - 4);
    if ((add & 0xffe0'fc00) != 0x8b00'0000 || rd_field(add) != guest_x18) return not_a_jump_table;
    const auto first = rn_field(add), second = (add >> 16) & 0x1f;
    if (first != guest_x18 && second != guest_x18) return not_a_jump_table;
    const auto base = first == guest_x18 ? second : first;
    if (base == guest_x18 || base == zero_register) return not_a_jump_table;
    // LDRSW x18, [xB, xI, LSL #2] or [xB, wI, UXTW #2]: LDRSW (register), scaled.
    const auto load = word_at(code, offset - 8);
    if ((load & 0xffe0'dc00) != 0xb8a0'5800 || rd_field(load) != guest_x18 || rn_field(load) != base) {
        return not_a_jump_table;
    }

    const auto check = bounds_check(code, offset - 8, (load >> 16) & 0x1f);
    if (!check) return "it is a register branch whose jump table has no bounds check before it";
    if (branch_enters_window(code, check->compare, offset)) {
        return "it is a register branch into whose jump sequence another branch leads";
    }
    // The bounds branch must leave the sequence (to the default case): a direct branch inside it
    // that lands back inside would change how many entries the index reaches (ADR-0020 X2d).
    for (std::size_t j = check->compare; j < offset; j += 4) {
        if (const auto displacement = branch_displacement(word_at(code, j))) {
            const auto target = static_cast<std::int64_t>(j) + *displacement;
            if (target > static_cast<std::int64_t>(check->compare) && target <= static_cast<std::int64_t>(offset)) {
                return "it is a register branch whose bounds branch stays inside the jump sequence";
            }
        }
    }
    const auto start = table_base(code, address, offset - 8, base);
    if (!start) return "it is a register branch whose table base does not come from ADRP and ADD";

    const std::size_t size = 4 * std::size_t{check->entries};
    const auto entries = read_only_data ? read_only_data(*start, size) : std::span<const std::byte>{};
    if (entries.size() != size) return "it is a register branch whose table is not in read-only data";
    std::vector<std::uint64_t> targets;
    for (std::size_t at = 0; at < size; at += 4) {
        std::int32_t entry;
        std::memcpy(&entry, entries.data() + at, sizeof entry);
        const auto target = *start + static_cast<std::uint64_t>(std::int64_t{entry});
        if (target % 4 != 0 || target < address || target - address >= code.size()) {
            return "it is a register branch whose jump table has a target outside its code";
        }
        targets.push_back(target);
    }
    if (targets.empty()) return "it is a register branch whose jump table has no entries";  // guarded by bounds_check
    std::ranges::sort(targets);
    targets.erase(std::ranges::unique(targets).begin(), targets.end());
    if (targets.back() - targets.front() >= max_jump_table_span) {
        return "it is a register branch whose jump table spans more than 64 KiB";
    }
    table = {.address = address + offset, .table = *start, .entries = check->entries, .targets = std::move(targets)};
    return nullptr;
}

/// log2 of the bytes the landing pads cover: the span of the targets, rounded up to a power of two
/// and at least 16, so that one AND finds a target outside it or off a word. Also the one place that
/// checks a JumpTable an outside caller may have built, so the size and the emitter can trust it: the
/// targets are word-aligned, strictly ascending and no wider than the cap.
unsigned jump_table_span_bits(const JumpTable &table) {
    if (table.targets.empty()) throw RewriteError("a jump table without targets");
    for (std::size_t i = 0; i < table.targets.size(); ++i) {
        if (table.targets[i] % 4 != 0) throw RewriteError("a jump table target is not word aligned");
        if (i > 0 && table.targets[i] <= table.targets[i - 1]) {
            throw RewriteError("jump table targets are not strictly ascending");
        }
    }
    const auto span = table.targets.back() - table.targets.front() + 4;
    if (span > max_jump_table_span) throw RewriteError("a jump table spans more than the cap");
    unsigned bits = 4;
    while ((std::uint64_t{1} << bits) < span) ++bits;
    return bits;
}

std::size_t landing_pads(const JumpTable &table) {
    return (std::size_t{1} << jump_table_span_bits(table)) / 4;
}

}  // namespace

void scan(std::span<const std::byte> code, std::uint64_t address, Findings &findings,
          const ReadOnlyData &read_only_data) {
    if (address % 4 != 0 || code.size() % 4 != 0) {
        throw RewriteError(std::format("code at {:#x} with {:#x} bytes is not word aligned", address, code.size()));
    }
    // First, relocate whole load-/store-exclusive windows that use x18 (ADR-0020 X2c). Each covers
    // its offsets, so the main loop below leaves them alone; a window a branch could enter in the
    // middle is dropped, and its x18 uses are refused there instead.
    std::vector<std::pair<std::size_t, std::size_t>> covered;
    for (std::size_t offset = 0; offset + 4 <= code.size(); offset += 4) {
        if (!is_load_exclusive(word_at(code, offset))) continue;
        const auto end = exclusive_window_end(code, offset);
        if (!end) continue;
        auto region = build_exclusive_region(code, address, offset, *end);
        if (!region || branch_enters_window(code, offset, *end)) continue;
        findings.exclusive_regions.push_back(std::move(*region));
        covered.emplace_back(offset, *end);
    }
    const auto is_covered = [&](std::size_t offset) {
        return std::ranges::any_of(covered, [&](const auto &span) { return offset >= span.first && offset <= span.second; });
    };
    // Whether a jump table target lies inside a relocated window, past its first word (ADR-0020 X2d).
    const auto enters_window = [&](std::uint64_t target) {
        const auto offset = target - address;
        return std::ranges::any_of(covered, [&](const auto &span) { return offset > span.first && offset <= span.second; });
    };

    // Whether the scan is between a load-exclusive and the store-exclusive or CLREX that ends it.
    bool exclusive_sequence = false;
    for (std::size_t offset = 0; offset < code.size(); offset += 4) {
        if (is_covered(offset)) continue;
        const auto instruction = word_at(code, offset);
        const std::uint64_t at = address + offset;
        const auto rt = static_cast<std::uint8_t>(instruction & 0x1f);
        const auto use = a64::register_use(instruction);
        const bool reads = (use.reads & x18_bit) != 0;
        const bool writes = (use.writes & x18_bit) != 0;

        if (reads || writes) {
            if (reads && is_register_dump(code, offset)) {
                findings.x18_saves.push_back(at);
            } else if (reads) {
                findings.x18_reads.push_back(at);
            } else {
                findings.x18_writes.push_back(at);
            }
            const char *reason = (exclusive_sequence && !is_exclusive(instruction)) ? inside_exclusive_sequence
                                                                                    : unsupported_reason(instruction);
            if (instruction == br_x18 && reason != inside_exclusive_sequence) {
                // A register branch runs only as the end of a jump table, through landing pads (X2d).
                JumpTable table;
                reason = find_jump_table(code, address, offset, read_only_data, table);
                if (!reason && std::ranges::any_of(table.targets, enters_window)) {
                    reason = "it is a register branch whose jump table leads into a relocated exclusive window";
                }
                if (!reason) findings.jump_tables.push_back(std::move(table));
            } else if (!reason) {
                findings.sites.push_back({at, SiteKind::x18, guest_x18, instruction});
            }
            if (reason) findings.x18_unsupported.push_back({at, reason});
        } else if ((instruction & register_mask) == mrs_tpidr_el0) {
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

        if (is_exclusive(instruction)) {
            exclusive_sequence = is_load_exclusive(instruction);
        } else if (is_clrex(instruction)) {
            exclusive_sequence = false;
        }
    }
}

std::size_t stub_size(const Site &site) {
    switch (site.kind) {
        case SiteKind::thread_pointer_read: return site.reg == zero_register ? 0 : 4 * read_island_words;
        case SiteKind::thread_pointer_write: return 4 * write_island_words;
        case SiteKind::syscall: return 4 * syscall_veneer_words;
        case SiteKind::x18: return 4 * x18_island_words(site.instruction);
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

        case SiteKind::x18: {
            if (is_conditional_branch(site.instruction)) {
                // A two-way island: read guest x18 into the value scratch, test it with the branch
                // retargeted three words ahead, and restore the scratch on both exits before leaving.
                const auto value = scratch_registers(site.instruction).value;
                const auto tested = a64::substitute(site.instruction, guest_x18, value);
                if (!tested) throw RewriteError("x18 cannot be substituted in a conditional branch");
                write(stur(value, sp, scratch_slot_0));
                write(mrs_tpidrro_el0(value));
                write(bic_7(value, value));
                write(ldr_unsigned(value, value, guest_x18_offset));
                write(with_branch_displacement(*tested, 3 * 4));  // taken: skip to the restore below
                write(ldur(value, sp, scratch_slot_0));           // not taken: restore, then fall through
                write(b(write.next_address(), back));
                write(ldur(value, sp, scratch_slot_0));           // taken: restore, then branch on
                const auto taken = site_address + static_cast<std::uint64_t>(conditional_branch_displacement(site.instruction));
                write(b(write.next_address(), taken));
                write.finish();
                return b(site_address, stub_address);
            }
            const auto use = a64::register_use(site.instruction);
            const bool reads = (use.reads & (1u << 18)) != 0, writes = (use.writes & (1u << 18)) != 0;
            const auto scratch = scratch_registers(site.instruction);
            if (!scratch.found) throw RewriteError("no scratch register for an x18 island");
            const auto substituted = a64::substitute(site.instruction, guest_x18, scratch.value);
            if (!substituted) throw RewriteError("x18 cannot be substituted in this instruction");
            write(stur(scratch.value, sp, scratch_slot_0));
            if (writes) write(stur(scratch.base, sp, scratch_slot_1));
            if (reads) {
                write(mrs_tpidrro_el0(scratch.value));
                write(bic_7(scratch.value, scratch.value));
                write(ldr_unsigned(scratch.value, scratch.value, guest_x18_offset));
            }
            if (const auto words = pc_relative_words(site.instruction)) {
                // The same address, computed for the island's place: ADRP, then ADD and the load.
                const auto target = pc_relative_target(site.instruction, site_address);
                write(adrp(scratch.value, write.next_address(), target));
                if (words >= 2) write(add_immediate(scratch.value, scratch.value, target & 0xfff));
                if (words == 3) {
                    constexpr std::uint32_t loads[] = {0xb940'0000, 0xf940'0000, 0xb980'0000};  // LDR W, X; LDRSW
                    write(loads[site.instruction >> 30] | reg(scratch.value, 5) | reg(scratch.value, 0));
                }
            } else {
                write(*substituted);
            }
            if (writes) {
                write(mrs_tpidrro_el0(scratch.base));
                write(bic_7(scratch.base, scratch.base));
                write(str_unsigned(scratch.value, scratch.base, guest_x18_offset));
            }
            write(ldur(scratch.value, sp, scratch_slot_0));
            if (writes) write(ldur(scratch.base, sp, scratch_slot_1));
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

namespace {

/// A branch of any kind (B, BL, B.cond, CBZ, CBNZ, TBZ, TBNZ) with its displacement set to `delta`.
std::uint32_t with_any_branch_displacement(std::uint32_t word, std::int64_t delta) {
    const auto encoded = static_cast<std::uint32_t>(delta / 4);
    if ((word & 0x7c00'0000) == 0x1400'0000) return (word & 0xfc00'0000) | (encoded & 0x03ff'ffff);  // B, BL
    if (((word >> 24) & 0xff) == 0x54) return (word & ~(0x7ffffu << 5)) | ((encoded & 0x7ffff) << 5);  // B.cond
    return with_branch_displacement(word, delta);  // CBZ, CBNZ, TBZ, TBNZ
}

}  // namespace

std::size_t exclusive_region_stub_size(const ExclusiveRegion &region) {
    std::size_t words = 1 /* save value */ + region.words.size() + 1 /* restore value */ + 1 /* branch */;
    if (region.reads_x18) words += 3;                 // mrs, bic, ldr before the window
    if (region.writes_x18) words += 1 + 3 + 1;         // save base; mrs, bic, str; restore base
    return 4 * words;
}

std::uint32_t emit_exclusive_region(const ExclusiveRegion &region, std::uint64_t region_address,
                                    std::uint64_t stub_address, std::span<std::byte> stub) {
    if (stub.size() != exclusive_region_stub_size(region)) throw RewriteError("the stub buffer has the wrong size");
    const auto value = region.scratch, base = region.base;
    const auto window_bytes = 4 * region.words.size();
    StubWriter write(stub, stub_address);

    write(stur(value, sp, scratch_slot_0));
    if (region.writes_x18) write(stur(base, sp, scratch_slot_1));
    if (region.reads_x18) {
        write(mrs_tpidrro_el0(value));
        write(bic_7(value, value));
        write(ldr_unsigned(value, value, guest_x18_offset));
    }
    // The window itself, x18 renamed to the value register and its own branches retargeted.
    const std::uint64_t window_start = write.next_address();
    for (std::size_t index = 0; index < region.words.size(); ++index) {
        auto word = region.words[index];
        const auto use = a64::register_use(word);
        if ((use.reads | use.writes) & x18_bit) word = *a64::substitute(word, guest_x18, value);
        if (const auto displacement = branch_displacement(word)) {
            const auto target = static_cast<std::int64_t>(region_address + 4 * index) + *displacement;
            const auto offset_in_window = target - static_cast<std::int64_t>(region_address);
            const std::uint64_t to = (offset_in_window >= 0 && offset_in_window < static_cast<std::int64_t>(window_bytes))
                                         ? window_start + static_cast<std::uint64_t>(offset_in_window)
                                         : static_cast<std::uint64_t>(target);
            word = with_any_branch_displacement(word, static_cast<std::int64_t>(to - write.next_address()));
        }
        write(word);
    }
    if (region.writes_x18) {
        write(mrs_tpidrro_el0(base));
        write(bic_7(base, base));
        write(str_unsigned(value, base, guest_x18_offset));
    }
    write(ldur(value, sp, scratch_slot_0));
    if (region.writes_x18) write(ldur(base, sp, scratch_slot_1));
    write(b(write.next_address(), region_address + window_bytes));  // the instruction after the window
    write.finish();
    return b(region_address, stub_address);
}

std::size_t jump_table_stub_size(const JumpTable &table) {
    return 4 * (jump_island_words + landing_pad_words * landing_pads(table));
}

std::uint32_t emit_jump_table(const JumpTable &table, std::uint64_t branch_address, std::uint64_t stub_address,
                              std::span<std::byte> stub) {
    if (stub.size() != jump_table_stub_size(table)) throw RewriteError("the stub buffer has the wrong size");
    const auto scratch = scratch_registers(br_x18);  // the br names no other register: x0 and x1
    const auto value = scratch.value, base = scratch.base;
    // Each target keeps its distance to the br in the container.
    const auto in_container = [&](std::uint64_t target) { return branch_address + (target - table.address); };
    const auto low = in_container(table.targets.front());
    const auto trap = stub_address + 4 * jump_trap_word;
    const auto pads = stub_address + 4 * jump_island_words;
    StubWriter write(stub, stub_address);

    write(stur(value, sp, scratch_slot_0));
    write(stur(base, sp, scratch_slot_1));
    write(mrs_tpidrro_el0(value));
    write(bic_7(value, value));
    write(ldr_unsigned(value, value, guest_x18_offset));  // T, the target the guest computed
    write(adrp(base, write.next_address(), low));
    write(add_immediate(base, base, low & 0xfff));
    write(sub_register(value, value, base));  // T - lo
    write(and_outside_span(base, value, jump_table_span_bits(table)));
    write(cbnz(base, write.next_address(), trap));
    write(adr(base, write.next_address(), pads));
    write(add_shifted(value, base, value, 2));  // the landing pad of T: 16 bytes for each word
    write(br(value));
    if (write.next_address() != trap) throw RewriteError("the jump table island is not the expected length");
    // No landing pad for T: the guest's registers back, then stop.
    write(ldur(value, sp, scratch_slot_0));
    write(ldur(base, sp, scratch_slot_1));
    write(jump_table_trap);

    // One landing pad per word from lo: both scratch registers back, then on to the target; a word
    // that is no target of the table traps like the check above.
    if (write.next_address() != pads) throw RewriteError("the jump table landing pads are misplaced");
    auto target = table.targets.begin();
    for (std::size_t pad = 0; pad < landing_pads(table); ++pad) {
        const auto at = low + 4 * pad;
        write(ldur(value, sp, scratch_slot_0));
        write(ldur(base, sp, scratch_slot_1));
        if (target != table.targets.end() && in_container(*target) == at) {
            write(b(write.next_address(), at));
            ++target;
        } else {
            write(jump_table_trap);
        }
        write(jump_table_trap);
    }
    if (target != table.targets.end()) throw RewriteError("a jump table target has no landing pad");
    write.finish();
    return b(branch_address, stub_address);
}

}  // namespace apk::rewrite
