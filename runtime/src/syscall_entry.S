// The two places where control passes between host and guest code (ADR-0019).
//
// apkrt_syscall_entry: called by every svc veneer in converted guest code. The veneer has saved
// x16 and x30; this entry saves all other registers, NZCV and FPSR in a struct apkrt_syscall_frame
// (syscall_frame.h) on the guest's stack, lets the C++ dispatcher work on the frame, and restores
// everything from it. Like the Linux kernel, only x0 changes: guest code may keep values in any
// register across an inline svc. x18 is saved and restored only for completeness; XNU clears it on
// every context switch anyway (F4), and converted code never relies on it.
// The entry and the exit also switch the thread's in_runtime (signals.h, ADR-0025): the entry sets
// it before any C++ runs, the exit clears it and only then looks at the pending signals once more,
// so that none gets lost between the dispatcher's end and guest code. The runtime signal's handler
// knows the words before the set and after the clear by the labels below.
//
// apkrt_enter_guest: starts a guest program the way Linux starts an executable: stack pointer on
// the start stack (argc, argv, envp, auxv), every register zero.
//
// apkrt_save_host / apkrt_return_to_host: _setjmp and _longjmp for the host side of a guest thread
// (threads.cpp), so that SYS_exit gets back to the host's stack. Our own, because libc's longjmp
// is intercepted by AddressSanitizer in the test builds, which then warns about the stack switch.

#define FRAME_SIZE 784  // sizeof(struct apkrt_syscall_frame)
#define SIGNALS_MASK 0  // offsetof(ThreadSignals, mask), pending follows it
#define IN_RUNTIME 16   // offsetof(ThreadSignals, in_runtime); ThreadSignals starts a GuestThread

    .text
    .p2align 2
    .globl _apkrt_syscall_entry
