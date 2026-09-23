// Probe for syscall_entry_test.c: calls the syscall entry the way an svc veneer in guest code
// does, with every register set to a known value, and stores all registers afterwards.
//
//   void probe_syscall_entry(uint64_t number, const struct probe_state *before,
//                            struct probe_state *after);
//
// struct probe_state { uint64_t x[32]; uint64_t nzcv; uint64_t fpsr; __uint128_t q[32]; }
// x[16], x[18] and x[30] are not compared: the veneer owns x16 and x30, XNU owns x18 (F4).

    .text
    .p2align 2
    .globl _probe_syscall_entry
_probe_syscall_entry:
    // Keep the callee-saved registers of the C caller and the two pointers.
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
    stp     x1, x2, [sp, #-16]!     // before, after
    str     x0, [sp, #-16]!         // the syscall number

    // Load the state to call with: SIMD registers, flags, then the general registers.
    ldp     q0, q1, [x1, #272]
    ldp     q2, q3, [x1, #304]
    ldp     q4, q5, [x1, #336]
    ldp     q6, q7, [x1, #368]
    ldp     q8, q9, [x1, #400]
    ldp     q10, q11, [x1, #432]
    ldp     q12, q13, [x1, #464]
    ldp     q14, q15, [x1, #496]
    ldp     q16, q17, [x1, #528]
    ldp     q18, q19, [x1, #560]
    ldp     q20, q21, [x1, #592]
    ldp     q22, q23, [x1, #624]
    ldp     q24, q25, [x1, #656]
    ldp     q26, q27, [x1, #688]
    ldp     q28, q29, [x1, #720]
    ldp     q30, q31, [x1, #752]
    ldr     x2, [x1, #256]
    msr     nzcv, x2
    ldr     x2, [x1, #264]
    msr     fpsr, x2
    ldr     x8, [sp]                // syscall number
    mov     x16, x1
    ldp     x0, x1, [x16, #0]
    ldp     x2, x3, [x16, #16]
    ldp     x4, x5, [x16, #32]
    ldp     x6, x7, [x16, #48]
    ldr     x9, [x16, #72]
    ldp     x10, x11, [x16, #80]
    ldp     x12, x13, [x16, #96]
    ldp     x14, x15, [x16, #112]
    ldr     x17, [x16, #136]
    ldp     x19, x20, [x16, #152]
    ldp     x21, x22, [x16, #168]
    ldp     x23, x24, [x16, #184]
    ldp     x25, x26, [x16, #200]
    ldp     x27, x28, [x16, #216]
    ldr     x29, [x16, #232]

    // Exactly what a veneer does around the call.
    stp     x16, x30, [sp, #-16]!
    adrp    x16, _apkrt_syscall_entry@GOTPAGE
    ldr     x16, [x16, _apkrt_syscall_entry@GOTPAGEOFF]
    blr     x16
    ldp     x16, x30, [sp], #16

    // Store everything into `after`. x0 (the result) goes to the stack first, so x0 can hold the
    // pointer; x18 would not do, XNU clears it on any context switch (F4).
    str     x0, [sp, #-16]!         // stack: result, number, before, after
    ldr     x0, [sp, #40]           // after
    stp     x1, x2, [x0, #8]
    stp     x3, x4, [x0, #24]
    stp     x5, x6, [x0, #40]
    stp     x7, x8, [x0, #56]
    stp     x9, x10, [x0, #72]
    stp     x11, x12, [x0, #88]
    stp     x13, x14, [x0, #104]
    str     x15, [x0, #120]
    str     x17, [x0, #136]
    stp     x19, x20, [x0, #152]
    stp     x21, x22, [x0, #168]
    stp     x23, x24, [x0, #184]
    stp     x25, x26, [x0, #200]
    stp     x27, x28, [x0, #216]
    str     x29, [x0, #232]
    mrs     x2, nzcv
    str     x2, [x0, #256]
    mrs     x2, fpsr
    str     x2, [x0, #264]
    stp     q0, q1, [x0, #272]
    stp     q2, q3, [x0, #304]
    stp     q4, q5, [x0, #336]
    stp     q6, q7, [x0, #368]
    stp     q8, q9, [x0, #400]
    stp     q10, q11, [x0, #432]
    stp     q12, q13, [x0, #464]
    stp     q14, q15, [x0, #496]
    stp     q16, q17, [x0, #528]
    stp     q18, q19, [x0, #560]
    stp     q20, q21, [x0, #592]
    stp     q22, q23, [x0, #624]
    stp     q24, q25, [x0, #656]
    stp     q26, q27, [x0, #688]
    stp     q28, q29, [x0, #720]
    stp     q30, q31, [x0, #752]
    ldr     x1, [sp], #16           // the result
    str     x1, [x0, #0]

    add     sp, sp, #32             // number, before, after
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
