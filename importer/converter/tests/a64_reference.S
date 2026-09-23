// Reference cases for a64_test.cpp, assembled by the host assembler (Apple syntax). Each case is
// one instruction and the general-purpose registers it reads and writes, taken from the Arm ARM;
// the assembler supplies the encoding, so the test does not depend on hand-made constants.
// x18 appears in every role it can take, next to registers that must not be mistaken for it:
// v18, d18, q18, the prefetch operation 18 (PSTL2KEEP), and SVE instructions.

    .arch armv9.6-a
    .arch_extension mops
    .arch_extension cssc
    .arch_extension ls64
    .arch_extension lse128
    .arch_extension the
    .arch_extension rcpc3
    .arch_extension gcs
    .arch_extension d128
    .arch_extension cmpbr
    .arch_extension cpa
    .arch_extension tme
    .arch_extension lsui
    .arch_extension memtag
    .arch_extension sve
    .arch_extension ite

    .set X0, 1 << 0
    .set X1, 1 << 1
    .set X2, 1 << 2
    .set X3, 1 << 3
    .set X4, 1 << 4
    .set X5, 1 << 5
    .set X6, 1 << 6
    .set X7, 1 << 7
    .set X12, 1 << 12
    .set X13, 1 << 13
    .set X14, 1 << 14
    .set X15, 1 << 15
    .set X16, 1 << 16
    .set X17, 1 << 17
    .set X18, 1 << 18
    .set X19, 1 << 19
    .set X20, 1 << 20
    .set X21, 1 << 21
    .set X29, 1 << 29
    .set X30, 1 << 30
    .set NONE, 0

// One case: the registers read, the registers written, the instruction.
    .macro case reads, writes, instruction:vararg
    .section __DATA,__a64_expect
    .long \reads, \writes
    .text
    \instruction
    .endm

    .section __DATA,__a64_expect
    .p2align 2
    .globl _a64_expect
_a64_expect:

    .text
    .p2align 2
    .globl _a64_cases
_a64_cases:

// --- Data processing, immediate ---------------------------------------------------------------
    case NONE, X18, adr x18, 1f
    case NONE, X18, adrp x18, 1f@PAGE
    case X1, X18, add x18, x1, #1
    case X18, X1, add x1, x18, #1
    case X18, NONE, add sp, x18, #16
    case NONE, X18, mov x18, sp
    case X18, X0, adds x0, x18, #1, lsl #12
    case X18, NONE, cmp x18, #5
    case X18, NONE, cmn w18, #5
    case X1, X18, addg x18, x1, #16, #1
    case X18, X1, subg x1, x18, #16, #1
    case X1, X18, smax x18, x1, #3
    case X18, X1, umin w1, w18, #3
    case X1, X18, and x18, x1, #0xff
    case X18, X1, orr x1, x18, #1
    case NONE, X18, mov x18, #0x5555555555555555
    case X18, NONE, tst x18, #1
    case X18, X18, eor w18, w18, #1
    case NONE, X18, movz x18, #1
    case NONE, X18, movn w18, #1
    case X18, X18, movk x18, #1, lsl #16
    case X1, X18, sbfx x18, x1, #1, #2
    case X18, X1, ubfiz w1, w18, #1, #2
    case X1|X18, X18, bfi x18, x1, #1, #2
    case X18|X1, X1, bfxil x1, x18, #1, #2
    case X1, X18, lsl x18, x1, #3
    case X1, X18, sxtw x18, w1
    case X1|X2, X18, extr x18, x1, x2, #3
    case X18, X1, ror w1, w18, #3

