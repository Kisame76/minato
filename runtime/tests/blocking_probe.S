// Probes for blocking_test.cpp (ADR-0026): guest code that makes syscalls the way converted guest
// code makes them, through the veneer's exact instruction sequence around the syscall entry.
//
// void blocking_sigreturn(void);
//   A signal restorer: rt_sigreturn through a veneer, with sp at the signal frame.

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
