// Probes for threads_test.cpp: Linux syscalls made the way converted guest code makes them, through
// the veneer's exact instruction sequence around the syscall entry.
//
// long probe_clone(uint64_t flags, void *child_stack, int *parent_tid, void *tls, int *child_tid,
//                  void (*function)(void *), void *argument);
//   Like bionic's __bionic_clone: pushes function and argument onto the child's stack, then
//   clone. Before the syscall x19-x28 and v8-v15 get known values (probe_marks); the child stores
//   what it finds in them into probe_child_registers, calls function(argument) and ends with exit(0).
//   The parent gets the result of clone.
//
// long probe_syscall(uint64_t number, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3);
//   One syscall through a veneer, from the calling thread's stack.

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
    .globl _probe_clone
_probe_clone:
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

    stp     x5, x6, [x1, #-16]!             // function and argument onto the child's stack
    adrp    x9, _probe_marks@PAGE
    add     x9, x9, _probe_marks@PAGEOFF
    ldp     x19, x20, [x9, #0]
    ldp     x21, x22, [x9, #16]
    ldp     x23, x24, [x9, #32]
    ldp     x25, x26, [x9, #48]
    ldp     x27, x28, [x9, #64]
    ldp     q8, q9, [x9, #80]
    ldp     q10, q11, [x9, #112]
    ldp     q12, q13, [x9, #144]
    ldp     q14, q15, [x9, #176]
    mov     x8, #220                        // __NR_clone
    VENEER_SYSCALL
    cbz     x0, .Lchild

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

.Lchild:
    adrp    x9, _probe_child_registers@PAGE
    add     x9, x9, _probe_child_registers@PAGEOFF
    stp     x19, x20, [x9, #0]
    stp     x21, x22, [x9, #16]
    stp     x23, x24, [x9, #32]
    stp     x25, x26, [x9, #48]
    stp     x27, x28, [x9, #64]
    stp     q8, q9, [x9, #80]
    stp     q10, q11, [x9, #112]
    stp     q12, q13, [x9, #144]
    stp     q14, q15, [x9, #176]
    mov     x10, sp
    str     x10, [x9, #208]                 // the child's stack pointer after the pushes
    mov     x29, #0
    mov     x30, #0
    ldp     x10, x0, [sp], #16              // function, argument
    blr     x10
    mov     x0, #0
    mov     x8, #93                         // __NR_exit
    VENEER_SYSCALL
    brk     #1                              // exit does not return

    .p2align 2
    .globl _probe_syscall
_probe_syscall:
    stp     x29, x30, [sp, #-16]!
    mov     x29, sp
    mov     x8, x0
    mov     x0, x1
    mov     x1, x2
    mov     x2, x3
    mov     x3, x4
    VENEER_SYSCALL
    ldp     x29, x30, [sp], #16
    ret