// --- Branches, exception generation, system ----------------------------------------------------
    case NONE, NONE, b 1f
    case NONE, X30, bl 1f
    case NONE, NONE, b.eq 1f
    case NONE, NONE, bc.ne 1f
    case X18, NONE, cbz x18, 1f
    case X18, NONE, cbnz w18, 1f
    case X18, NONE, tbz x18, #3, 1f
    case X18, NONE, tbnz w18, #3, 1f
    case X18|X1, NONE, cbgt x18, x1, .
    case X1, NONE, cbgt x1, #3, .
    case X18|X1, NONE, cbbgt w18, w1, .
    case X1|X18, NONE, cbhgt w1, w18, .
    case X18, NONE, br x18
    case X18, X30, blr x18
    case X30, X30, blr x30
    case X30, NONE, ret
    case X18, NONE, ret x18
    case X18, NONE, braaz x18
    case X18, X30, blraaz x18
    case X18|X1, NONE, braa x18, x1
    case X1, NONE, braa x1, sp
    case X18|X1, X30, blrab x18, x1
    case X30, NONE, retaa
    case X30, NONE, retab
    case NONE, NONE, eret
    case NONE, NONE, svc #0
    case NONE, NONE, brk #1
    case NONE, NONE, udf #18
    case NONE, NONE, nop
    case X30, X30, xpaclri
    case X16|X17, X17, pacia1716
    case X16|X17, X17, autib1716
    case X30, X30, paciasp
    case X30, X30, autibz
    case NONE, NONE, bti c
    case X16, X16, hint #40         // CHKFEAT X16
    case NONE, NONE, csdb
    case NONE, NONE, hint #9
    case NONE, NONE, dmb ish
    case NONE, NONE, isb
    case NONE, NONE, clrex
    case NONE, NONE, sb
    case NONE, NONE, cfinv
    case NONE, NONE, msr ssbs, #1
    case X18, NONE, wfet x18
    case X18, NONE, wfit x18
    case NONE, X18, tstart x18
    case NONE, X18, ttest x18
    case NONE, NONE, tcommit
    case NONE, NONE, tcancel #1
    case NONE, X18, mrs x18, TPIDR_EL0
    case X18, NONE, msr TPIDR_EL0, x18
    case NONE, X1, mrs x1, NZCV
    case X18, NONE, msr FPCR, x18
    case NONE, NONE, msr TPIDR_EL0, xzr
    case X18, NONE, dc civac, x18
    case X18, NONE, ic ivau, x18
    case X18, NONE, tlbi vae1, x18
    case NONE, NONE, ic iallu
    case NONE, X18, sysl x18, #0, c1, c2, #3
    case X18, NONE, gcspushm x18
    case NONE, X18, gcspopm x18
    case X18, NONE, trcit x18
    case X18|X19, NONE, sysp #0, c2, c0, #0, x18, x19
    case NONE, X18|X19, mrrs x18, x19, S3_0_C2_C0_0
    case X18|X19, NONE, msrr S3_0_C2_C0_0, x18, x19

// --- Loads and stores --------------------------------------------------------------------------
// Exclusive, ordered, compare and swap
    case X1, X18, ldxr x18, [x1]
    case X1|X2, X18, stxr w18, x1, [x2]
    case X18|X2, X1, stxr w1, x18, [x2]
    case X1, X18, ldaxrb w18, [x1]
    case X18, X1, stlxrh w1, w18, [sp]
    case X1, X18|X19, ldxp x18, x19, [x1]
    case X18|X19|X2, X1, stxp w1, x18, x19, [x2]
    case X18, X1|X17, ldaxp w1, w17, [x18]
    case X1, X18, ldar x18, [x1]
    case X18|X1, NONE, stlr x18, [x1]
    case X1|X18, NONE, stlrb w1, [x18]
    case NONE, X18, ldlar x18, [sp]
    case X18|X1, NONE, stllr x18, [x1]
    case X18|X1|X2, X18, cas x18, x1, [x2]
    case X1|X18|X2, X1, casal w1, w18, [x2]
    case X18|X1, X18, casb w18, w1, [sp]
    case X18|X19|X0|X1|X2, X18|X19, casp x18, x19, x0, x1, [x2]
    case X0|X1|X18|X19|X2, X0|X1, caspal x0, x1, x18, x19, [x2]
    case X18|X19|X0, X18, cast x18, x19, [x0]
    case X18|X19|X20|X21|X0, X18|X19, caspt x18, x19, x20, x21, [x0]
// SIMD structures: only the base and the offset register are general-purpose
    case X18, NONE, ld1 {v18.16b}, [x18]
    case X18, X18, st1 {v0.4s, v1.4s}, [x18], #32
    case X1|X18, X1, ld1 {v0.2d}, [x1], x18
    case X0, NONE, ld4 {v18.4s, v19.4s, v20.4s, v21.4s}, [x0]
    case X18, NONE, ld1 {v0.s}[1], [x18]
    case X1|X18, X1, st1 {v0.b}[3], [x1], x18
    case X18, X18, ld1r {v0.4s}, [x18], #4
    case X1|X18, X1, ld4r {v0.2d, v1.2d, v2.2d, v3.2d}, [x1], x18
