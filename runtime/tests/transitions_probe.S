// Probes for transitions_test.cpp (ADR-0025): guest code that makes syscalls the way converted guest
// code makes them, through the veneer's exact instruction sequence around the syscall entry.
//
// void probe_sigreturn(void);
//   A signal restorer: rt_sigreturn through a veneer, with sp at the signal frame.
//
// long probe_syscall_without_veneer(uint64_t number, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3);
//   Calls the syscall entry like a veneer but returns with ret, so the entry's return address holds
//   no veneer: after the syscall the thread goes on at that address.
//
// uint64_t probe_call(void *function, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3);
//   Calls `function` as guest code calls a function pointer (ld-android.so its apkrt_loader);
//   probe_call_return is the word after the call.
//
// probe_site, probe_after_site, probe_stub_return: a syscall site as the converter lays it out, for
//   the tests that build a frame by hand. The svc became a branch to a stub, which calls the entry
//   and branches back to the word after the site; the entry returns to probe_stub_return. A restart
//   runs the site again.
//
// void probe_syscall_loop(uint64_t rounds, uint64_t pid, uint64_t tid, const uint64_t *set);
//   Unblocks the signals in *set, runs `rounds` rounds of getpid and tgkill(pid, tid, SIGUSR2), then
//   blocks *set again. The tgkill is a signal to the thread itself, delivered at the end of that
//   syscall, so handlers and rt_sigreturn run all the time. Every instruction the thread runs while
//   *set is unblocked lies between probe_loop_begin and probe_loop_end, veneers included.

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
    .globl _probe_sigreturn
_probe_sigreturn:
    mov     x8, #139                        // __NR_rt_sigreturn
    VENEER_SYSCALL
    brk     #1                              // rt_sigreturn does not return

    .p2align 2
    .globl _probe_syscall_without_veneer
_probe_syscall_without_veneer:
    mov     x8, x0
    mov     x0, x1
    mov     x1, x2
    mov     x2, x3
    mov     x3, x4
    stp     x16, x30, [sp, #-16]!
    adrp    x16, _apkrt_syscall_entry@GOTPAGE
    ldr     x16, [x16, _apkrt_syscall_entry@GOTPAGEOFF]
    blr     x16
    ldp     x16, x30, [sp], #16
    ret

    .p2align 2
    .globl _probe_call
_probe_call:
    stp     x29, x30, [sp, #-16]!
    mov     x29, sp
    mov     x16, x0
    mov     x0, x1
    mov     x1, x2
    mov     x2, x3
    mov     x3, x4
    blr     x16
    .globl _probe_call_return
_probe_call_return:
    ldp     x29, x30, [sp], #16
    ret

    .p2align 2
    .globl _probe_site
_probe_site:
    b       .Lprobe_stub
    .globl _probe_after_site
_probe_after_site:
    brk     #1
.Lprobe_stub:
    stp     x16, x30, [sp, #-16]!
    adrp    x16, _apkrt_syscall_entry@GOTPAGE
    ldr     x16, [x16, _apkrt_syscall_entry@GOTPAGEOFF]
    blr     x16
    .globl _probe_stub_return
_probe_stub_return:
    ldp     x16, x30, [sp], #16
    b       _probe_after_site

    .p2align 2
    .globl _probe_syscall_loop
_probe_syscall_loop:
    stp     x29, x30, [sp, #-16]!
    mov     x29, sp
    stp     x19, x20, [sp, #-16]!
    stp     x21, x22, [sp, #-16]!
    mov     x19, x0                         // rounds left
    mov     x20, x1                         // pid
    mov     x21, x2                         // tid
    mov     x22, x3                         // the signals to unblock

    .globl _probe_loop_begin
_probe_loop_begin:
    mov     x0, #1                          // SIG_UNBLOCK
    mov     x1, x22
    mov     x2, #0
    mov     x3, #8
    mov     x8, #135                        // __NR_rt_sigprocmask
    VENEER_SYSCALL
.Lround:
    mov     x8, #172                        // __NR_getpid
    VENEER_SYSCALL
    mov     x0, x20
    mov     x1, x21
    mov     x2, #12                         // SIGUSR2
    mov     x8, #131                        // __NR_tgkill
    VENEER_SYSCALL
    subs    x19, x19, #1
    b.ne    .Lround
    mov     x0, #0                          // SIG_BLOCK
    mov     x1, x22
    mov     x2, #0
    mov     x3, #8
    mov     x8, #135                        // __NR_rt_sigprocmask
    VENEER_SYSCALL
    .globl _probe_loop_end
_probe_loop_end:

    ldp     x21, x22, [sp], #16
    ldp     x19, x20, [sp], #16
    ldp     x29, x30, [sp], #16
    ret
