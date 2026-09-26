// Probes for host_call_test.cpp (ADR-0030, decision 1): guest code that calls host functions the way a
// front lib's stubs do (tools/frontgen): the host function from its slot of a table into x16, the entry
// from slot 0 into x17, and a branch to the entry.
//
// uint64_t probe_stub_1(...), probe_stub_2(...), probe_stub_3(...)
//   Plain stubs for the slots 1 to 3 of probe_table: a tail branch into the entry, which returns to the
//   stub's caller. The arguments stay where the caller put them, those on the stack included.
//
// uint64_t probe_host_call(uint64_t a0, ..., uint64_t a7)
//   Calls probe_stub_1 from a frame of its own, with x29 = sp at the call; probe_host_call_return is
//   the word after the call, where the caller's state goes on.
//
// void probe_host_call_state(const struct probe_state *before, struct probe_state *after)
//   Loads x0-x15, x19-x29 and v0-v31 from `before` and calls probe_stub_1; then stores every register
//   into `after`: x[30] the stack pointer before the call, x[31] the one after it. probe_state has the
//   layout of the syscall frame (syscall_entry_probe.S). x16 and x17 belong to the stub, x18 to XNU.
//
// probe_host_clobber: a host function that returns probe_results (x0, x1, v0-v3) and leaves other
//   values in every other register a host function may change: x2-x17 and v4-v7, v16-v31.

.macro PROBE_STUB slot
    .p2align 2
    .globl _probe_stub_\slot
