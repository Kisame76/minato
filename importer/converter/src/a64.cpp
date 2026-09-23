#include "apkconv/a64.h"

namespace apk::a64 {
namespace {

constexpr unsigned zr = 31;  // xzr or sp: never a register of the masks
constexpr unsigned lr = 30;

/// Bits high..low of `word`.
constexpr std::uint32_t field(std::uint32_t word, unsigned high, unsigned low) {
    return (word >> low) & ((2u << (high - low)) - 1);
}

constexpr bool flag(std::uint32_t word, unsigned bit) {
    return ((word >> bit) & 1) != 0;
}

// The register fields of the Arm ARM: Rd and Rt at 4:0, Rn at 9:5, Rt2 and Ra at 14:10, Rm and Rs
// at 20:16.
constexpr unsigned rd(std::uint32_t word) { return field(word, 4, 0); }
constexpr unsigned rn(std::uint32_t word) { return field(word, 9, 5); }
constexpr unsigned ra(std::uint32_t word) { return field(word, 14, 10); }
constexpr unsigned rm(std::uint32_t word) { return field(word, 20, 16); }

/// Collects the registers of one instruction; register 31 is dropped.
class Use {
public:
    Use &read(unsigned reg) {
        use_.reads |= bit_of(reg);
        return *this;
    }
    Use &write(unsigned reg) {
        use_.writes |= bit_of(reg);
        return *this;
    }
    Use &modify(unsigned reg) { return read(reg).write(reg); }

    /// Registers reg to reg + count - 1, as in the pairs of CASP and the eight registers of ST64B.
    Use &read(unsigned reg, unsigned count) {
        for (unsigned index = 0; index < count; ++index) read(reg + index);
        return *this;
    }
    Use &write(unsigned reg, unsigned count) {
        for (unsigned index = 0; index < count; ++index) write(reg + index);
        return *this;
    }
    Use &modify(unsigned reg, unsigned count) { return read(reg, count).write(reg, count); }

    operator RegisterUse() const { return use_; }

private:
    static std::uint32_t bit_of(unsigned reg) { return reg < zr ? 1u << reg : 0; }

