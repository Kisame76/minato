// Probes for blocking_test.cpp (ADR-0026): guest code that makes syscalls the way converted guest
// code makes them, through the veneer's exact instruction sequence around the syscall entry.
//
// void blocking_sigreturn(void);
//   A signal restorer: rt_sigreturn through a veneer, with sp at the signal frame.
//
// long probe_syscall6(uint64_t number, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
//                     uint64_t a5);
//   One syscall with six arguments through a veneer, from the calling thread's stack (probe_syscall of
//   threads_probe.S passes four).

// The veneer's instructions around the call of the syscall entry (rewrite.cpp).
.macro VENEER_SYSCALL
    stp     x16, x30, [sp, #-16]!
    adrp    x16, _apkrt_syscall_entry@GOTPAGE
    ldr     x16, [x16, _apkrt_syscall_entry@GOTPAGEOFF]
    blr     x16
    ldp     x16, x30, [sp], #16
    b       1f
1:
.endm

    .text
    .p2align 2
    .globl _blocking_sigreturn
_blocking_sigreturn:
    mov     x8, #139                        // __NR_rt_sigreturn
    VENEER_SYSCALL
    brk     #1                              // rt_sigreturn does not return

    .p2align 2
    .globl _probe_syscall6
_probe_syscall6:
    stp     x29, x30, [sp, #-16]!
    mov     x29, sp
    mov     x8, x0
    mov     x0, x1
    mov     x1, x2
    mov     x2, x3
    mov     x3, x4
    mov     x4, x5
    mov     x5, x6
    VENEER_SYSCALL
    ldp     x29, x30, [sp], #16
    ret
