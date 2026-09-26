// Test 2 of ADR-0030 (thunk-test): the caller, the stand-in for the runtime's entry and the recording host
// function, in assembly so that every register is under the test's control. The offsets are those of
// thunk_test.h.

    .text

// void thunk_call(const struct thunk_in *in, struct thunk_out *out)
// Calls in->stub with in's argument registers and stack area, garbage in x8-x17 and v16-v31, and in's
// values in the callee-saved registers x19-x29 and d8-d15 (its own are saved first); then stores the
// results, the callee-saved registers and how far the stack pointer moved. The stub may break x29 and
// the stack pointer: thunk_state keeps what is needed to come back.
    .globl thunk_call
    .hidden thunk_call
    .type thunk_call, %function
    .p2align 2
thunk_call:
    stp     x29, x30, [sp, #-160]!
    mov     x29, sp
    stp     x19, x20, [sp, #16]
    stp     x21, x22, [sp, #32]
    stp     x23, x24, [sp, #48]
    stp     x25, x26, [sp, #64]
    stp     x27, x28, [sp, #80]
    stp     d8, d9, [sp, #96]
    stp     d10, d11, [sp, #112]
    stp     d12, d13, [sp, #128]
    stp     d14, d15, [sp, #144]
    adrp    x9, thunk_state
    add     x9, x9, :lo12:thunk_state
    mov     x10, sp
    stp     x1, x10, [x9]                   // out, and the stack pointer to come back to
    sub     sp, sp, #128                    // the stack area
    mov     x10, sp
    str     x10, [x9, #16]                  // the stack pointer at the call
    mov     x17, x0
    ldp     x9, x10, [x17, #192]
    stp     x9, x10, [sp]
    ldp     x9, x10, [x17, #208]
    stp     x9, x10, [sp, #16]
    ldp     x9, x10, [x17, #224]
    stp     x9, x10, [sp, #32]
    ldp     x9, x10, [x17, #240]
    stp     x9, x10, [sp, #48]
    ldp     x9, x10, [x17, #256]
    stp     x9, x10, [sp, #64]
    ldp     x9, x10, [x17, #272]
    stp     x9, x10, [sp, #80]
    ldp     x9, x10, [x17, #288]
    stp     x9, x10, [sp, #96]
    ldp     x9, x10, [x17, #304]
    stp     x9, x10, [sp, #112]
    add     x16, x17, #656
    ldp     x19, x20, [x16]
    ldp     x21, x22, [x16, #16]
    ldp     x23, x24, [x16, #32]
    ldp     x25, x26, [x16, #48]
    ldp     x27, x28, [x16, #64]
    ldr     x29, [x16, #80]
    ldp     d8, d9, [x16, #88]
    ldp     d10, d11, [x16, #104]
    ldp     d12, d13, [x16, #120]
    ldp     d14, d15, [x16, #136]
    ldp     q16, q17, [x17, #400]
    ldp     q18, q19, [x17, #432]
    ldp     q20, q21, [x17, #464]
    ldp     q22, q23, [x17, #496]
    ldp     q24, q25, [x17, #528]
    ldp     q26, q27, [x17, #560]
    ldp     q28, q29, [x17, #592]
    ldp     q30, q31, [x17, #624]
    ldp     q0, q1, [x17, #64]
    ldp     q2, q3, [x17, #96]
    ldp     q4, q5, [x17, #128]
    ldp     q6, q7, [x17, #160]
    ldp     x0, x1, [x17]
    ldp     x2, x3, [x17, #16]
    ldp     x4, x5, [x17, #32]
    ldp     x6, x7, [x17, #48]
    ldp     x8, x9, [x17, #320]
    ldp     x10, x11, [x17, #336]
    ldp     x12, x13, [x17, #352]
    ldp     x14, x15, [x17, #368]
    ldr     x30, [x17, #808]
    ldr     x16, [x17, #384]
    ldr     x17, [x17, #392]
    blr     x30
    adrp    x9, thunk_state
    add     x9, x9, :lo12:thunk_state
    ldr     x10, [x9]
    stp     x0, x1, [x10]
    stp     q0, q1, [x10, #16]
    stp     q2, q3, [x10, #48]
    stp     x19, x20, [x10, #80]
    stp     x21, x22, [x10, #96]
    stp     x23, x24, [x10, #112]
    stp     x25, x26, [x10, #128]
    stp     x27, x28, [x10, #144]
    str     x29, [x10, #160]
    stp     d8, d9, [x10, #168]
    stp     d10, d11, [x10, #184]
    stp     d12, d13, [x10, #200]
    stp     d14, d15, [x10, #216]
    mov     x11, sp
    ldr     x12, [x9, #16]
    sub     x11, x11, x12
    str     x11, [x10, #232]
    ldr     x11, [x9, #8]
    mov     sp, x11
    ldp     x19, x20, [sp, #16]
    ldp     x21, x22, [sp, #32]
    ldp     x23, x24, [sp, #48]
    ldp     x25, x26, [sp, #64]
    ldp     x27, x28, [sp, #80]
    ldp     d8, d9, [sp, #96]
    ldp     d10, d11, [sp, #112]
    ldp     d12, d13, [sp, #128]
    ldp     d14, d15, [sp, #144]
    ldp     x29, x30, [sp], #160
    ret
    .size thunk_call, . - thunk_call

// The stand-in for the runtime's entry apkrt_host_call, which the tables hold in slot 0 until the runtime
// binds them: on a host thread the entry is a plain tail call to x16 (ADR-0030, decision 1). It marks
// that the stub went through slot 0, and traps where a stub read slot 0 of the next table as its host
// function, which would call the entry forever.
    .globl thunk_entry
    .hidden thunk_entry
    .type thunk_entry, %function
    .p2align 2
thunk_entry:
    adr     x17, thunk_entry
    cmp     x16, x17
    b.eq    1f
    adrp    x17, thunk_entered
    add     x17, x17, :lo12:thunk_entered
    str     x17, [x17]
    br      x16
1:  brk     #0
    .size thunk_entry, . - thunk_entry

// The host function of the slot under test: records x0-x15, v0-v31 and the 128 bytes from its stack
// pointer on, and returns thunk_results in x0, x1 and v0-v3.
    .globl thunk_record
    .hidden thunk_record
    .type thunk_record, %function
    .p2align 2
thunk_record:
    adrp    x16, thunk_recorded
    add     x16, x16, :lo12:thunk_recorded
    stp     x0, x1, [x16]
    stp     x2, x3, [x16, #16]
    stp     x4, x5, [x16, #32]
    stp     x6, x7, [x16, #48]
    stp     x8, x9, [x16, #64]
    stp     x10, x11, [x16, #80]
    stp     x12, x13, [x16, #96]
    stp     x14, x15, [x16, #112]
    stp     q0, q1, [x16, #128]
    stp     q2, q3, [x16, #160]
    stp     q4, q5, [x16, #192]
    stp     q6, q7, [x16, #224]
    stp     q8, q9, [x16, #256]
    stp     q10, q11, [x16, #288]
    stp     q12, q13, [x16, #320]
    stp     q14, q15, [x16, #352]
    stp     q16, q17, [x16, #384]
    stp     q18, q19, [x16, #416]
    stp     q20, q21, [x16, #448]
    stp     q22, q23, [x16, #480]
    stp     q24, q25, [x16, #512]
    stp     q26, q27, [x16, #544]
    stp     q28, q29, [x16, #576]
    stp     q30, q31, [x16, #608]
    add     x17, x16, #640
    ldp     x9, x10, [sp]
    stp     x9, x10, [x17]
    ldp     x9, x10, [sp, #16]
    stp     x9, x10, [x17, #16]
    ldp     x9, x10, [sp, #32]
    stp     x9, x10, [x17, #32]
    ldp     x9, x10, [sp, #48]
    stp     x9, x10, [x17, #48]
    ldp     x9, x10, [sp, #64]
    stp     x9, x10, [x17, #64]
    ldp     x9, x10, [sp, #80]
    stp     x9, x10, [x17, #80]
    ldp     x9, x10, [sp, #96]
    stp     x9, x10, [x17, #96]
    ldp     x9, x10, [sp, #112]
    stp     x9, x10, [x17, #112]
    ldr     x9, [x16, #768]
    add     x9, x9, #1
    str     x9, [x16, #768]
    adrp    x17, thunk_results
    add     x17, x17, :lo12:thunk_results
    ldp     x0, x1, [x17]
    ldp     q0, q1, [x17, #16]
    ldp     q2, q3, [x17, #48]
    ret
    .size thunk_record, . - thunk_record

// Every other slot: counts the call, so that a stub that loads the wrong slot shows.
    .globl thunk_wrong
    .hidden thunk_wrong
    .type thunk_wrong, %function
    .p2align 2
thunk_wrong:
    adrp    x17, thunk_wrong_calls
    add     x17, x17, :lo12:thunk_wrong_calls
    ldr     x16, [x17]
    add     x16, x16, #1
    str     x16, [x17]
    ret
    .size thunk_wrong, . - thunk_wrong

    .bss
    .p2align 4
    .globl thunk_recorded
    .hidden thunk_recorded
    .type thunk_recorded, %object
thunk_recorded:
    .zero 784
    .size thunk_recorded, 784
    .globl thunk_results
    .hidden thunk_results
    .type thunk_results, %object
    .p2align 4
thunk_results:
    .zero 80
    .size thunk_results, 80
    .globl thunk_entered
    .hidden thunk_entered
    .type thunk_entered, %object
    .p2align 3
thunk_entered:
    .zero 8
    .size thunk_entered, 8
    .globl thunk_wrong_calls
    .hidden thunk_wrong_calls
    .type thunk_wrong_calls, %object
    .p2align 3
thunk_wrong_calls:
    .zero 8
    .size thunk_wrong_calls, 8
    .p2align 3
thunk_state:                                // out, the stack pointer to come back to, the one at the call
    .zero 24