    RegisterUse use_;
};

/// Base register of a load or store, written back as well in the pre- and post-index forms.
Use address(unsigned base, bool writeback) {
    Use use;
    return writeback ? use.modify(base) : use.read(base);
}

/// DecodeBitMasks for logical immediates: element size from N:NOT(imms), and never all ones.
bool is_logical_immediate(bool sf, bool n, unsigned imms) {
    if (!sf && n) return false;
    const unsigned length_bits = (static_cast<unsigned>(n) << 6) | (~imms & 0x3f);
    if (length_bits < 2) return false;
    unsigned length = 0;
    while ((length_bits >> (length + 1)) != 0) ++length;
    const unsigned levels = (1u << length) - 1;
    return (imms & levels) != levels;
}

// --- Data processing, immediate (op1 = 100x) ---------------------------------------------------

RegisterUse data_processing_immediate(std::uint32_t w) {
    const bool sf = flag(w, 31);
    const auto opc = field(w, 30, 29);
    switch (field(w, 25, 23)) {
        case 0b000:
        case 0b001:  // ADR, ADRP
            return Use().write(rd(w));
        case 0b010:  // ADD, ADDS, SUB, SUBS (immediate)
            return Use().read(rn(w)).write(rd(w));
        case 0b011:
            if (!flag(w, 22)) {  // ADDG, SUBG (MTE)
                if (!sf || flag(w, 29)) return {};
                return Use().read(rn(w)).write(rd(w));
            }
            // SMAX, UMAX, SMIN, UMIN (immediate) (CSSC)
            if (opc != 0 || field(w, 21, 18) > 0b0011) return {};
            return Use().read(rn(w)).write(rd(w));
        case 0b100:  // AND, ORR, EOR, ANDS (immediate)
            if (!is_logical_immediate(sf, flag(w, 22), field(w, 15, 10))) return {};
            return Use().read(rn(w)).write(rd(w));
        case 0b101:  // MOVN, MOVZ, MOVK; MOVK keeps the other bits of the register
            if (opc == 0b01 || (!sf && flag(w, 22))) return {};
            return opc == 0b11 ? Use().modify(rd(w)) : Use().write(rd(w));
        case 0b110:  // SBFM, BFM, UBFM; BFM keeps the bits outside the field
            if (opc == 0b11 || flag(w, 22) != sf || (!sf && (flag(w, 21) || flag(w, 15)))) return {};
            return opc == 0b01 ? Use().read(rn(w)).modify(rd(w)) : Use().read(rn(w)).write(rd(w));
        case 0b111:
            if (opc == 0b00) {  // EXTR
                if (flag(w, 21) || flag(w, 22) != sf || (!sf && flag(w, 15))) return {};
                return Use().read(rn(w)).read(rm(w)).write(rd(w));
            }
            if (opc == 0b11 && sf && !flag(w, 22) && rd(w) == zr) return Use().modify(lr);  // AUTIASPPC, AUTIBSPPC
            return {};
    }
    return {};
}

// --- Branches, exception generation and system instructions (op1 = 101x) ------------------------

/// HINT space, by CRm:op2. Unallocated hints execute as NOP.
RegisterUse hint(unsigned number) {
    switch (number) {
        case 7:  // XPACLRI
        case 24: case 25: case 26: case 27:  // PACIAZ, PACIASP, PACIBZ, PACIBSP
        case 28: case 29: case 30: case 31:  // AUTIAZ, AUTIASP, AUTIBZ, AUTIBSP
            return Use().modify(lr);
        case 8: case 10: case 12: case 14:  // PACIA1716, PACIB1716, AUTIA1716, AUTIB1716
            return Use().read(16).modify(17);
        case 40:  // CHKFEAT X16
            return Use().modify(16);
    }
    return {};
}

/// System instructions (bits 31:24 = 11010101). Whatever takes a register reads it when L is clear
/// and writes it when L is set: SYS, MSR, WFET, WFIT read, SYSL, MRS, TSTART, TTEST write. Hints,
/// barriers and PSTATE have Rt = 31. The decoding follows LLVM, which also takes op0 = 00
/// encodings that are nothing else for MSR and MRS.
RegisterUse system(std::uint32_t w) {
    const bool l = flag(w, 21);
    const auto t = rd(w);
    if (flag(w, 23)) {  // TCHANGEF, TCHANGEB; d is taken as read as well (to be safe)
        if (field(w, 22, 21) != 0 || flag(w, 19) || flag(w, 16)) return {};
        if (!flag(w, 20)) return field(w, 15, 10) == 0 ? Use().read(rn(w)).modify(t) : Use();
        return field(w, 16, 12) == 0 ? Use().modify(t) : Use();
    }
    if (!flag(w, 22)) {
        if (!l && field(w, 20, 12) == 0b00'011'0010 && t == zr) return hint(field(w, 11, 5));
        return l ? Use().write(t) : Use().read(t);
    }
    // 128-bit forms with a pair of registers from an even number: SYSP, MSRR (read), MRRS (write)
    if (t == zr || t % 2 != 0) return {};
    return l ? Use().write(t, 2) : Use().read(t, 2);
}

RegisterUse branch_register(std::uint32_t w) {
    const auto opc = field(w, 24, 21), op3 = field(w, 15, 10), n = rn(w), op4 = rd(w);
    if (field(w, 20, 16) != 0b11111) return {};
    const bool plain = op3 == 0 && op4 == 0;
    const bool pac = (op3 & 0b111110) == 0b000010;  // A and B key forms
    switch (opc) {
        case 0b0000:  // BR, BRAAZ, BRABZ
            return plain || (pac && op4 == zr) ? Use().read(n) : Use();
        case 0b0001:  // BLR, BLRAAZ, BLRABZ
            return plain || (pac && op4 == zr) ? Use().read(n).write(lr) : Use();
        case 0b0010:  // RET; RETAA, RETAB; RETAASPPCR, RETABSPPCR with a modifier register in op4
            if (plain) return Use().read(n);
            return pac && n == zr ? Use().read(lr).read(op4) : Use();
        case 0b1000:  // BRAA, BRAB
            return pac ? Use().read(n).read(op4) : Use();
        case 0b1001:  // BLRAA, BLRAB
            return pac ? Use().read(n).read(op4).write(lr) : Use();
    }
    return {};  // ERET, ERETAA, ERETAB, DRPS: no register
}

/// CB<cc>, CBB<cc>, CBH<cc> (CMPBR): compare two registers, or one with an immediate, and branch.
RegisterUse compare_and_branch(std::uint32_t w) {
    const auto cc = field(w, 23, 21);
    if (cc == 0b100 || cc == 0b101) return {};
    if (flag(w, 24)) return flag(w, 14) ? Use() : Use().read(rd(w));
    const auto kind = field(w, 15, 14);  // 00 word or doubleword, 10 byte, 11 halfword
    if (kind == 0b01 || (kind != 0 && flag(w, 31))) return {};
    return Use().read(rd(w)).read(rm(w));
}

RegisterUse branch_exception_system(std::uint32_t w) {
    if (field(w, 30, 26) == 0b00101) return flag(w, 31) ? Use().write(lr) : Use();  // B, BL
    if (field(w, 30, 25) == 0b011010 || field(w, 30, 25) == 0b011011) {             // CBZ, CBNZ, TBZ, TBNZ
        return Use().read(rd(w));
    }
    if (field(w, 30, 25) == 0b111010) return compare_and_branch(w);
    switch (field(w, 31, 24)) {
        case 0b0101'0101:  // RETAASPPC, RETABSPPC
            return field(w, 23, 22) == 0 && rd(w) == zr ? Use().read(lr) : Use();
        case 0b1101'0101:
            return system(w);
        case 0b1101'0110:
        case 0b1101'0111:
            return branch_register(w);
    }
    return {};  // B.cond, BC.cond, SVC, HVC, SMC, BRK, HLT, TCANCEL, DCPS: no register
}

// --- Loads and stores (op1 = x1x0) -------------------------------------------------------------

/// Load/store exclusive, load-acquire/store-release, compare and swap (bits 29:24 = 001000).
RegisterUse exclusive_ordered(std::uint32_t w) {
    const bool o2 = flag(w, 23), l = flag(w, 22), o1 = flag(w, 21);
    const auto s = rm(w), t = rd(w), t2 = ra(w), n = rn(w);
    if (!o2 && !o1) {  // LDXR, LDAXR / STXR, STLXR: s is the status register
        return l ? Use().read(n).write(t) : Use().read(n).read(t).write(s);
    }
    if (!o2) {
        if (flag(w, 31)) {  // LDXP, LDAXP / STXP, STLXP
            return l ? Use().read(n).write(t).write(t2) : Use().read(n).read(t).read(t2).write(s);
        }
        // CASP, CASPA, CASPL, CASPAL: two pairs of consecutive registers from even numbers
        if (t2 != zr || s % 2 != 0 || t % 2 != 0) return {};
        return Use().read(n).modify(s, 2).read(t, 2);
    }
    if (!o1) return l ? Use().read(n).write(t) : Use().read(n).read(t);  // LDAR, LDLAR / STLR, STLLR
    if (t2 != zr) return {};
    return Use().read(n).modify(s).read(t);  // CAS, CASA, CASL, CASAL
}

/// Unprivileged exclusives and compare and swap (LSUI), bits 29:24 = 001001: LDTXR, STTXR and
/// their ordered forms (bit 23 clear), CAST, CASPT and theirs (bit 23 set). LLVM ignores Rt2.
RegisterUse unprivileged_exclusive(std::uint32_t w) {
    const auto s = rm(w), t = rd(w), n = rn(w);
    if (flag(w, 21)) return {};
    if (!flag(w, 23)) {
        if (!flag(w, 31)) return {};
        return flag(w, 22) ? Use().read(n).write(t) : Use().read(n).read(t).write(s);
    }
    if (!flag(w, 30)) return {};  // 64-bit only
    if (flag(w, 31)) return Use().read(n).modify(s).read(t);
    if (s % 2 != 0 || t % 2 != 0) return {};
    return Use().read(n).modify(s, 2).read(t, 2);
}

/// Advanced SIMD load/store structures: only the base and the offset register are general-purpose.
RegisterUse simd_structure(std::uint32_t w) {
    if (flag(w, 31)) return {};
    const bool single = flag(w, 24), post = flag(w, 23), l = flag(w, 22), q = flag(w, 30);
    const auto m = rm(w), size = field(w, 11, 10);
    if (!single) {
        if (flag(w, 21) || (!post && m != 0)) return {};
        switch (field(w, 15, 12)) {
            case 0b0000:  // LD4, ST4
            case 0b0100:  // LD3, ST3
            case 0b1000:  // LD2, ST2
                if (size == 0b11 && !q) return {};
                break;
            case 0b0010: case 0b0110: case 0b0111: case 0b1010:  // LD1, ST1 with 4, 3, 1, 2 registers
                break;
            default:
                return {};
        }
    } else {
        // LDAP1, STL1 (LRCPC3): one doubleword lane, Rm = 00001 in the no-offset form
        if (!post && m == 1 && !flag(w, 21) && field(w, 15, 10) == 0b100001) return Use().read(rn(w));
        if (!post && m != 0) return {};
        const bool s = flag(w, 12);
        switch (field(w, 15, 13)) {
            case 0b000: case 0b001:  // bytes
                break;
            case 0b010: case 0b011:  // halfwords
                if (size & 1) return {};
                break;
            case 0b100: case 0b101:  // words, doublewords
                if (size >= 0b10 || (size == 0b01 && s)) return {};
                break;
            default:  // LD1R, LD2R, LD3R, LD4R
                if (!l || s) return {};
                break;
        }
    }
    auto use = address(rn(w), post);
    if (post) use.read(m);  // m = 31: the offset is the size of the transfer
    return use;
}

/// Memory copy and memory set (MOPS). Every register is updated as the operation proceeds,
/// except the value of SET.
/// The three registers must differ. The size (n) may be xzr, and the value of SET (s); the
/// addresses may not.
RegisterUse memory_copy_set(std::uint32_t w) {
    const auto d = rd(w), s = rm(w), n = rn(w);
    if (field(w, 31, 30) != 0 || d == zr || d == n || d == s || s == n) return {};
    if (field(w, 23, 22) == 0b11) {  // SETP, SETM, SETE and SETG*
        if (field(w, 15, 14) == 0b11) return {};
        return Use().modify(d).modify(n).read(s);
    }
    return s == zr ? Use() : Use().modify(d).modify(s).modify(n);  // CPYF*, CPY*
}

/// Load/store memory tags (MTE), bits 31:24 = 11011001 and bit 21 set.
RegisterUse memory_tags(std::uint32_t w) {
    const auto opc = field(w, 23, 22), mode = field(w, 11, 10);
    const auto n = rn(w), t = rd(w);
    if (mode == 0b00) {
        const bool offset = field(w, 20, 12) != 0;
        switch (opc) {
            case 0b00: return offset ? Use() : Use().read(n).read(t);   // STZGM
            case 0b01: return Use().read(n).modify(t);                  // LDG: inserts the tag into t
            case 0b10: return offset ? Use() : Use().read(n).read(t);   // STGM
            case 0b11: return offset ? Use() : Use().read(n).write(t);  // LDGM
        }
    }
    // STG, STZG, ST2G, STZ2G: post-index (01), signed offset (10), pre-index (11)
    return address(n, mode != 0b10).read(t);
}

/// Bits 29:24 = 011001 (V = 0) or 011101 (V = 1): LRCPC 2 and 3, MOPS, MTE, LSE128, THE, LSUI, GCS.
RegisterUse load_store_011x01(std::uint32_t w) {
    const bool v = flag(w, 26);
    const auto size = field(w, 31, 30), opc = field(w, 23, 22);
    const auto n = rn(w), t = rd(w), s = rm(w);
    if (!flag(w, 21)) {
        switch (field(w, 11, 10)) {
            case 0b00:  // STLUR, LDAPUR, LDAPURS* (LRCPC2)
                if (v) return {};
                if (opc == 0b00) return Use().read(n).read(t);
                if (opc == 0b01 || (opc == 0b10 && size != 0b11) || (opc == 0b11 && size <= 0b01)) {
                    return Use().read(n).write(t);
                }
                return {};
            case 0b01:
                return memory_copy_set(w);
            case 0b10:  // STILP, LDIAPP, STLR (pre-index), LDAPR (post-index) (LRCPC3)
                if (v) return opc >= 0b10 && size != 0b00 ? Use() : Use().read(n);  // STLUR, LDAPUR (SIMD)
                if (size < 0b10) return {};
                if (opc <= 0b01) {
                    const auto opc2 = field(w, 15, 12);
                    if (opc2 > 0b0001) return {};
                    auto use = address(n, opc2 == 0);
                    return opc == 0b01 ? use.write(t).write(s) : use.read(t).read(s);
                }
                if (s != 0 || field(w, 15, 12) != 0) return {};
                return opc == 0b11 ? address(n, true).write(t) : address(n, true).read(t);
            case 0b11:  // GCSSTR, GCSSTTR
                if (v || size != 0b11 || opc != 0 || s != zr || field(w, 15, 13) != 0) return {};
                return Use().read(n).read(t);
        }
    }
    if (v) return {};
    if (size == 0b11) return memory_tags(w);
    const bool o3 = flag(w, 15);
    const auto op = field(w, 14, 12);
    switch (field(w, 11, 10)) {
        case 0b00:  // LDCLRP, LDSETP, SWPP (LSE128), RCWCLRP, RCWSWPP, RCWSETP (THE): pairs, both updated
            if (size > 0b01 || t == zr || s == zr) return {};
            if (!o3 ? (op != 0b001 && op != 0b011) : op > 0b011) return {};
            if (size == 0b01 && (!o3 || op == 0b000)) return {};  // the soft forms: RCWS*P only
            return Use().read(n).modify(t).modify(s);
        case 0b01:  // LDTADD, LDTCLR, LDTSET, SWPT (LSUI)
            if (flag(w, 31)) return {};
            switch (field(w, 15, 12)) {
                case 0b0000: case 0b0001: case 0b0011: case 0b1000: return Use().read(n).read(s).write(t);
            }
            return {};
        case 0b10:  // RCWCAS, RCWSCAS (THE)
            if (size > 0b01 || field(w, 15, 12) != 0) return {};
            return Use().read(n).modify(s).read(t);
        case 0b11:  // RCWCASP, RCWSCASP (THE)
            if (size > 0b01 || field(w, 15, 12) != 0 || s % 2 != 0 || t % 2 != 0) return {};
            return Use().read(n).modify(s, 2).read(t, 2);
    }
    return {};
}

/// Load/store pair: no-allocate (bits 24:23 = 00), post-index, signed offset, pre-index.
RegisterUse load_store_pair(std::uint32_t w) {
    const auto opc = field(w, 31, 30), mode = field(w, 24, 23);
    const bool v = flag(w, 26), l = flag(w, 22);
    auto use = address(rn(w), mode == 0b01 || mode == 0b11);
    if (v) return use;  // S, D, Q registers
    if (opc == 0b01 && mode == 0b00) return {};
    // opc 00: W, 10: X, 01: STGP / LDPSW, 11: STTP, LDTP, STTNP, LDTNP (LSUI)
    return l ? use.write(rd(w)).write(ra(w)) : use.read(rd(w)).read(ra(w));
}

enum class Mode { unscaled, post_index, unprivileged, pre_index, register_offset, unsigned_offset };

/// Single-register loads and stores of every addressing mode.
RegisterUse load_store_register(std::uint32_t w, Mode mode) {
    const auto size = field(w, 31, 30), opc = field(w, 23, 22);
    const bool v = flag(w, 26);
    auto use = address(rn(w), mode == Mode::post_index || mode == Mode::pre_index);
    if (mode == Mode::register_offset) {
        if (!flag(w, 14)) return {};  // option<1> = 0: no such extend
        use.read(rm(w));
    }
    if (v) {  // B, H, S, D, Q registers
        if (mode == Mode::unprivileged || (opc >= 0b10 && size != 0b00)) return {};
        return use;
    }
    const auto t = rd(w);
    switch (opc) {
        case 0b00: return use.read(t);   // STRB, STRH, STR
        case 0b01: return use.write(t);  // LDRB, LDRH, LDR
        case 0b10:
            if (size != 0b11) return use.write(t);  // LDRSB, LDRSH, LDRSW into an X register
            // PRFM, PRFUM, RPRFM: t names the prefetch operation
            return mode == Mode::unscaled || mode == Mode::register_offset || mode == Mode::unsigned_offset ? use
                                                                                                             : Use();
        default:
            return size <= 0b01 ? use.write(t) : Use();  // LDRSB, LDRSH into a W register
    }
}

/// Atomic memory operations (LSE, LRCPC, LS64, THE), bit 21 set and bits 11:10 = 00.
RegisterUse atomic(std::uint32_t w) {
    const auto size = field(w, 31, 30);
    const bool a = flag(w, 23), r = flag(w, 22), o3 = flag(w, 15);
    const auto op = field(w, 14, 12), s = rm(w), t = rd(w), n = rn(w);
    if (flag(w, 26)) {  // LDF{ADD,MAX,MIN,MAXNM,MINNM}, their BF16 and store forms (LSFE)
        if (op == 0b001 || op == 0b010 || op == 0b011 || (o3 && (t != zr || a))) return {};  // stores: no acquire
        return Use().read(n);
    }
    if (!o3) return Use().read(n).read(s).write(t);  // LDADD, LDCLR, LDEOR, LDSET, LD{S,U}{MAX,MIN}
    const bool ls64 = size == 0b11 && !a && !r && t % 2 == 0 && t <= 22;
    switch (op) {
        case 0b000:  // SWP
            return Use().read(n).read(s).write(t);
        case 0b001:  // ST64B; RCWCLR, RCWSCLR
        case 0b010:  // ST64BV0; RCWSWP, RCWSSWP
        case 0b011:  // ST64BV; RCWSET, RCWSSET
            if (size <= 0b01) return Use().read(n).read(s).write(t);
            if (!ls64) return {};
            if (op == 0b001) return s == zr ? Use().read(n).read(t, 8) : Use();
            return Use().read(n).read(t, 8).write(s);
        case 0b100:  // LDAPRB, LDAPRH, LDAPR
            return a && !r && s == zr ? Use().read(n).write(t) : Use();
        case 0b101:  // LD64B
            return ls64 && s == zr ? Use().read(n).write(t, 8) : Use();
    }
    return {};
}

/// LDRAA, LDRAB: bit 21 set, bit 10 set, bit 11 the writeback.
RegisterUse load_authenticated(std::uint32_t w) {
    if (field(w, 31, 30) != 0b11 || flag(w, 26)) return {};
    return address(rn(w), flag(w, 11)).write(rd(w));
}

RegisterUse load_store(std::uint32_t w) {
    const bool v = flag(w, 26);
    switch (field(w, 29, 27)) {
        case 0b001:
            if (v) return simd_structure(w);
            return flag(w, 24) ? unprivileged_exclusive(w) : exclusive_ordered(w);
        case 0b011:
            if (flag(w, 24)) return load_store_011x01(w);
            // LDR (literal), LDRSW (literal); PRFM (literal) names an operation, SIMD ones no GPR
            return !v && field(w, 31, 30) != 0b11 ? Use().write(rd(w)) : Use();
        case 0b101:
            return load_store_pair(w);
        case 0b111:
            if (flag(w, 24)) return load_store_register(w, Mode::unsigned_offset);
            if (!flag(w, 21)) {
                constexpr Mode modes[] = {Mode::unscaled, Mode::post_index, Mode::unprivileged, Mode::pre_index};
                return load_store_register(w, modes[field(w, 11, 10)]);
            }
            switch (field(w, 11, 10)) {
                case 0b00: return atomic(w);
                case 0b10: return load_store_register(w, Mode::register_offset);
                default: return load_authenticated(w);
            }
    }
    return {};
}

// --- Data processing, register (op1 = x101) ----------------------------------------------------

RegisterUse data_processing_2_source(std::uint32_t w) {
    const bool sf = flag(w, 31), s = flag(w, 29);
    const auto opcode = field(w, 15, 10);
    auto use = Use().read(rn(w)).read(rm(w)).write(rd(w));
    if (s) return opcode == 0 && sf ? use : Use();  // SUBPS
    switch (opcode) {
        case 0b000000:  // SUBP
        case 0b000100:  // IRG
        case 0b000101:  // GMI
        case 0b001100:  // PACGA
            return sf ? use : Use();
        case 0b000010: case 0b000011:                                // UDIV, SDIV
        case 0b001000: case 0b001001: case 0b001010: case 0b001011:  // LSLV, LSRV, ASRV, RORV
        case 0b011000: case 0b011001: case 0b011010: case 0b011011:  // SMAX, UMAX, SMIN, UMIN (CSSC)
            return use;
        case 0b010000: case 0b010001: case 0b010010:  // CRC32B, CRC32H, CRC32W
        case 0b010100: case 0b010101: case 0b010110:  // CRC32CB, CRC32CH, CRC32CW
            return sf ? Use() : use;
        case 0b010011: case 0b010111:  // CRC32X, CRC32CX
            return sf ? use : Use();
    }
    return {};
}

RegisterUse data_processing_1_source(std::uint32_t w) {
    const bool sf = flag(w, 31);
    const auto opcode2 = field(w, 20, 16), opcode = field(w, 15, 10), n = rn(w), d = rd(w);
    if (flag(w, 29)) return {};
    if (opcode2 == 0) {
        // RBIT, REV16, REV32 or REV, REV, CLZ, CLS, CTZ, CNT, ABS (CSSC)
        if (opcode > 0b001000 || (opcode == 0b000011 && !sf)) return {};
        return Use().read(n).write(d);
    }
    if (opcode2 != 0b00001 || !sf) return {};
    // Pointer authentication: the pointer in d is both input and output.
    if (opcode <= 0b000111) return Use().read(n).modify(d);                         // PACIA .. AUTDB
    if (opcode <= 0b001111) return n == zr ? Use().modify(d) : Use();               // PACIZA .. AUTDZB
    if (opcode <= 0b010001) return n == zr ? Use().modify(d) : Use();               // XPACI, XPACD
    // PAuth_LR, all with d = x30
    if (d != lr) return {};
    switch (opcode) {
        case 0b100000: case 0b100001:  // PACNBIASPPC, PACNBIBSPPC
        case 0b101000: case 0b101001:  // PACIASPPC, PACIBSPPC
            return n == zr ? Use().modify(lr) : Use();
        case 0b100100: case 0b100101:  // AUTIASPPCR, AUTIBSPPCR
            return Use().read(n).modify(lr);
        case 0b100010: case 0b100011:  // PACIA171615, PACIB171615
        case 0b101110: case 0b101111:  // AUTIA171615, AUTIB171615
            return n == zr ? Use().read(15).read(16).modify(17) : Use();
    }
    return {};
}

RegisterUse data_processing_3_source(std::uint32_t w) {
    const bool sf = flag(w, 31), o0 = flag(w, 15);
    const auto op31 = field(w, 23, 21);
    auto use = Use().read(rn(w)).read(rm(w)).write(rd(w));
    if (field(w, 30, 29) != 0) return {};
    switch (op31) {
        case 0b000:  // MADD, MSUB
            return use.read(ra(w));
        case 0b001: case 0b101:  // SMADDL, SMSUBL, UMADDL, UMSUBL
            return sf ? use.read(ra(w)) : Use();
        case 0b010: case 0b110:  // SMULH, UMULH: Ra is not an operand
            return sf && !o0 ? use : Use();
        case 0b011:  // MADDPT, MSUBPT (CPA)
            return sf ? use.read(ra(w)) : Use();
    }
    return {};
}

RegisterUse data_processing_register(std::uint32_t w) {
    const bool sf = flag(w, 31);
    const auto op2 = field(w, 24, 21), op3 = field(w, 15, 10);
    auto three = Use().read(rn(w)).read(rm(w)).write(rd(w));
    if (!flag(w, 28)) {
        if (!flag(w, 24)) {  // logical (shifted register)
            return !sf && flag(w, 15) ? Use() : three;
        }
        if (!flag(w, 21)) {  // add/subtract (shifted register)
            return field(w, 23, 22) == 0b11 || (!sf && flag(w, 15)) ? Use() : three;
        }
        // add/subtract (extended register)
        return field(w, 23, 22) != 0 || field(w, 12, 10) > 4 ? Use() : three;
    }
    if (flag(w, 24)) return data_processing_3_source(w);
    const auto top = field(w, 31, 29);  // sf, op, S
    switch (op2) {
        case 0b0000:
            if (op3 == 0) return three;  // ADC, ADCS, SBC, SBCS
            if ((op3 & 0b011111) == 0b000001) {  // RMIF
                return top == 0b101 && !flag(w, 4) ? Use().read(rn(w)) : Use();
            }
            if ((op3 & 0b101111) == 0b000010) {  // SETF8, SETF16
                return top == 0b001 && rm(w) == 0 && field(w, 4, 0) == 0b01101 ? Use().read(rn(w)) : Use();
            }
            if ((op3 & 0b111000) == 0b001000) {  // ADDPT, SUBPT (CPA)
                return sf && !flag(w, 29) ? three : Use();
            }
            return {};
        case 0b0010:  // CCMN, CCMP (register, bit 11 clear; immediate, bit 11 set)
            if (!flag(w, 29) || flag(w, 10) || flag(w, 4)) return {};
            return flag(w, 11) ? Use().read(rn(w)) : Use().read(rn(w)).read(rm(w));
        case 0b0100:  // CSEL, CSINC, CSINV, CSNEG
            return flag(w, 29) || flag(w, 11) ? Use() : three;
        case 0b0110:
            return flag(w, 30) ? data_processing_1_source(w) : data_processing_2_source(w);
    }
    return {};
}

// --- Scalar floating-point and Advanced SIMD (op1 = x111) --------------------------------------
// Only the moves and conversions between general-purpose and SIMD registers name the former.

RegisterUse float_integer_conversion(std::uint32_t w) {
    const bool sf = flag(w, 31);
    const auto ftype = field(w, 23, 22), rmode = field(w, 20, 19), opcode = field(w, 18, 16);
    if (ftype == 0b10) {  // FMOV Xd, Vn.D[1] and FMOV Vd.D[1], Xn
        if (!sf || rmode != 0b01) return {};
        if (opcode == 0b110) return Use().write(rd(w));
        return opcode == 0b111 ? Use().read(rn(w)) : Use();
    }
    const bool fmov_size = ftype == 0b11 || sf == (ftype == 0b01);
    switch (opcode) {
        case 0b000: case 0b001:  // FCVTNS, FCVTNU, FCVTPS, FCVTPU, FCVTMS, FCVTMU, FCVTZS, FCVTZU
            return Use().write(rd(w));
        case 0b010: case 0b011:  // SCVTF, UCVTF
            return rmode == 0 ? Use().read(rn(w)) : Use();
        case 0b100: case 0b101:  // FCVTAS, FCVTAU
            return rmode == 0 ? Use().write(rd(w)) : Use();
        case 0b110:  // FMOV to a general-purpose register, FJCVTZS
            if (rmode == 0) return fmov_size ? Use().write(rd(w)) : Use();
            return rmode == 0b11 && !sf && ftype == 0b01 ? Use().write(rd(w)) : Use();
        default:  // FMOV from a general-purpose register
            return rmode == 0 && fmov_size ? Use().read(rn(w)) : Use();
    }
}

RegisterUse float_fixed_conversion(std::uint32_t w) {
    const auto ftype = field(w, 23, 22), rmode = field(w, 20, 19), opcode = field(w, 18, 16);
    if (ftype == 0b10 || (!flag(w, 31) && !flag(w, 15))) return {};
    if (rmode == 0b11 && opcode <= 0b001) return Use().write(rd(w));                   // FCVTZS, FCVTZU
    if (rmode == 0b00 && (opcode == 0b010 || opcode == 0b011)) return Use().read(rn(w));  // SCVTF, UCVTF
    return {};
}

/// DUP (general), INS (general), SMOV, UMOV.
RegisterUse simd_copy(std::uint32_t w) {
    const bool q = flag(w, 30);
    const auto imm5 = field(w, 20, 16);
    if (flag(w, 29) || (imm5 & 0b01111) == 0) return {};
    unsigned size = 0;  // 0 B, 1 H, 2 S, 3 D
    while (((imm5 >> size) & 1) == 0) ++size;
    switch (field(w, 14, 11)) {
        case 0b0001:  // DUP (general)
            return size == 3 && !q ? Use() : Use().read(rn(w));
        case 0b0011:  // INS (general)
            return q ? Use().read(rn(w)) : Use();
        case 0b0101:  // SMOV
            return size == 3 || (size == 2 && !q) ? Use() : Use().write(rd(w));
        case 0b0111:  // UMOV
            return q == (size == 3) ? Use().write(rd(w)) : Use();
    }
    return {};
}

RegisterUse floating_point_simd(std::uint32_t w) {
    if (!flag(w, 30) && field(w, 28, 24) == 0b11110) {
        if (flag(w, 29)) return {};
        if (!flag(w, 21)) return float_fixed_conversion(w);
        return field(w, 15, 10) == 0 ? float_integer_conversion(w) : Use();
    }
    if (!flag(w, 31) && field(w, 28, 21) == 0b0111'0000 && !flag(w, 15) && flag(w, 10)) return simd_copy(w);
    return {};
}

}  // namespace

RegisterUse register_use(std::uint32_t instruction) {
    const auto op1 = field(instruction, 28, 25);
    if ((op1 & 0b1110) == 0b1000) return data_processing_immediate(instruction);
    if ((op1 & 0b1110) == 0b1010) return branch_exception_system(instruction);
    if ((op1 & 0b0101) == 0b0100) return load_store(instruction);
    if ((op1 & 0b0111) == 0b0101) return data_processing_register(instruction);
    if ((op1 & 0b0111) == 0b0111) return floating_point_simd(instruction);
    return {};  // reserved, SME, SVE, unallocated
}

}  // namespace apk::a64
