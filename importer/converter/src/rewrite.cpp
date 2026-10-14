#include "apkconv/rewrite.h"

#include "apkconv/a64.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <tuple>

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
constexpr std::size_t indirect_branch_island_words = 5;  // save x16, mrs, bic, ldr, br x16 (BR x18)
constexpr std::size_t indirect_call_island_words = 8;    // + save x30, adrp+add x30 for the return (BLR x18)

/// CBZ, CBNZ (bits 30:25 = 011010) and TBZ, TBNZ (011011): a conditional branch that tests one
/// register (Rt, bits 4:0) and has a taken and a not-taken exit. The rest of the branch group
/// takes no register (B, B.cond) or one only the two-way island can follow (CMPBR), or none a plain
/// island can (BR, BLR, RET).
constexpr bool is_conditional_branch(std::uint32_t instruction) {
    const auto kind = (instruction >> 25) & 0x3f;
    return kind == 0b011010 || kind == 0b011011;
}

/// CB<cc>, CBB<cc>, CBH<cc> (FEAT_CMPBR): compare two registers, or one with a 6-bit immediate, and
/// branch to a PC-relative label. Both forms carry the label as imm9 at bits 13:5 (Arm ARM, C4.1).
constexpr bool is_cmpbr(std::uint32_t instruction) {
    return ((instruction >> 25) & 0x3f) == 0b111010;
}

/// A branch with a register operand and two exits that the two-way island runs: the one-register
/// CBZ/CBNZ/TBZ/TBNZ and the two-register compare-and-branch CMPBR.
constexpr bool is_two_way_branch(std::uint32_t instruction) {
    return is_conditional_branch(instruction) || is_cmpbr(instruction);
}