_probe_stub_\slot:
    adrp    x17, _probe_table@PAGE
    add     x17, x17, _probe_table@PAGEOFF
    ldr     x16, [x17, #(8 * \slot)]
    ldr     x17, [x17]
    br      x17
.endm

    .text
    PROBE_STUB 1
    PROBE_STUB 2
    PROBE_STUB 3

    .p2align 2
    .globl _probe_host_call
_probe_host_call:
    stp     x29, x30, [sp, #-16]!
    mov     x29, sp
    bl      _probe_stub_1
    .globl _probe_host_call_return
_probe_host_call_return:
    ldp     x29, x30, [sp], #16
    ret

    .p2align 2
    .globl _probe_host_call_state
_probe_host_call_state:
    stp     x29, x30, [sp, #-16]!
    mov     x29, sp
    stp     x27, x28, [sp, #-16]!
    stp     x25, x26, [sp, #-16]!
    stp     x23, x24, [sp, #-16]!
    stp     x21, x22, [sp, #-16]!
    stp     x19, x20, [sp, #-16]!
    stp     d14, d15, [sp, #-16]!
    stp     d12, d13, [sp, #-16]!
    stp     d10, d11, [sp, #-16]!
    stp     d8, d9, [sp, #-16]!
    stp     x0, x1, [sp, #-16]!             // before, after
    mov     x9, sp
    str     x9, [x1, #240]                  // after.x[30]: the stack pointer at the call

    ldp     q0, q1, [x0, #272]
    ldp     q2, q3, [x0, #304]
    ldp     q4, q5, [x0, #336]
    ldp     q6, q7, [x0, #368]
    ldp     q8, q9, [x0, #400]
    ldp     q10, q11, [x0, #432]
    ldp     q12, q13, [x0, #464]
    ldp     q14, q15, [x0, #496]
    ldp     q16, q17, [x0, #528]
    ldp     q18, q19, [x0, #560]
    ldp     q20, q21, [x0, #592]
    ldp     q22, q23, [x0, #624]
    ldp     q24, q25, [x0, #656]
    ldp     q26, q27, [x0, #688]
    ldp     q28, q29, [x0, #720]
    ldp     q30, q31, [x0, #752]
    mov     x16, x0
    ldp     x0, x1, [x16, #0]
    ldp     x2, x3, [x16, #16]
    ldp     x4, x5, [x16, #32]
    ldp     x6, x7, [x16, #48]
    ldp     x8, x9, [x16, #64]
    ldp     x10, x11, [x16, #80]
    ldp     x12, x13, [x16, #96]
    ldp     x14, x15, [x16, #112]
    ldp     x19, x20, [x16, #152]
    ldp     x21, x22, [x16, #168]
    ldp     x23, x24, [x16, #184]
    ldp     x25, x26, [x16, #200]
    ldp     x27, x28, [x16, #216]
    ldr     x29, [x16, #232]

    bl      _probe_stub_1

    ldr     x16, [sp, #8]                   // after; x16 and x17 are free after a call
    stp     x0, x1, [x16, #0]
    stp     x2, x3, [x16, #16]
    stp     x4, x5, [x16, #32]
    stp     x6, x7, [x16, #48]
    stp     x8, x9, [x16, #64]
    stp     x10, x11, [x16, #80]
    stp     x12, x13, [x16, #96]
    stp     x14, x15, [x16, #112]
    stp     x19, x20, [x16, #152]
    stp     x21, x22, [x16, #168]
    stp     x23, x24, [x16, #184]
    stp     x25, x26, [x16, #200]
    stp     x27, x28, [x16, #216]
    str     x29, [x16, #232]
    mov     x17, sp
    str     x17, [x16, #248]                // after.x[31]: the stack pointer after the call
    stp     q0, q1, [x16, #272]
    stp     q2, q3, [x16, #304]
    stp     q4, q5, [x16, #336]
    stp     q6, q7, [x16, #368]
    stp     q8, q9, [x16, #400]
    stp     q10, q11, [x16, #432]
    stp     q12, q13, [x16, #464]
    stp     q14, q15, [x16, #496]
    stp     q16, q17, [x16, #528]
    stp     q18, q19, [x16, #560]
    stp     q20, q21, [x16, #592]
    stp     q22, q23, [x16, #624]
    stp     q24, q25, [x16, #656]
    stp     q26, q27, [x16, #688]
    stp     q28, q29, [x16, #720]
    stp     q30, q31, [x16, #752]

    add     sp, sp, #16                     // before, after
    ldp     d8, d9, [sp], #16
    ldp     d10, d11, [sp], #16
    ldp     d12, d13, [sp], #16
    ldp     d14, d15, [sp], #16
    ldp     x19, x20, [sp], #16
    ldp     x21, x22, [sp], #16
    ldp     x23, x24, [sp], #16
    ldp     x25, x26, [sp], #16
    ldp     x27, x28, [sp], #16
    ldp     x29, x30, [sp], #16
    ret

// probe_results: x0 and x1 at 0, v0-v3 at 16.
    .p2align 2
    .globl _probe_host_clobber
_probe_host_clobber:
    adrp    x9, _probe_results@PAGE
    add     x9, x9, _probe_results@PAGEOFF
    ldp     q0, q1, [x9, #16]
    ldp     q2, q3, [x9, #48]
    ldp     x0, x1, [x9, #0]
    mov     x2, #0x2222
    mov     x3, #0x3333
    mov     x4, #0x4444
    mov     x5, #0x5555
    mov     x6, #0x6666
    mov     x7, #0x7777
    mov     x8, #0x8888
    mov     x9, #0x9999
    mov     x10, #0xaaaa
    mov     x11, #0xbbbb
    mov     x12, #0xcccc
    mov     x13, #0xdddd
    mov     x14, #0xeeee
    mov     x15, #0xffff
    mov     x16, #0x1616
    mov     x17, #0x1717
    movi    v4.16b, #0x44
    movi    v5.16b, #0x55
    movi    v6.16b, #0x66
    movi    v7.16b, #0x77
    movi    v16.16b, #0x16
    movi    v17.16b, #0x17
    movi    v18.16b, #0x18
    movi    v19.16b, #0x19
    movi    v20.16b, #0x20
    movi    v21.16b, #0x21
    movi    v22.16b, #0x22
    movi    v23.16b, #0x23
    movi    v24.16b, #0x24
    movi    v25.16b, #0x25
    movi    v26.16b, #0x26
    movi    v27.16b, #0x27
    movi    v28.16b, #0x28
    movi    v29.16b, #0x29
    movi    v30.16b, #0x30
    movi    v31.16b, #0x31
    ret