// Literal loads; PRFM names a prefetch operation, not a register
    case NONE, X18, ldr x18, 1f
    case NONE, X18, ldrsw x18, 1f
    case NONE, NONE, ldr q18, 1f
    case NONE, NONE, prfm pstl2keep, 1f
// Memory copy and set (MOPS): every register is updated, except the value of SET
    case X0|X1|X18, X0|X1|X18, cpyfp [x0]!, [x1]!, x18!
    case X18|X1|X2, X18|X1|X2, cpye [x18]!, [x1]!, x2!
    case X18|X1|X2, X18|X1, setp [x18]!, x1!, x2
    case X0|X1|X18, X0|X1, setgm [x0]!, x1!, x18
// LRCPC 2 and 3, GCS
    case X1, X18, ldapur x18, [x1, #-1]
    case X18|X1, NONE, stlur w18, [x1]
    case X1, X18, ldapursw x18, [x1]
    case X18, X1, ldapursb w1, [x18]
    case X0, X18|X19, ldiapp x18, x19, [x0]
    case X18, X0|X1|X18, ldiapp x0, x1, [x18], #16
    case X18|X19|X0, X0, stilp x18, x19, [x0, #-16]!
    case X0, X18|X0, ldapr x18, [x0], #8
    case X18|X0, X0, stlr x18, [x0, #-8]!
    case X18|X0, NONE, gcsstr x18, [x0]
    case X1|X18, NONE, gcssttr x1, [x18]
// Memory tags (MTE); STG and friends store the tag of their first register
    case X18|X1, NONE, stg x18, [x1]
    case X1|X18, X18, stg x1, [x18, #16]!
    case X18, X18, stzg sp, [x18], #16
    case X18, NONE, st2g x18, [sp, #-32]
    case X18|X1, NONE, stz2g x18, [x1]
    case X18|X1, X18, ldg x18, [x1]
    case X18|X1, NONE, stgm x18, [x1]
    case X1|X18, NONE, stzgm x1, [x18]
    case X1, X18, ldgm x18, [x1]
    case X18|X19|X0, NONE, stgp x18, x19, [x0]
    case X0|X1|X18, X18, stgp x0, x1, [x18, #16]!
// 128-bit atomics (LSE128), read-check-write (THE), unprivileged (LSUI)
    case X18|X19|X0, X18|X19, ldclrp x18, x19, [x0]
    case X0|X1|X18, X0|X1, swppal x0, x1, [x18]
    case X18|X1|X2, X18, rcwcas x18, x1, [x2]
    case X18|X19|X20|X21|X2, X18|X19, rcwcasp x18, x19, x20, x21, [x2]
    case X18|X2, X1, rcwswp x18, x1, [x2]
    case X1|X2, X18, rcwsclr x1, x18, [x2]
    case X18|X19|X2, X18|X19, rcwsetp x18, x19, [x2]
    case X18|X0, X19, swpt w18, w19, [x0]
    case X18|X0, X19, ldtadd x18, x19, [x0]
    case X0, X18|X19, ldtp x18, x19, [x0]
    case X18|X19|X0, X0, sttp x18, x19, [x0, #16]!
    case X0, X18|X19, ldtnp x18, x19, [x0]
// Pairs
    case X0, X18|X19, ldp x18, x19, [x0]
    case X18|X19|X0, NONE, stp x18, x19, [x0, #0x90]
    case X29|X30, NONE, stp x29, x30, [sp, #-16]!
    case NONE, X29|X30, ldp x29, x30, [sp], #16
    case X2, X18|X1, ldpsw x18, x1, [x2]
    case X2, X18|X1, ldnp w18, w1, [x2]
    case X1|X18|X2, NONE, stnp x1, x18, [x2]
    case X18, X18, ldp q18, q19, [x18], #32
    case X18, X18, stp d0, d1, [x18, #-16]!
    case X18, NONE, ldnp s0, s1, [x18]
// Single registers
    case X1, X18, ldr x18, [x1]
    case X18, X1, ldr x1, [x18, #8]
    case X18|X1, X1, str x18, [x1, #8]!
    case X30|X18, X18, str x30, [x18], #8        // ShadowCallStack prologue
    case X18, X30|X18, ldr x30, [x18, #-8]!      // ShadowCallStack epilogue
    case X1, X18, ldur x18, [x1, #-1]
    case X18, NONE, sturb w18, [sp, #1]
    case X1, X18, ldtr x18, [x1]
    case X18|X1, NONE, sttrh w18, [x1]
    case X1, X18, ldrsw x18, [x1, #4]
    case X1, X18|X1, ldrsb w18, [x1], #1
    case X18, X1|X18, ldrsh x1, [x18, #-2]!
    case X2|X18, X1, ldr x1, [x2, x18]
    case X2|X18, X1, ldr x1, [x2, w18, sxtw #3]
    case X18|X1|X2, NONE, str w18, [x1, x2, lsl #2]
    case X18, NONE, prfm pldl1keep, [x18]
    case X1, NONE, prfm pstl2keep, [x1]
    case X1, NONE, prfum pstl2keep, [x1, #-1]
    case X1|X18, NONE, prfm pstl2keep, [x1, x18]
    case X18, NONE, ldr q18, [x18]
    case X1|X18, NONE, str d18, [x1, x18]
    case X18, X18, ldr b0, [x18], #1
    case X1, X18, ldraa x18, [x1]
    case X18, X1|X18, ldrab x1, [x18, #8]!
// Atomics (LSE, LRCPC), LS64 with its eight consecutive registers
    case X18|X2, X1, ldadd x18, x1, [x2]
    case X18|X2, NONE, stadd x18, [x2]
    case X1|X2, X18, ldsmaxal w1, w18, [x2]
    case X18|X1, X18, swp x18, x18, [x1]
    case X1, X18, swpal w1, w18, [sp]
    case X1, X18, ldaprb w18, [x1]
    case X18, X1, ldapr x1, [x18]
    case X12|X13|X14|X15|X16|X17|X18|X19|X0, NONE, st64b x12, [x0]
    case X18, X12|X13|X14|X15|X16|X17|X18|X19, ld64b x12, [x18]
    case X0|X1|X2|X3|X4|X5|X6|X7|X1, X18, st64bv x18, x0, [x1]
    case X14|X15|X16|X17|X18|X19|X20|X21|X2, X1, st64bv0 x1, x14, [x2]

// --- Data processing, register -----------------------------------------------------------------
    case X1|X2, X18, add x18, x1, x2
    case X18|X2, X1, add x1, x18, x2, lsl #3
    case X2|X18, X1, sub w1, w2, w18, asr #3
    case X1, X18, neg x18, x1
    case X1|X18, NONE, cmp x1, x18
    case X1, X18, add x18, sp, w1, uxtw
    case X18|X1, NONE, add sp, x18, x1, uxtx #2
    case X18|X2, X1, adds x1, x18, w2, sxtw
    case X18|X1, NONE, cmn x18, w1, uxtb
    case X1|X2, X18, and x18, x1, x2
    case X1, X18, mov x18, x1
    case X19, X18, mov x18, x19                  // bionic, __init_additional_stacks
    case X1, X18, mvn w18, w1
    case X18|X2, X1, orr x1, x18, x2, ror #3
    case X18|X1, NONE, tst x18, x1
    case X18|X2, X1, bics x1, x18, x2
    case X1|X2, X18, adc x18, x1, x2
    case X18|X2, X1, sbcs w1, w18, w2
    case X1, X18, ngc x18, x1
    case X18|X1, X0, addpt x0, x18, x1
    case X1, X18, subpt x18, sp, x1
    case X18|X1|X2, X0, maddpt x0, x18, x1, x2
    case X18, NONE, rmif x18, #3, #2
    case X18, NONE, setf8 w18
    case X18, NONE, setf16 w18
    case X18|X1, NONE, ccmp x18, x1, #0, eq
    case X1, NONE, ccmn w1, #3, #4, ne
    case X1|X2, X18, csel x18, x1, x2, eq
    case X18|X2, X1, csinc w1, w18, w2, ne
    case NONE, X18, cset x18, eq
    case X18, X1, cneg x1, x18, mi
    case NONE, X18, csetm w18, lt
    case X1|X2, X18, udiv x18, x1, x2
    case X18|X2, X1, sdiv w1, w18, w2
    case X2|X18, X1, lsl x1, x2, x18
    case X18|X1, X18, rorv w18, w18, w1
    case X1|X2, X18, crc32b w18, w1, w2
    case X2|X18, X1, crc32cx w1, w2, x18
    case X1, X18, pacga x18, x1, sp
    case X1|X2, X18, smax x18, x1, x2
    case X2|X18, X1, umin w1, w2, w18
    case X1|X2, X18, irg x18, x1, x2
    case X18|X2, X1, gmi x1, x18, x2
    case X1|X2, X18, subp x18, x1, x2
    case X18|X2, X1, subps x1, x18, x2
    case X18|X1, NONE, cmpp x18, x1
    case X1, X18, rbit x18, x1
    case X18, X1, rev w1, w18
    case X18, X18, rev16 x18, x18
    case X18, X1, rev32 x1, x18
    case X1, X18, clz x18, x1
    case X18, X1, cls w1, w18
    case X1, X18, ctz x18, x1
    case X18, X1, cnt w1, w18
    case X1, X18, abs x18, x1
    case X18|X1, X18, pacia x18, x1
    case X1, X1, pacia x1, sp
    case X1|X18, X1, autdb x1, x18
    case X18, X18, paciza x18
    case X18, X18, autdzb x18
    case X18, X18, xpaci x18
    case X1, X1, xpacd x1
    case X1|X2|X3, X18, madd x18, x1, x2, x3
    case X2|X3|X18, X1, msub x1, x2, x3, x18
    case X1|X2, X18, mul x18, x1, x2
    case X1|X2|X3, X18, smaddl x18, w1, w2, x3
    case X18|X2, X1, umulh x1, x18, x2
    case X1|X2, X18, smulh x18, x1, x2
    case X2|X18, X1, mneg x1, x2, x18
    case X2|X3|X18, X1, umsubl x1, w2, w3, x18

// --- FP and SIMD: only moves and conversions name general-purpose registers ---------------------
    case NONE, X18, fmov x18, d0
    case X18, NONE, fmov d0, x18
    case NONE, X18, fmov w18, s0
    case X18, NONE, fmov s0, w18
    case NONE, X18, fmov w18, h0
    case X18, NONE, fmov h0, x18
    case NONE, X18, fmov x18, v0.d[1]
    case X18, NONE, fmov v0.d[1], x18
    case NONE, X18, fcvtzs x18, d0
    case NONE, X18, fcvtzu w18, s0
    case NONE, X18, fcvtas x18, h0
    case NONE, X18, fcvtmu w18, d0
    case X18, NONE, scvtf d0, x18
    case X18, NONE, ucvtf s0, w18
    case NONE, X18, fjcvtzs w18, d0
    case NONE, X18, fcvtzs x18, d0, #3
    case X18, NONE, scvtf s0, w18, #3
    case X18, NONE, ucvtf h0, x18, #3
    case X18, NONE, dup v0.4s, w18
    case X18, NONE, dup v0.2d, x18
    case X18, NONE, mov v0.s[1], w18
    case NONE, X18, umov w18, v0.b[1]
    case NONE, X18, mov x18, v0.d[1]
    case NONE, X18, smov x18, v0.s[1]
    case NONE, X18, smov w18, v0.h[1]
    case NONE, NONE, mov v0.s[1], v18.s[0]
    case NONE, NONE, dup v0.4s, v18.s[1]
    case NONE, NONE, add v18.4s, v18.4s, v18.4s
    case NONE, NONE, fadd d18, d18, d18
    case NONE, NONE, fmov d18, #1.0
    case NONE, NONE, fcsel s18, s1, s2, eq

// --- SVE: left out, it cannot run (AT_HWCAP does not offer it) -----------------------------------
    case NONE, NONE, ld1w {z0.s}, p0/z, [x18]
    case NONE, NONE, cntd x18

    .globl _a64_cases_end
_a64_cases_end:
1:
    nop