_apkrt_syscall_entry:
    hint    #34                     // bti c: called with blr
    sub     sp, sp, #FRAME_SIZE
    // The frame's lowest word first: on a guest stack that is used up, this store faults while the
    // thread still counts as in guest code, a guest fault as at its next push. No later store of
    // the entry can fault: the rest of the frame lies between this word and the veneer's.
    stp     x0, x1, [sp, #0]
    stp     x9, x10, [sp, #72]
    // In the runtime from here on: in_runtime of the calling GuestThread, which the TSD holds under
    // apkrt_guest_thread_key (the load pthread_getspecific does); ~0 while no guest thread exists.
    // Nothing here may touch the flags: NZCV is saved further down.
    adrp    x9, _apkrt_guest_thread_key@PAGE
    ldr     x9, [x9, _apkrt_guest_thread_key@PAGEOFF]
    add     x10, x9, #1
    cbz     x10, 1f
    mrs     x10, TPIDRRO_EL0
    and     x10, x10, #~7
    ldr     x9, [x10, x9, lsl #3]
    cbz     x9, 1f                  // a host thread (tests)
    mov     w10, #1
    strb    w10, [x9, #IN_RUNTIME]
1:
    .globl _apkrt_syscall_entered
_apkrt_syscall_entered:
    stp     x2, x3, [sp, #16]
    stp     x4, x5, [sp, #32]
    stp     x6, x7, [sp, #48]
    str     x8, [sp, #64]
    str     x11, [sp, #88]
    stp     x12, x13, [sp, #96]
    stp     x14, x15, [sp, #112]
    stp     x16, x17, [sp, #128]
    stp     x18, x19, [sp, #144]
    stp     x20, x21, [sp, #160]
    stp     x22, x23, [sp, #176]
    stp     x24, x25, [sp, #192]
    stp     x26, x27, [sp, #208]
    stp     x28, x29, [sp, #224]
    str     x30, [sp, #240]
    mrs     x9, nzcv
    mrs     x10, fpsr
    stp     x9, x10, [sp, #256]
    // All 32 SIMD registers in full: the C ABI keeps only the low halves of v8-v15.
    stp     q0, q1, [sp, #272]
    stp     q2, q3, [sp, #304]
    stp     q4, q5, [sp, #336]
    stp     q6, q7, [sp, #368]
    stp     q8, q9, [sp, #400]
    stp     q10, q11, [sp, #432]
    stp     q12, q13, [sp, #464]
    stp     q14, q15, [sp, #496]
    stp     q16, q17, [sp, #528]
    stp     q18, q19, [sp, #560]
    stp     q20, q21, [sp, #592]
    stp     q22, q23, [sp, #624]
    stp     q24, q25, [sp, #656]
    stp     q26, q27, [sp, #688]
    stp     q28, q29, [sp, #720]
    stp     q30, q31, [sp, #752]

    // A frame record over the saved x29/x30, so debuggers and crash logs walk on into the guest.
    add     x29, sp, #232
    mov     x0, sp
    bl      _apkrt_syscall_dispatch     // x0: the thread's ThreadSignals if the exit checks, else 0

    ldp     x9, x10, [sp, #256]
    msr     nzcv, x9
    msr     fpsr, x10
    ldp     q0, q1, [sp, #272]
    ldp     q2, q3, [sp, #304]
    ldp     q4, q5, [sp, #336]
    ldp     q6, q7, [sp, #368]
    ldp     q8, q9, [sp, #400]
    ldp     q10, q11, [sp, #432]
    ldp     q12, q13, [sp, #464]
    ldp     q14, q15, [sp, #496]
    ldp     q16, q17, [sp, #528]
    ldp     q18, q19, [sp, #560]
    ldp     q20, q21, [sp, #592]
    ldp     q22, q23, [sp, #624]
    ldp     q24, q25, [sp, #656]
    ldp     q26, q27, [sp, #688]
    ldp     q28, q29, [sp, #720]
    ldp     q30, q31, [sp, #752]
    ldp     x2, x3, [sp, #16]
    ldp     x4, x5, [sp, #32]
    ldp     x6, x7, [sp, #48]
    ldp     x8, x9, [sp, #64]
    ldp     x10, x11, [sp, #80]
    ldp     x12, x13, [sp, #96]
    ldp     x14, x15, [sp, #112]
    ldp     x16, x17, [sp, #128]
    ldp     x18, x19, [sp, #144]
    ldp     x20, x21, [sp, #160]
    ldp     x22, x23, [sp, #176]
    ldp     x24, x25, [sp, #192]
    ldp     x26, x27, [sp, #208]
    ldp     x28, x29, [sp, #224]
    ldr     x30, [sp, #240]
    // x0 and x1 come last. x16 belongs to the veneer, which restores it, so the check may use it.
    cbz     x0, 2f
    strb    wzr, [x0, #IN_RUNTIME]
    // From here to apkrt_syscall_returned the thread counts as returned from the syscall: a runtime
    // signal delivers on the state after it, from the frame (at the ret: from the registers, the
    // first argument in x16 for a restart).
    .globl _apkrt_syscall_returning
_apkrt_syscall_returning:
    ldp     x1, x16, [x0, #SIGNALS_MASK]    // mask, pending
    bic     x16, x16, x1                    // without touching the guest's flags, restored above
    cbnz    x16, 3f
2:  ldp     x0, x1, [sp, #0]
    ldr     x16, [sp, #248]                 // x[31]: the first argument
    add     sp, sp, #FRAME_SIZE
    .globl _apkrt_syscall_return
_apkrt_syscall_return:
    ret
3:  mov     w1, #1                          // a signal to deliver: back in the runtime first
    strb    w1, [x0, #IN_RUNTIME]
    .globl _apkrt_syscall_returned
_apkrt_syscall_returned:
    mov     x0, sp
    bl      _apkrt_syscall_deliver          // delivers and resumes the guest, never returns
    brk     #1

// void apkrt_enter_guest(uint64_t entry, uint64_t stack_pointer)  -- never returns
    .p2align 2
    .globl _apkrt_enter_guest
_apkrt_enter_guest:
    mov     sp, x1
    mov     x16, x0
    msr     nzcv, xzr
    msr     fpsr, xzr
    msr     fpcr, xzr
    movi    v0.2d, #0
    movi    v1.2d, #0
    movi    v2.2d, #0
    movi    v3.2d, #0
    movi    v4.2d, #0
    movi    v5.2d, #0
    movi    v6.2d, #0
    movi    v7.2d, #0
    movi    v8.2d, #0
    movi    v9.2d, #0
    movi    v10.2d, #0
    movi    v11.2d, #0
    movi    v12.2d, #0
    movi    v13.2d, #0
    movi    v14.2d, #0
    movi    v15.2d, #0
    movi    v16.2d, #0
    movi    v17.2d, #0
    movi    v18.2d, #0
    movi    v19.2d, #0
    movi    v20.2d, #0
    movi    v21.2d, #0
    movi    v22.2d, #0
    movi    v23.2d, #0
    movi    v24.2d, #0
    movi    v25.2d, #0
    movi    v26.2d, #0
    movi    v27.2d, #0
    movi    v28.2d, #0
    movi    v29.2d, #0
    movi    v30.2d, #0
    movi    v31.2d, #0
    mov     x0, xzr
    mov     x1, xzr
    mov     x2, xzr
    mov     x3, xzr
    mov     x4, xzr
    mov     x5, xzr
    mov     x6, xzr
    mov     x7, xzr
    mov     x8, xzr
    mov     x9, xzr
    mov     x10, xzr
    mov     x11, xzr
    mov     x12, xzr
    mov     x13, xzr
    mov     x14, xzr
    mov     x15, xzr
    mov     x17, xzr
    mov     x19, xzr
    mov     x20, xzr
    mov     x21, xzr
    mov     x22, xzr
    mov     x23, xzr
    mov     x24, xzr
    mov     x25, xzr
    mov     x26, xzr
    mov     x27, xzr
    mov     x28, xzr
    mov     x29, xzr
    mov     x30, xzr
    br      x16

// int apkrt_save_host(uint64_t context[22]): callee-saved registers, sp and lr; returns 0, and 1
// when apkrt_return_to_host comes back to it.
    .p2align 2
    .globl _apkrt_save_host
_apkrt_save_host:
    stp     x19, x20, [x0, #0]
    stp     x21, x22, [x0, #16]
    stp     x23, x24, [x0, #32]
    stp     x25, x26, [x0, #48]
    stp     x27, x28, [x0, #64]
    stp     x29, x30, [x0, #80]
    mov     x1, sp
    str     x1, [x0, #96]
    stp     d8, d9, [x0, #112]
    stp     d10, d11, [x0, #128]
    stp     d12, d13, [x0, #144]
    stp     d14, d15, [x0, #160]
    mov     x0, #0
    ret

// void apkrt_return_to_host(const uint64_t context[22])  -- never returns
    .p2align 2
    .globl _apkrt_return_to_host
_apkrt_return_to_host:
    ldp     x19, x20, [x0, #0]
    ldp     x21, x22, [x0, #16]
    ldp     x23, x24, [x0, #32]
    ldp     x25, x26, [x0, #48]
    ldp     x27, x28, [x0, #64]
    ldp     x29, x30, [x0, #80]
    ldr     x1, [x0, #96]
    mov     sp, x1
    ldp     d8, d9, [x0, #112]
    ldp     d10, d11, [x0, #128]
    ldp     d12, d13, [x0, #144]
    ldp     d14, d15, [x0, #160]
    mov     x0, #1
    ret
