// Minato, added to bionic's libc.so (ADR-0021): the restorer of signal handlers. Linux returns a
// handler without SA_RESTORER to the rt_sigreturn trampoline of its vDSO, and bionic's sigaction
// leaves SA_RESTORER out on arm64 on purpose, so that unwinders find that trampoline. There is no
// vDSO here; the runtime returns such handlers here instead (signals.cpp).
//
// For unwinders it looks like the vDSO's: a signal frame whose caller's fp and lr lie in the frame
// record x29 points at (the runtime writes it above the signal frame), and a nop before the entry,
// because some unwinders look up pc - 1.

    .text
    .p2align 2
    .cfi_startproc
    .cfi_signal_frame
    .cfi_def_cfa x29, 0
    .cfi_offset x29, 0
    .cfi_offset x30, 8
    nop
    .globl __apkrt_sigreturn
    .type __apkrt_sigreturn, %function
__apkrt_sigreturn:
    mov     x8, #139        // __NR_rt_sigreturn
    svc     #0
    .cfi_endproc
    .size __apkrt_sigreturn, . - __apkrt_sigreturn