/// BR x18 or BLR x18 exactly: an unconditional indirect branch or call through x18, without pointer
/// authentication. These run in an indirect island (ADR-0020 X2e); RET, the authenticated branches
/// (BRAA, BLRAA, ...) and x18 used only as the modifier of one do not, and stay refused.
constexpr bool is_indirect_branch_on_x18(std::uint32_t instruction) {
    return instruction == 0xd61f'0240u || instruction == 0xd63f'0240u;  // br x18, blr x18
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

/// A branch of any kind (B, BL, B.cond, CBZ, CBNZ, TBZ, TBNZ, CMPBR) with its displacement set to
/// `delta`. Throws when `delta` is not a multiple of 4 or does not fit the branch's field, so a
/// target the island cannot reach is caught here, as b() and adrp() catch theirs, not truncated to
/// a word inside the island (step C C1b).
std::uint32_t with_any_branch_displacement(std::uint32_t word, std::int64_t delta) {
    if (delta % 4 != 0) {
        throw RewriteError(std::format("a branch displacement of {:#x} is not a multiple of 4", delta));
    }
    const auto encoded = delta / 4;
    const auto field = [&](unsigned bits) -> std::uint32_t {
        const std::int64_t limit = std::int64_t{1} << (bits - 1);
        if (encoded < -limit || encoded >= limit) {
            throw RewriteError(std::format("a branch displacement of {:#x} is out of range for a {}-bit field",
                                           delta, bits));
        }
        return static_cast<std::uint32_t>(encoded) & ((1u << bits) - 1);
    };
    if ((word & 0x7c00'0000) == 0x1400'0000) return (word & 0xfc00'0000) | field(26);        // B, BL (imm26)
    if (((word >> 24) & 0xff) == 0x54) return (word & ~(0x7ffffu << 5)) | (field(19) << 5);  // B.cond (imm19)
    if (is_cmpbr(word)) return (word & ~(0x1ffu << 5)) | (field(9) << 5);                     // CMPBR (imm9)
    if (is_test_bit_branch(word)) return (word & ~(0x3fffu << 5)) | (field(14) << 5);         // TBZ, TBNZ (imm14)
    return (word & ~(0x7ffffu << 5)) | (field(19) << 5);                                      // CBZ, CBNZ (imm19)
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
/// Words of an indirect island: BLR x18 also saves x30 and recomputes the guest return address.
std::size_t indirect_island_words(std::uint32_t instruction) {
    return instruction == 0xd63f'0240u ? indirect_call_island_words : indirect_branch_island_words;  // blr vs br x18
}

std::size_t x18_island_words(std::uint32_t instruction) {
    if (is_two_way_branch(instruction)) return branch_island_words;
    if (is_indirect_branch_on_x18(instruction)) return indirect_island_words(instruction);
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

// Why an exclusive window with x18 inside is not relocated as one island (ADR-0020 X2c), each x18 use in it
// refused with the cause, so that one refusal names every cause at once.
constexpr const char *window_unclosed = "it lies in an exclusive window that does not close within 64 words";
constexpr const char *window_nested =
    "it lies in an exclusive window that opens another load-exclusive before it closes";
constexpr const char *window_stack = "it lies in an exclusive window that moves the stack pointer";
constexpr const char *window_no_register = "it lies in an exclusive window that leaves no register free";
constexpr const char *window_registers =
    "it lies in an exclusive window that writes x18 and leaves fewer than two registers free";
constexpr const char *window_field = "it lies in an exclusive window in which x18 has no register field of its own";
constexpr const char *window_entered = "it lies in an exclusive window into which a branch from outside leads";

/// Why an x18 instruction cannot run in an island, or nullptr when it can. The island saves two
/// scratch registers in the red zone, loads the guest x18 from the TSD, runs the instruction with
/// the value register in place of x18, stores the value back and branches to the next instruction;
/// a two-way branch (CBZ/CBNZ/TBZ/TBNZ and CMPBR) leaves by two exits, a PC-relative form is computed
/// for the island's place. That leaves out: the thread pointer moved through x18, register branches,
/// exclusives (scan() adds what lies between them), sp moved or addressed below, and x18 without a
/// register field of its own.
const char *unsupported_reason(std::uint32_t instruction) {
    const auto system_register = instruction & register_mask;
    if (system_register == mrs_tpidr_el0 || system_register == msr_tpidr_el0) {
        return "it moves the thread pointer through x18";
    }
    if (is_branch(instruction) && !is_two_way_branch(instruction) && !is_indirect_branch_on_x18(instruction)) {
        return "it is a register branch the indirect island cannot follow: a return, a pointer-authenticated "
               "branch, or x18 only as the modifier of one";
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
/// (imm26), B.cond, BC.cond (imm19), the conditional branches CBZ, CBNZ, TBZ, TBNZ, and the
/// compare-and-branch CB<cc>, CBB<cc>, CBH<cc> (CMPBR, imm9 at bits 13:5).
std::optional<std::int64_t> branch_displacement(std::uint32_t word) {
    if ((word & 0x7c00'0000) == 0x1400'0000) {  // B, BL
        return static_cast<std::int64_t>(static_cast<std::int32_t>(word << 6) >> 6) * 4;
    }
    if (((word >> 24) & 0xff) == 0x54) {  // B.cond, BC.cond
        return static_cast<std::int64_t>(static_cast<std::int32_t>(word << 8) >> 13) * 4;
    }
    if (is_conditional_branch(word)) return conditional_branch_displacement(word);
    if (is_cmpbr(word)) return static_cast<std::int64_t>(static_cast<std::int32_t>(word << 18) >> 23) * 4;
    return std::nullopt;
}

/// How far after its load-exclusive a window may close, in bytes.
constexpr std::size_t window_cap = 64 * 4;

/// The end offset (inclusive) of the exclusive window that opens at `open` (a load-exclusive): the
/// first store-exclusive, plus a CLREX right behind it, or the first CLREX. Nullopt when the window
/// nests another load-exclusive or finds no close within a small cap.
std::optional<std::size_t> exclusive_window_end(std::span<const std::byte> code, std::size_t open) {
    for (std::size_t j = open + 4; j < code.size() && j - open <= window_cap; j += 4) {
        const auto word = word_at(code, j);
        if (is_load_exclusive(word)) return std::nullopt;
        if (is_store_exclusive(word)) {
            return (j + 4 < code.size() && is_clrex(word_at(code, j + 4))) ? j + 4 : j;
        }
        if (is_clrex(word)) return j;
    }
    return std::nullopt;
}

/// For a window that exclusive_window_end() finds no close for: the offset of its last word as scan() follows the
/// sequence (the store-exclusive or CLREX that ends it, the word before the next load-exclusive, or the last word of
/// the code), and why it is not relocated.
std::pair<std::size_t, const char *> unclosed_window(std::span<const std::byte> code, std::size_t open) {
    for (std::size_t j = open + 4; j < code.size(); j += 4) {
        const auto word = word_at(code, j);
        if (is_load_exclusive(word)) return {j - 4, j - open <= window_cap ? window_nested : window_unclosed};
        if (is_exclusive(word) || is_clrex(word)) return {j, window_unclosed};
    }
    return {code.size() - 4, window_unclosed};
}

/// The words of the window [open, end] that a forward branch inside it can skip: what they write
/// may not have happened when a later word of the window runs.
std::vector<bool> skippable_words(std::span<const std::byte> code, std::size_t open, std::size_t end) {
    const auto count = static_cast<std::int64_t>((end - open) / 4 + 1);
    std::vector<bool> skippable(static_cast<std::size_t>(count), false);
    for (std::int64_t k = 0; k < count; ++k) {
        const auto displacement = branch_displacement(word_at(code, open + 4 * static_cast<std::size_t>(k)));
        if (!displacement) continue;
        const auto target = std::min(k + *displacement / 4, count);
        for (auto j = k + 1; j < target; ++j) skippable[static_cast<std::size_t>(j)] = true;
    }
    return skippable;
}

/// Whether one of `registers` is live into the window [open, end], the store after the window
/// counted as a read: a word reads it before a write that every path through the window passes, or
/// there is no such write.
bool live_into_window(std::span<const std::byte> code, std::size_t open, std::size_t end, std::uint32_t registers) {
    const auto skippable = skippable_words(code, open, end);
    std::uint32_t written = 0;
    for (std::size_t k = 0; open + 4 * k <= end; ++k) {
        const auto use = a64::register_use(word_at(code, open + 4 * k));
        if ((use.reads & registers & ~written) != 0) return true;
        if (!skippable[k]) written |= use.writes;
    }
    return (registers & ~written) != 0;
}

/// Builds the relocatable region for the window [open, end] when it is simple enough (ADR-0020
/// X2c): it uses x18, keeps a scratch register free, moves the stack pointer nowhere and
/// substitutes x18 in every word that uses it. Nullopt otherwise, with the cause in `problem` (nullptr for a
/// window without x18), so the x18 uses inside are refused instead. Whether the window may run again from its
/// start is restart_problem()'s question.
std::optional<ExclusiveRegion> build_exclusive_region(std::span<const std::byte> code, std::uint64_t address,
                                                      std::size_t open, std::size_t end, const char *&problem) {
    problem = nullptr;
    ExclusiveRegion region;
    region.address = address + open;
    std::uint32_t used = 0;
    bool any_x18 = false;
    for (std::size_t j = open; j <= end; j += 4) {
        const auto word = word_at(code, j);
        if (writes_stack_pointer(word)) {
            problem = window_stack;
            return std::nullopt;
        }
        const auto use = a64::register_use(word);
        used |= use.reads | use.writes;
        if ((use.reads | use.writes) & x18_bit) {
            any_x18 = true;
            if (use.writes & x18_bit) region.writes_x18 = true;
        }
        region.words.push_back(word);
    }
    if (!any_x18) return std::nullopt;  // an exclusive loop without x18 needs no rewrite
    // The island loads x18 before the window when some path reads it before writing it, or leaves
    // it unwritten: the store after the window writes the value register back on every path. The
    // first use alone does not tell, a branch can skip it (ADR-0020 X4).
    region.loads_x18 = live_into_window(code, open, end, x18_bit);

    // Free registers: the value in place of x18, and when the window writes x18 the TSD base for the
    // store-back; a window that only reads x18 loads it through the value register itself. They may
    // hold live values around the loop, so the island saves them in the red zone.
    const unsigned needed = region.writes_x18 ? 2 : 1;
    unsigned found = 0;
    for (unsigned candidate = 0; candidate < 29 && found < needed; ++candidate) {
        if (candidate == guest_x18 || (used & (1u << candidate)) != 0) continue;
        (found++ == 0 ? region.scratch : region.base) = static_cast<std::uint8_t>(candidate);
    }
    if (found < needed) {
        problem = region.writes_x18 ? window_registers : window_no_register;
        return std::nullopt;
    }
    for (const auto word : region.words) {
        const auto use = a64::register_use(word);
        if (((use.reads | use.writes) & x18_bit) && !a64::substitute(word, guest_x18, region.scratch)) {
            problem = window_field;
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

// --- Windows that run again from their start (ADR-0020 X4) -------------------------------------
//
// A signal inside a relocated window sends the thread back to its load-exclusive, so running the
// window a second time must give what running it once gives.

/// Whether `word` may stand in such a window: data processing on general-purpose registers
/// (immediate and register; not ADR and ADRP, which the relocated island would compute for its own
/// place), a direct branch other than BL (restart_problem checks that it stays inside; the return
/// address of a BL would point into the island), a barrier, CLREX, and the exclusive pair itself.
/// Anything else the island would copy unchanged, a system instruction or SIMD register the checks
/// below cannot follow, or a memory access running twice.
bool allowed_in_window(std::uint32_t word) {
    if (is_exclusive(word) || is_clrex(word)) return true;
    if (branch_displacement(word)) return (word & 0xfc00'0000) != 0x9400'0000;  // not BL
    const auto barrier = word & 0xffff'f0ff;  // CRm cleared
    if (barrier == 0xd503'309f || barrier == 0xd503'30bf || barrier == 0xd503'30df) return true;  // DSB, DMB, ISB
    if ((word & 0x1c00'0000) == 0x1000'0000) return (word & 0x1f00'0000) != 0x1000'0000;         // DP (immediate)
    return (word & 0x0e00'0000) == 0x0a00'0000;                                                  // DP (register)
}

/// Whether `word`, allowed in a window, changes NZCV: the S forms of add and subtract (immediate,
/// shifted and extended register, with carry), ANDS and BICS, the conditional compares, RMIF, SETF8
/// and SETF16.
bool modifies_flags(std::uint32_t word) {
    if ((word & 0x1f00'0000) == 0x1100'0000 || (word & 0x1f00'0000) == 0x0b00'0000) return bit(word, 29);
    if ((word & 0x1f80'0000) == 0x1200'0000 || (word & 0x1f00'0000) == 0x0a00'0000) return ((word >> 29) & 3) == 3;
    if ((word & 0x1fe0'0000) == 0x1a00'0000) return bit(word, 29);  // ADCS, SBCS, RMIF, SETF8, SETF16
    return (word & 0x1fe0'0000) == 0x1a40'0000;                      // CCMN, CCMP
}

/// Whether `word`, allowed in a window, reads NZCV: B.cond and BC.cond, the conditional selects and
/// compares, and add and subtract with carry.
bool reads_flags(std::uint32_t word) {
    if ((word & 0xff00'0000) == 0x5400'0000) return true;                                         // B.cond, BC.cond
    if ((word & 0x1fe0'0000) == 0x1a80'0000 || (word & 0x1fe0'0000) == 0x1a40'0000) return true;  // CSEL.., CCMN, CCMP
    return (word & 0x1fe0'fc00) == 0x1a00'0000;                                                   // ADC, ADCS, SBC, SBCS
}

/// Whether `word` sets all of NZCV without reading them: ADDS, SUBS, ANDS, BICS.
bool sets_flags(std::uint32_t word) {
    return modifies_flags(word) && !reads_flags(word) && (word & 0x1fe0'0000) != 0x1a00'0000;
}

constexpr const char *window_branch_leaves =
    "it lies in an exclusive window that cannot run again from its start: a branch leaves it";
constexpr const char *window_instruction =
    "it lies in an exclusive window that cannot run again from its start: it holds an instruction other than "
    "data processing on general-purpose registers, a branch within the window (not a call), a barrier or the "
    "exclusive pair";
constexpr const char *window_memory =
    "it lies in an exclusive window that cannot run again from its start: it holds a memory access, which would run "
    "again";
constexpr const char *window_call =
    "it lies in an exclusive window that cannot run again from its start: it holds a call, whose return address would "
    "point into the island";
constexpr const char *window_writes_input =
    "it lies in an exclusive window that cannot run again from its start: it writes a register it reads first";
constexpr const char *window_reads_flags =
    "it lies in an exclusive window that cannot run again from its start: it reads the flags before it sets them";

/// Why the window [open, end] cannot run again from its load-exclusive, or nullptr when it can. It
/// holds only what allowed_in_window() allows, and no branch leaves it. It writes no register it
/// reads before writing it; x18 is exempt, the island renames it, and so is what the load-exclusive
/// loads and the status the store-exclusive writes, both of which come first or last. If it changes
/// the flags, it does not read them before it sets them.
const char *restart_problem(std::span<const std::byte> code, std::size_t open, std::size_t end) {
    const std::size_t count = (end - open) / 4 + 1;
    std::size_t commit = count - 1;  // the store-exclusive, or the CLREX that closes a window without one
    for (std::size_t k = 1; k < count; ++k) {
        if (is_store_exclusive(word_at(code, open + 4 * k))) {
            commit = k;
            break;
        }
    }
    std::uint32_t written = 0;
    bool changes_flags = false;
    for (std::size_t k = 0; k < count; ++k) {
        const auto word = word_at(code, open + 4 * k);
        if (!allowed_in_window(word)) {
            if (is_load_store(word)) return window_memory;
            // BL; BLR, BLRAA, BLRAB and their Z forms
            if ((word & 0xfc00'0000) == 0x9400'0000 || (word & 0xfeff'0000) == 0xd63f'0000) return window_call;
            return window_instruction;
        }
        if (const auto displacement = branch_displacement(word)) {
            const auto target = 4 * static_cast<std::int64_t>(k) + *displacement;
            if (target < 0 || target >= 4 * static_cast<std::int64_t>(count)) return window_branch_leaves;
        }
        if (k != commit) written |= a64::register_use(word).writes;
        changes_flags = changes_flags || modifies_flags(word);
    }
    written &= ~x18_bit;
    const auto skippable = skippable_words(code, open, end);
    const auto opening = a64::register_use(word_at(code, open));
    if ((opening.reads & written) != 0) return window_writes_input;
    std::uint32_t defined = opening.writes;
    bool flags_defined = false;
    for (std::size_t k = 1; k < count; ++k) {
        const auto word = word_at(code, open + 4 * k);
        const auto use = a64::register_use(word);
        if ((use.reads & ~defined & written) != 0) return window_writes_input;
        if (changes_flags && !flags_defined && reads_flags(word)) return window_reads_flags;
        if (!skippable[k]) {
            defined |= use.writes;
            flags_defined = flags_defined || sets_flags(word);
        }
    }
    return nullptr;
}

// --- Windows that keep x18 in the register (ADR-0020, the Unity test APK step B) -----------------------------
//
// XNU zeroes x18 only at a context switch (F4), which an exception brings, and every exception return clears the
// local exclusive monitor: the Arm ARM's AArch64.ExceptionReturn calls ClearExclusiveLocal unconditionally. So in
// a window that runs again from its load-exclusive when its store-exclusive fails, a pass that lost x18 fails, and
// only a pass without an exception, which kept x18, succeeds.

/// How many words after a window's retry branch x18 must be written again.
constexpr std::size_t dead_x18_reach = 16;

/// Whether the window [open, end], which an island cannot take, may run as it is with x18 in the register. It must
/// run the same again from its start (restart_problem), and no branch may enter it. It ends with its store-exclusive,
/// and the next word is CBNZ on its status register (not x18) back to the load-exclusive, so every pass ends at the
/// store-exclusive and fails when it lost x18. No path reads x18 before writing it, so a pass uses no value from
/// before its load-exclusive. No value derived from x18 decides a branch or gives the store-exclusive's address,
/// so a pass that lost x18 can neither loop nor fault, only compute values its failed store-exclusive discards;
/// nor does the window move sp or authenticate a pointer.
/// After the CBNZ, x18 is written within a few words before any read or branch: the guest's x18 lives in TSD slot
/// 11 (ADR-0020), which this window leaves alone, so x18 must be dead after it.
bool stays_in_place(std::span<const std::byte> code, std::size_t open, std::size_t end) {
    const auto closing = word_at(code, end);
    if (!is_store_exclusive(closing) || end + 4 >= code.size()) return false;
    const unsigned status = (closing >> 16) & 0x1f;
    if (status == guest_x18 || status == zero_register) return false;
    const auto retry = word_at(code, end + 4);
    if ((retry & 0x7f00'0000) != 0x3500'0000 || (retry & 0x1f) != status) return false;  // CBNZ wS|xS
    const auto back = static_cast<std::int64_t>(open) - static_cast<std::int64_t>(end + 4);
    if (conditional_branch_displacement(retry) != back) return false;
    if (restart_problem(code, open, end) != nullptr || branch_enters_window(code, open, end)) return false;
    // Two faults the checks below cannot see: sp, register 31, is no register the decoder reports, and an
    // authentication (PACIA, AUTIA, XPACI and the rest of data processing, 1 source, opcode2 1) can fault with
    // FEAT_FPAC where it would only compute a value.
    for (std::size_t j = open; j <= end; j += 4) {
        const auto word = word_at(code, j);
        if (writes_stack_pointer(word) || (word & 0xffff'0000) == 0xdac1'0000) return false;
    }

    // Written before read on every path through the window.
    const auto skippable = skippable_words(code, open, end);
    bool written = false;
    for (std::size_t k = 0; open + 4 * k <= end; ++k) {
        const auto use = a64::register_use(word_at(code, open + 4 * k));
        if ((use.reads & x18_bit) != 0 && !written) return false;
        if (!skippable[k]) written = written || (use.writes & x18_bit) != 0;
    }

    // What x18 reaches, whatever the order: registers and the flags, up to the store-exclusive, whose status is not.
    std::uint32_t tainted = x18_bit;
    bool flags = false;
    for (bool changed = true; changed;) {
        changed = false;
        for (std::size_t j = open; j < end; j += 4) {
            const auto word = word_at(code, j);
            const auto use = a64::register_use(word);
            if ((use.reads & tainted) == 0 && !(flags && reads_flags(word))) continue;
            if ((use.writes & ~tainted) != 0) {
                tainted |= use.writes;
                changed = true;
            }
            if (!flags && modifies_flags(word)) flags = changed = true;
        }
    }
    for (std::size_t j = open; j < end; j += 4) {
        const auto word = word_at(code, j);
        if (!is_branch(word)) continue;  // any branch, the register branches with no displacement included
        if ((a64::register_use(word).reads & tainted) != 0 || (flags && reads_flags(word))) return false;
    }
    if ((tainted & (1u << rn_field(closing))) != 0) return false;

    // Dead after the window.
    for (std::size_t j = end + 8, n = 0; j + 4 <= code.size() && n < dead_x18_reach; j += 4, ++n) {
        const auto word = word_at(code, j);
        const auto use = a64::register_use(word);
        if ((use.reads & x18_bit) != 0) return false;
        if ((use.writes & x18_bit) != 0) return true;
        if (is_branch(word)) return false;
    }
    return false;
}

// --- Jump tables through x18 (ADR-0020 X2d) ------------------------------------------------------

constexpr std::uint32_t br_x18 = 0xd61f'0240;
constexpr std::uint32_t jump_table_trap = 0xd420'0300;  // BRK #0x18: a target without a landing pad
constexpr std::uint64_t max_jump_table_span = 32 * 1024;

// The island (see emit_jump_table): 13 words up to its BR, then the trap of 3; each landing pad has 4.
constexpr std::size_t jump_trap_word = 13;
constexpr std::size_t jump_island_words = 16;
constexpr std::size_t landing_pad_words = 4;
// The island table gives the length of an island in 16 bits (ADR-0020 X4), which caps the span.
static_assert(jump_island_words + landing_pad_words * (max_jump_table_span / 4) <= 0xffff);

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
// The table bytes are not served as read-only data, so they may lie in executable memory, where the
// x18 pass scans them as code and could rewrite an entry. Such a br stays refused rather than
// degrading to an indirect island, which would branch to a possibly corrupted runtime target (X2e).
constexpr const char *table_not_read_only = "it is a register branch whose table is not in read-only data";

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
    if (entries.size() != size) return table_not_read_only;
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
        return "it is a register branch whose jump table spans more than 32 KiB";
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
    // its offsets, so the main loop below leaves them alone. Every other window is dropped with its
    // cause, and its x18 uses are refused there with it: one without a close, one the island cannot
    // build, one a branch could enter in the middle, one that cannot run again from its start (X4).
    std::vector<std::pair<std::size_t, std::size_t>> covered;
    std::vector<std::tuple<std::size_t, std::size_t, const char *>> refused_windows;
    for (std::size_t offset = 0; offset + 4 <= code.size(); offset += 4) {
        if (!is_load_exclusive(word_at(code, offset))) continue;
        const auto end = exclusive_window_end(code, offset);
        if (!end) {
            const auto [last, problem] = unclosed_window(code, offset);
            refused_windows.emplace_back(offset, last, problem);
            continue;
        }
        const char *problem = nullptr;
        auto region = build_exclusive_region(code, address, offset, *end, problem);
        if (!region) {
            if (problem && stays_in_place(code, offset, *end)) {
                findings.in_place_windows.push_back(address + offset);
                covered.emplace_back(offset, *end);
            } else if (problem) {
                refused_windows.emplace_back(offset, *end, problem);
            }
            continue;
        }
        problem = branch_enters_window(code, offset, *end) ? window_entered : restart_problem(code, offset, *end);
        if (problem) {
            refused_windows.emplace_back(offset, *end, problem);
            continue;
        }
        findings.exclusive_regions.push_back(std::move(*region));
        covered.emplace_back(offset, *end);
    }
    const auto refused_window = [&](std::size_t offset) -> const char * {
        for (const auto &[open, end, problem] : refused_windows) {
            if (offset >= open && offset <= end) return problem;
        }
        return nullptr;
    };
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
            const char *window_reason = refused_window(offset);
            const char *reason = window_reason != nullptr                              ? window_reason
                                 : (exclusive_sequence && !is_exclusive(instruction)) ? inside_exclusive_sequence
                                                                                      : unsupported_reason(instruction);
            if (instruction == br_x18 && window_reason == nullptr && reason != inside_exclusive_sequence) {
                // A br x18 runs as the end of a jump table through landing pads (X2d), or, when it ends
                // no jump table the rewriter knows, as a general indirect island that branches to the
                // guest x18 (X2e). A recognized table whose target enters a relocated exclusive window
                // stays refused: the indirect island cannot check the runtime target against it.
                JumpTable table;
                const char *table_reason = find_jump_table(code, address, offset, read_only_data, table);
                if (!table_reason && std::ranges::any_of(table.targets, enters_window)) {
                    reason = "it is a register branch whose jump table leads into a relocated exclusive window";
                } else if (!table_reason) {
                    findings.jump_tables.push_back(std::move(table));
                    reason = nullptr;
                } else if (table_reason == table_not_read_only) {
                    reason = table_reason;  // a table in executable memory: stay refused, do not degrade (X2e)
                } else {
                    findings.sites.push_back({at, SiteKind::x18, guest_x18, instruction});  // the indirect island
                    reason = nullptr;
                }
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
            if (is_two_way_branch(site.instruction)) {
                // A two-way island: read guest x18 into the value scratch, test it with the branch
                // retargeted three words ahead, and restore the scratch on both exits before leaving.
                // Holds for CMPBR too (it reads a second register directly, x18 only through value).
                const auto value = scratch_registers(site.instruction).value;
                const auto tested = a64::substitute(site.instruction, guest_x18, value);
                if (!tested) throw RewriteError("x18 cannot be substituted in a two-way branch");
                write(stur(value, sp, scratch_slot_0));
                write(mrs_tpidrro_el0(value));
                write(bic_7(value, value));
                write(ldr_unsigned(value, value, guest_x18_offset));
                write(with_any_branch_displacement(*tested, 3 * 4));  // taken: skip to the restore below
                write(ldur(value, sp, scratch_slot_0));              // not taken: restore, then fall through
                write(b(write.next_address(), back));
                write(ldur(value, sp, scratch_slot_0));              // taken: restore, then branch on
                const auto taken = site_address + static_cast<std::uint64_t>(*branch_displacement(site.instruction));
                write(b(write.next_address(), taken));
                write.finish();
                return b(site_address, stub_address);
            }
            if (is_indirect_branch_on_x18(site.instruction)) {
                // An indirect island: load the guest x18 into x16 and branch to it. x16 (IP0) is dead
                // at both forms under the AArch64 PCS: BLR x18 is a call, so x16 is caller-saved and
                // not an argument; BR x18 is a tail branch to a function entry, where x16 is undefined
                // (and x18, reserved on Android, is never an intra-procedure branch register). So
                // clobbering x16 past the branch is sound for conforming code. A signal stopped inside
                // the island restores x16 (and, for BLR, x30) from the red zone and returns to the
                // site, so the pass re-runs cleanly (ADR-0020 X2e).
                const bool is_call = site.instruction == 0xd63f'0240u;  // blr x18
                write(stur(x16, sp, scratch_slot_0));
                if (is_call) write(stur(x30, sp, scratch_slot_1));
                write(mrs_tpidrro_el0(x16));
                write(bic_7(x16, x16));
                write(ldr_unsigned(x16, x16, guest_x18_offset));
                if (is_call) {
                    // The guest's return address, the instruction after the call, for the island's place.
                    write(adrp(x30, write.next_address(), back));
                    write(add_immediate(x30, x30, back & 0xfff));
                }
                write(br(x16));
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

std::size_t exclusive_region_stub_size(const ExclusiveRegion &region) {
    std::size_t words = 1 /* save value */ + region.words.size() + 1 /* restore value */ + 1 /* branch */;
    if (region.loads_x18) words += 3;                 // mrs, bic, ldr before the window
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
    if (region.loads_x18) {
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

IslandLayout island_layout(const Site &site) {
    if (site.kind != SiteKind::x18) throw RewriteError("only an x18 site has an island layout");
    const auto scratch = scratch_registers(site.instruction);
    IslandLayout layout{.kind = APK_ISLAND_SINGLE,
                        .words = static_cast<std::uint32_t>(x18_island_words(site.instruction)),
                        .value = static_cast<std::uint8_t>(scratch.value)};
    if (is_two_way_branch(site.instruction)) {
        layout.kind = APK_ISLAND_BRANCH;  // changes nothing but pc: always back to the site
        return layout;
    }
    if (is_indirect_branch_on_x18(site.instruction)) {
        // As emit() writes it: x16 holds the guest x18, and BLR also saves and recomputes x30. The
        // island branches to the guest x18 and never stores it, so (like APK_ISLAND_BRANCH) a signal
        // always returns to the site; the red zone gives x16 back from word 1 and x30 from word 2.
        layout.kind = APK_ISLAND_INDIRECT;
        layout.words = static_cast<std::uint32_t>(indirect_island_words(site.instruction));
        layout.value = x16;
        if (site.instruction == 0xd63f'0240u) layout.base = x30;  // blr x18 saves x30 for the return
        return layout;  // commit = NO_COMMIT, store = 0, resume = 0 by default
    }
    // As emit() writes it: save the value (and the base), load x18, the instruction in one to three
    // words, then store x18 with mrs, bic and str.
    const auto use = a64::register_use(site.instruction);
    const bool reads = (use.reads & x18_bit) != 0, writes = (use.writes & x18_bit) != 0;
    const std::size_t first = 1 + (writes ? 1u : 0u) + (reads ? 3u : 0u);
    const auto body = std::max<std::size_t>(pc_relative_words(site.instruction), 1);
    layout.commit = static_cast<std::uint8_t>(first + body - 1);
    if (writes) {
        layout.base = static_cast<std::uint8_t>(scratch.base);
        layout.store = static_cast<std::uint8_t>(layout.commit + 3);
    }
    layout.resume = 1;
    return layout;
}

IslandLayout exclusive_region_layout(const ExclusiveRegion &region) {
    IslandLayout layout{.kind = APK_ISLAND_EXCLUSIVE,
                        .words = static_cast<std::uint32_t>(exclusive_region_stub_size(region) / 4),
                        .value = region.scratch};
    // As emit_exclusive_region() writes it: save the value (and the base), load x18, the window,
    // then store x18 with mrs, bic and str. The window completes with its store-exclusive, or with
    // the CLREX that closes a window without one.
    const std::size_t first = 1 + (region.writes_x18 ? 1u : 0u) + (region.loads_x18 ? 3u : 0u);
    std::size_t closing = region.words.size() - 1;
    for (std::size_t index = 1; index < region.words.size(); ++index) {
        if (is_store_exclusive(region.words[index])) {
            closing = index;
            break;
        }
    }
    layout.commit = static_cast<std::uint8_t>(first + closing);
    if (region.writes_x18) {
        layout.base = region.base;
        layout.store = static_cast<std::uint8_t>(first + region.words.size() + 2);
    }
    layout.resume = static_cast<std::uint8_t>(region.words.size());
    return layout;
}

IslandLayout jump_table_layout(const JumpTable &table) {
    // As emit_jump_table() writes it: both scratch registers saved and nothing stored. The jump
    // changes nothing but pc, so the thread always goes back to the br.
    const auto scratch = scratch_registers(br_x18);
    return {.kind = APK_ISLAND_JUMP_TABLE,
            .words = static_cast<std::uint32_t>(jump_table_stub_size(table) / 4),
            .value = static_cast<std::uint8_t>(scratch.value),
            .base = static_cast<std::uint8_t>(scratch.base)};
}

}  // namespace apk::rewrite
