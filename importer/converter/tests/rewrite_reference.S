// Reference code for rewrite_test.cpp, assembled and linked by the host toolchain (Apple syntax).
// The rewriter must emit exactly these words, including the branch and page offsets the linker
// resolves, so the tests compare against a real assembler instead of hand-computed constants.

    // The x18 forms below include extensions: MTE, CPA, LSUI, CMPBR, LS64, LRCPC3.
    .arch armv9.6-a
    .arch_extension memtag
    .arch_extension cpa
    .arch_extension lsui
    .arch_extension cmpbr
    .arch_extension ls64
    .arch_extension rcpc3

    .text
    .p2align 2

// Island for `mrs x5, TPIDR_EL0`, without its final branch back.
    .globl _ref_thread_pointer_read_x5
_ref_thread_pointer_read_x5:
    mrs     x5, TPIDRRO_EL0
    bic     x5, x5, #7
    ldr     x5, [x5, #48]

// Island for `msr TPIDR_EL0, x7`, scratch register x16, without its final branch back.
    .globl _ref_thread_pointer_write_x7
_ref_thread_pointer_write_x7:
    str     x16, [sp, #-16]!
    mrs     x16, TPIDRRO_EL0
    bic     x16, x16, #7
    str     x7, [x16, #48]
    ldr     x16, [sp], #16

// Island for `msr TPIDR_EL0, x16`: the scratch register moves to x17.
    .globl _ref_thread_pointer_write_x16
_ref_thread_pointer_write_x16:
    str     x17, [sp, #-16]!
    mrs     x17, TPIDRRO_EL0
    bic     x17, x17, #7
    str     x16, [x17, #48]
    ldr     x17, [sp], #16

// Island for `msr TPIDR_EL0, xzr`.
    .globl _ref_thread_pointer_write_xzr
_ref_thread_pointer_write_xzr:
    str     x16, [sp, #-16]!
    mrs     x16, TPIDRRO_EL0
    bic     x16, x16, #7
    str     xzr, [x16, #48]
    ldr     x16, [sp], #16

// Veneer for `svc #0`, complete: the slot and the branch back are resolved by the linker.
    .globl _ref_syscall_veneer
_ref_syscall_veneer:
    stp     x16, x30, [sp, #-16]!
    adrp    x16, _ref_syscall_slot@PAGE
    ldr     x16, [x16, _ref_syscall_slot@PAGEOFF]
    blr     x16
    ldp     x16, x30, [sp], #16
    b       _ref_after_site

// x18 islands (ADR-0020), complete with the branch back: the island runs the instruction with a
// scratch register in place of x18, whose value lives in Darwin TSD slot 11 (offset 88). Scratch
// registers are saved in the red zone, at sp - 128 and sp - 120, without moving sp.
// `add x18, x18, x1`: reads and writes x18; scratch x0, TSD base in x2.
    .globl _ref_x18_island_read_write
_ref_x18_island_read_write:
    stur    x0, [sp, #-128]
    stur    x2, [sp, #-120]
    mrs     x0, TPIDRRO_EL0
    bic     x0, x0, #7
    ldr     x0, [x0, #88]
    add     x0, x0, x1
    mrs     x2, TPIDRRO_EL0
    bic     x2, x2, #7
    str     x0, [x2, #88]
    ldur    x0, [sp, #-128]
    ldur    x2, [sp, #-120]
    b       _ref_after_site

// `str x18, [x0]`: reads x18 only; scratch x1.
    .globl _ref_x18_island_read
_ref_x18_island_read:
    stur    x1, [sp, #-128]
    mrs     x1, TPIDRRO_EL0
    bic     x1, x1, #7
    ldr     x1, [x1, #88]
    str     x1, [x0]
    ldur    x1, [sp, #-128]
    b       _ref_after_site

// `mov x18, x19`: writes x18 only; scratch x0, TSD base in x1.
    .globl _ref_x18_island_write
_ref_x18_island_write:
    stur    x0, [sp, #-128]
    stur    x1, [sp, #-120]
    mov     x0, x19
    mrs     x1, TPIDRRO_EL0
    bic     x1, x1, #7
    str     x0, [x1, #88]
    ldur    x0, [sp, #-128]
    ldur    x1, [sp, #-120]
    b       _ref_after_site

// A site as the rewriter leaves it: a branch to its stub.
    .globl _ref_site
_ref_site:
    b       _ref_syscall_veneer
    .globl _ref_after_site
_ref_after_site:
    nop

// What the scanner must find, in this order, and what it must leave alone.
    .globl _ref_patterns
_ref_patterns:
    mrs     x3, TPIDR_EL0           // thread pointer read, x3
    msr     TPIDR_EL0, x9           // thread pointer write, x9
    svc     #0                      // Linux syscall
    mrs     x30, TPIDR_EL0          // thread pointer read, x30
    msr     TPIDR_EL0, xzr          // thread pointer write, xzr
    svc     #0x80                   // not a Linux syscall: reported, not rewritten
    mrs     x3, TPIDRRO_EL0         // Darwin's own thread pointer: left alone
    mrs     x3, S3_3_C13_C0_5       // TPIDR2_EL0 (SME): left alone
    msr     S3_3_C13_C0_5, x3       // TPIDR2_EL0 (SME): left alone
    mrs     x3, TPIDR_EL1           // kernel register, cannot appear in user code, left alone
    hvc     #0                      // not svc
    brk     #0                      // not svc
    nop
    .globl _ref_patterns_end
_ref_patterns_end:

// x18 (F4, ADR-0019): what reads it, what only writes it, and register dumps that store it.
    .globl _ref_x18
_ref_x18:
    mov     x18, x19                // write (bionic, __init_additional_stacks)
    ldp     x18, x19, [x0, #0x90]   // write (libunwind, jumpto)
    str     x30, [x18], #8          // read (ShadowCallStack prologue)
    add     x0, x18, #1             // read
    stp     x16, x17, [x0, #0x80]   // register dump, ascending (libunwind, unw_getcontext)
    stp     x18, x19, [x0, #0x90]   // save
    stp     x20, x21, [x0, #0xa0]
    stp     x20, x21, [sp, #0xa0]   // register dump, descending (hwasan)
    stp     x18, x19, [sp, #0x90]   // save
    stp     x16, x17, [sp, #0x80]
    stp     x18, x19, [x0, #0x90]   // read: the neighbours store elsewhere
    stp     x20, x21, [x1, #0xa0]
    stp     x16, x17, [x0, #0x70]
    stp     x18, x19, [x0, #0x90]   // read: the neighbours are not x16 and x20 at -16 and +16
    fmov    d18, x0                 // neither: d18 is no general-purpose register
    prfm    pstl2keep, [x0]         // neither: the operation 18 is no register
    .globl _ref_x18_end
_ref_x18_end:

// x18 next to register 31 in forms an island handles (ADR-0020): 31 as xzr, sp read but not moved,
// sp as the base of an access at or above it. Every word goes into an island.
    .globl _ref_x18_islands
_ref_x18_islands:
    mov     x18, x19                // orr x18, xzr, x19
    orr     x18, xzr, #0xff         // logical immediate: Rn is xzr
    cmp     x18, #1                 // subs xzr, x18, #1: with S set, Rd is xzr
    cmp     x18, x1, uxtx           // extended register, S set: xzr
    tst     x18, #1                 // ands xzr, x18, #1
    add     x18, sp, #16            // sp read, not written
    mov     x18, sp
    add     x18, sp, x1
    ldr     x18, [sp, #8]           // unsigned offset
    str     w18, [sp, #12]
    ldur    x18, [sp, #0x1e]        // unscaled, not negative (libunity.so)
    ldr     x18, [sp, x1]           // register offset
    stp     x18, x19, [sp, #16]
    ldp     x0, x18, [sp]
    stnp    x18, x19, [sp, #16]
    ldar    x18, [sp]               // no offset
    str     x30, [x18], #8          // writeback to x18, not to sp (ShadowCallStack)

// x18 in forms no island handles, grouped by the reason; every word is refused.
// sp moves, and with it the red zone where the island keeps its registers.
    .globl _ref_x18_moves_sp
_ref_x18_moves_sp:
    mov     sp, x18                 // add sp, x18, #0
    sub     sp, x18, #16
    add     sp, sp, x18             // extended register
    and     sp, x18, #~15           // logical immediate: Rd is sp
    addg    sp, x18, #16, #1
    irg     sp, x18
    addpt   sp, sp, x18
    ldr     x18, [sp], #16          // post-index
    str     x18, [sp, #-16]!        // pre-index
    stp     x18, x19, [sp, #-16]!
    ldp     x18, x19, [sp], #16
    ldraa   x18, [sp, #8]!
    stlr    x18, [sp, #-8]!
    ld1     {v0.16b}, [sp], x18     // x18 as the post-index offset
// Below sp, where the island keeps its registers. Linux code never goes there: it has no red zone.
    .globl _ref_x18_below_sp
_ref_x18_below_sp:
    stur    x18, [sp, #-8]
    sttr    x18, [sp, #-8]
    stp     x18, x19, [sp, #-16]
    ldnp    x18, x19, [sp, #-16]
    ldraa   x18, [sp, #-8]
    stlur   x18, [sp, #-8]
// Branches with a register the island cannot follow: it leaves through them and never returns to
// restore the scratch. cbz, cbnz, tbz and tbnz get a two-way island of their own (below), and br x18
// gets landing pads when it ends a jump table (further below); here it ends none.
    .globl _ref_x18_branches
_ref_x18_branches:
    br      x18
    blr     x18
    ret     x18
    braa    x18, x0
    braa    x0, x18                 // x18 as the modifier
// The thread pointer moved through x18: the island of either register would lose the other.
    .globl _ref_x18_thread_pointer
_ref_x18_thread_pointer:
    mrs     x18, TPIDR_EL0
    msr     TPIDR_EL0, x18
// x18 without a register field of its own: half of a pair, one of the eight registers of ST64B.
    .globl _ref_x18_without_field
_ref_x18_without_field:
    casp    x18, x19, x0, x1, [x2]
    st64b   x16, [x0]
    .globl _ref_x18_unsupported_end
_ref_x18_unsupported_end:

// An LL/SC loop, with x18 before, inside and after it.
    .globl _ref_x18_ll_sc
_ref_x18_ll_sc:
    add     x18, x18, #1            // island: before the loop
1:  ldaxr   x0, [x1]
    add     x18, x0, #1             // refused: between the load- and the store-exclusive
    stlxr   w2, x18, [x1]           // refused: a store-exclusive
    cbnz    w2, 1b
    add     x18, x18, #1            // island: after the loop
    ldxr    x0, [x1]
    clrex                           // ends the sequence as well
    add     x18, x18, #2            // island
    .globl _ref_x18_ll_sc_end
_ref_x18_ll_sc_end:

// ADR-0020 X4: a signal inside a relocated window sends the thread back to its load-exclusive, so
// the rewriter relocates only windows that run the same when they run again from there. First nine
// that do not, then three that do.
    .globl _ref_x18_window_exit
_ref_x18_window_exit:               // a branch leaves the window, which only reads x18
    ldaxr   x8, [x18]
    cmp     x8, x1
    b.ne    1f
    stlxr   w9, x2, [x18]
    cbnz    w9, _ref_x18_window_exit
    ret
1:  clrex
    ret
    .globl _ref_x18_window_input
_ref_x18_window_input:              // writes x1, which it reads first
    ldxr    x8, [x18]
    add     x8, x8, x1
    add     x1, x1, #1
    stxr    w9, x8, [x18]
    cbnz    w9, _ref_x18_window_input
    ret
    .globl _ref_x18_window_flags
_ref_x18_window_flags:              // reads the flags before it sets them, and then sets them
    ldxr    x8, [x18]
    csel    x8, x8, x1, eq
    cmp     x8, #0
    stxr    w9, x8, [x18]
    cbnz    w9, _ref_x18_window_flags
    ret
    .globl _ref_x18_window_memory
_ref_x18_window_memory:             // another memory access inside the window
    ldxr    x8, [x18]
    ldr     x10, [x0]
    stxr    w9, x8, [x18]
    cbnz    w9, _ref_x18_window_memory
    ret
    .globl _ref_x18_window_syscall
_ref_x18_window_syscall:            // an svc, which the island would copy unchanged
    ldxr    x8, [x18]
    svc     #0
    stxr    w9, x8, [x18]
    cbnz    w9, _ref_x18_window_syscall
    ret
    .globl _ref_x18_window_simd
_ref_x18_window_simd:               // SIMD registers, which the check cannot follow
    ldxr    x8, [x18]
    fmov    d0, x8
    fmov    x8, d0
    stxr    w9, x8, [x18]
    cbnz    w9, _ref_x18_window_simd
    ret
    .globl _ref_x18_window_exit_next
_ref_x18_window_exit_next:          // a branch to the word right after the window leaves it too
    ldaxr   x8, [x18]
    cmp     x8, x1
    b.ne    1f
    stlxr   w9, x2, [x18]
1:  cbnz    w9, _ref_x18_window_exit_next
    ret
    .globl _ref_x18_window_base
_ref_x18_window_base:               // changes the base register of its load-exclusive
    ldxr    x8, [x0]
    mov     x0, x18
    stxr    w9, x8, [x18]
    cbnz    w9, _ref_x18_window_base
    ret
    .globl _ref_x18_window_call
_ref_x18_window_call:               // a bl, whose return address would point into the island
    ldxr    x8, [x18]
    bl      1f
1:  stxr    w9, x8, [x18]
    cbnz    w9, _ref_x18_window_call
    ret
    .globl _ref_x18_window_flags_read
_ref_x18_window_flags_read:         // runs the same again: reads the flags, never changes them
    ldxr    x8, [x18]
    csel    x8, x8, x1, eq
    stxr    w9, x8, [x18]
    cbnz    w9, _ref_x18_window_flags_read
    ret
    .globl _ref_x18_window_skipped_write
_ref_x18_window_skipped_write:      // runs the same again, but the cbz can skip the write of x18
    ldxr    x8, [x0]
    cbz     x8, 1f
    mov     x18, x8
1:  stxr    w9, x18, [x0]
    cbnz    w9, _ref_x18_window_skipped_write
    ret
    .globl _ref_x18_window_status
_ref_x18_window_status:             // runs the same again: only the store-exclusive writes w9
    ldxr    x8, [x18]
    add     x8, x8, x9
    stxr    w9, x8, [x18]
    cbnz    w9, _ref_x18_window_status
    ret
    .globl _ref_x18_window_end
_ref_x18_window_end:

// A window a CLREX closes without a store-exclusive: the CLREX completes it (ADR-0020 X4).
    .globl _ref_x18_window_clrex_only
_ref_x18_window_clrex_only:
    ldaxr   x8, [x18]
    cmp     x8, x1
    clrex
    ret
    .globl _ref_x18_window_clrex_only_end
_ref_x18_window_clrex_only_end:

// A window whose cbz can skip its only write of x18, and which never reads x18: on that path the
// store after the window writes back whatever the value register holds (found in the review of X4).
    .globl _ref_x18_window_unread
_ref_x18_window_unread:
    ldxr    x8, [x0]
    cbz     x8, 1f
    mov     x18, x8
1:  stxr    w9, x8, [x0]
    cbnz    w9, _ref_x18_window_unread
    ret
    .globl _ref_x18_window_unread_end
_ref_x18_window_unread_end:

// Two-way island for a conditional branch (ADR-0020 X2): the value scratch (x0 here) holds the
// guest x18 from slot 11, the branch tests it, and both exits restore the scratch before leaving.
// `back` is the instruction after the site, `taken` the branch's own target; the rewriter
// recomputes each for the container, so the test checks those two branches by their target.
// `cbz x18, .+0x40`:
    .globl _ref_x18_island_cbz
_ref_x18_island_cbz:
    stur    x0, [sp, #-128]
    mrs     x0, TPIDRRO_EL0
    bic     x0, x0, #7
    ldr     x0, [x0, #88]
    cbz     x0, 1f
    ldur    x0, [sp, #-128]
    b       _ref_after_site
1:  ldur    x0, [sp, #-128]
    b       _ref_x18_cbz_taken
_ref_x18_cbz_taken:
    nop
// `tbnz x18, #33, .+0x40`: the test-bit branch encodes its target in 14 bits, not 19.
    .globl _ref_x18_island_tbz
_ref_x18_island_tbz:
    stur    x0, [sp, #-128]
    mrs     x0, TPIDRRO_EL0
    bic     x0, x0, #7
    ldr     x0, [x0, #88]
    tbnz    x0, #33, 1f
    ldur    x0, [sp, #-128]
    b       _ref_after_site
1:  ldur    x0, [sp, #-128]
    b       _ref_x18_tbz_taken
_ref_x18_tbz_taken:
    nop

// PC-relative forms on x18 (ADR-0020 X2): the island computes the same address for its own place,
// always with ADRP (then ADD, then the load), since ADR and the literal loads reach only 1 MiB and
// the islands lie before the image. The sites are assembled here too, so that the linker resolves
// the original displacement as well as the island's, and the islands start on a page of their own:
// an ADRP left as it was would then point to the wrong page. x18 is only written, so x0 takes the
// value and x1 the TSD base.
    .globl _ref_x18_site_adrp
_ref_x18_site_adrp:
    adrp    x18, _ref_syscall_slot@PAGE     // a page in __DATA
_ref_x18_after_adrp:
    nop
    .globl _ref_x18_site_adr
_ref_x18_site_adr:
    adr     x18, _ref_x18_literal
_ref_x18_after_adr:
    nop
// The loads from a literal keep their size and extension: LDR X, LDR W, LDRSW.
    .globl _ref_x18_site_ldr
_ref_x18_site_ldr:
    ldr     x18, _ref_x18_literal
_ref_x18_after_ldr:
    nop
    .globl _ref_x18_site_ldr_w
_ref_x18_site_ldr_w:
    ldr     w18, _ref_x18_literal
_ref_x18_after_ldr_w:
    nop
    .globl _ref_x18_site_ldrsw
_ref_x18_site_ldrsw:
    ldrsw   x18, _ref_x18_literal
_ref_x18_after_ldrsw:
    nop

    .p2align 12
    .globl _ref_x18_island_adrp
_ref_x18_island_adrp:
    stur    x0, [sp, #-128]
    stur    x1, [sp, #-120]
    adrp    x0, _ref_syscall_slot@PAGE
    mrs     x1, TPIDRRO_EL0
    bic     x1, x1, #7
    str     x0, [x1, #88]
    ldur    x0, [sp, #-128]
    ldur    x1, [sp, #-120]
    b       _ref_x18_after_adrp

    .globl _ref_x18_island_adr
_ref_x18_island_adr:
    stur    x0, [sp, #-128]
    stur    x1, [sp, #-120]
    adrp    x0, _ref_x18_literal@PAGE
    add     x0, x0, _ref_x18_literal@PAGEOFF
    mrs     x1, TPIDRRO_EL0
    bic     x1, x1, #7
    str     x0, [x1, #88]
    ldur    x0, [sp, #-128]
    ldur    x1, [sp, #-120]
    b       _ref_x18_after_adr

    .globl _ref_x18_island_ldr
_ref_x18_island_ldr:
    stur    x0, [sp, #-128]
    stur    x1, [sp, #-120]
    adrp    x0, _ref_x18_literal@PAGE
    add     x0, x0, _ref_x18_literal@PAGEOFF
    ldr     x0, [x0]
    mrs     x1, TPIDRRO_EL0
    bic     x1, x1, #7
    str     x0, [x1, #88]
    ldur    x0, [sp, #-128]
    ldur    x1, [sp, #-120]
    b       _ref_x18_after_ldr

    .globl _ref_x18_island_ldr_w
_ref_x18_island_ldr_w:
    stur    x0, [sp, #-128]
    stur    x1, [sp, #-120]
    adrp    x0, _ref_x18_literal@PAGE
    add     x0, x0, _ref_x18_literal@PAGEOFF
    ldr     w0, [x0]
    mrs     x1, TPIDRRO_EL0
    bic     x1, x1, #7
    str     x0, [x1, #88]
    ldur    x0, [sp, #-128]
    ldur    x1, [sp, #-120]
    b       _ref_x18_after_ldr_w

    .globl _ref_x18_island_ldrsw
_ref_x18_island_ldrsw:
    stur    x0, [sp, #-128]
    stur    x1, [sp, #-120]
    adrp    x0, _ref_x18_literal@PAGE
    add     x0, x0, _ref_x18_literal@PAGEOFF
    ldrsw   x0, [x0]
    mrs     x1, TPIDRRO_EL0
    bic     x1, x1, #7
    str     x0, [x1, #88]
    ldur    x0, [sp, #-128]
    ldur    x1, [sp, #-120]
    b       _ref_x18_after_ldrsw

// A jump table through x18 (ADR-0020 X2d), shaped as clang 8 builds one: the bounds check, the base
// from ADRP and ADD, then LDRSW, ADD and BR on x18. The table sits in read-only data. Its last entry
// repeats a target, and the word between case1 and case2 is none, so its landing pad traps.
    .globl _ref_x18_jump_function
_ref_x18_jump_function:
    cmp     w18, #3
    b.hi    1f
    adrp    x9, _ref_x18_jump_table@PAGE
    add     x9, x9, _ref_x18_jump_table@PAGEOFF
    ldrsw   x18, [x9, x18, lsl #2]
    add     x18, x18, x9
    .globl _ref_x18_jump_site
_ref_x18_jump_site:
    br      x18
    .globl _ref_x18_jump_case0
_ref_x18_jump_case0:
    nop
    .globl _ref_x18_jump_case1
_ref_x18_jump_case1:
    nop
    nop
    .globl _ref_x18_jump_case2
_ref_x18_jump_case2:
    nop
1:  ret
    .globl _ref_x18_jump_function_end
_ref_x18_jump_function_end:

    .section __TEXT,__const
    .p2align 2
    .globl _ref_x18_jump_table
_ref_x18_jump_table:
    .long   _ref_x18_jump_case0 - _ref_x18_jump_table
    .long   _ref_x18_jump_case1 - _ref_x18_jump_table
    .long   _ref_x18_jump_case2 - _ref_x18_jump_table
    .long   _ref_x18_jump_case1 - _ref_x18_jump_table
    .text
    .p2align 2

// Its island: T from slot 11, T - lo checked against a span of 16 bytes (the mask keeps bits 4 to 63
// and 0 to 1), then one landing pad of 16 bytes per word from case0 to case2.
    .globl _ref_x18_island_jump
_ref_x18_island_jump:
    stur    x0, [sp, #-128]
    stur    x1, [sp, #-120]
    mrs     x0, TPIDRRO_EL0
    bic     x0, x0, #7
    ldr     x0, [x0, #88]
    adrp    x1, _ref_x18_jump_case0@PAGE
    add     x1, x1, _ref_x18_jump_case0@PAGEOFF
    sub     x0, x0, x1
    and     x1, x0, #0xfffffffffffffff3
    cbnz    x1, 1f
    adr     x1, 2f
    add     x0, x1, x0, lsl #2
    br      x0
1:  ldur    x0, [sp, #-128]         // no landing pad: the registers back, then stop
    ldur    x1, [sp, #-120]
    brk     #0x18
2:  ldur    x0, [sp, #-128]         // case0
    ldur    x1, [sp, #-120]
    b       _ref_x18_jump_case0
    brk     #0x18
    ldur    x0, [sp, #-128]         // case1
    ldur    x1, [sp, #-120]
    b       _ref_x18_jump_case1
    brk     #0x18
    ldur    x0, [sp, #-128]         // the word after case1: no target
    ldur    x1, [sp, #-120]
    brk     #0x18
    brk     #0x18
    ldur    x0, [sp, #-128]         // case2
    ldur    x1, [sp, #-120]
    b       _ref_x18_jump_case2
    brk     #0x18
    .globl _ref_x18_island_jump_end
_ref_x18_island_jump_end:

    .p2align 3
_ref_x18_literal:
    .quad   0x1122334455667788

    .data
    .p2align 3
    .globl _ref_syscall_slot
_ref_syscall_slot:
    .quad   0
