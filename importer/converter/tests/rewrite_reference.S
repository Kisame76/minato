// Reference code for rewrite_test.cpp, assembled and linked by the host toolchain (Apple syntax).
// The rewriter must emit exactly these words, including the branch and page offsets the linker
// resolves, so the tests compare against a real assembler instead of hand-computed constants.

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

    .data
    .p2align 3
    .globl _ref_syscall_slot
_ref_syscall_slot:
    .quad   0
