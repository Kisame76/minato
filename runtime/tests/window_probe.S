// Probe for window_test.cpp (ADR-0028): a syscall at a site as the converter lays it out, so that a
// restart after an SA_RESTART handler runs the syscall again (transitions_probe.S has the same layout for
// frames built by hand).
//
// long probe_site_syscall(uint64_t number, uint64_t a0, uint64_t a1);
//   The svc became a branch to a stub, which calls the syscall entry through the veneer's instructions
//   (rewrite.cpp) and branches back to the word after the site. The syscall's exit goes on after the
//   site; a restart goes back to the site itself.

    .text
    .p2align 2
    .globl _probe_site_syscall
_probe_site_syscall:
    stp     x29, x30, [sp, #-16]!
    mov     x29, sp
    mov     x8, x0
    mov     x0, x1
    mov     x1, x2
.Lsite:
    b       .Lstub                          // was: svc #0
.Lafter_site:
    ldp     x29, x30, [sp], #16
    ret
.Lstub:
    stp     x16, x30, [sp, #-16]!
    adrp    x16, _apkrt_syscall_entry@GOTPAGE
    ldr     x16, [x16, _apkrt_syscall_entry@GOTPAGEOFF]
    blr     x16
    ldp     x16, x30, [sp], #16
    b       .Lafter_site
