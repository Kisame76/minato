// Probe for stepper.cpp: the call that the stepper single-steps.
//
// uint64_t stepper_call(uint64_t (*function)(uint64_t), uint64_t argument);
//   Stops at stepper_call_brk first, where the stepper turns single stepping on and moves pc past
//   the BRK, then calls function(argument). stepper_call_return is the word after the call, where
//   the stepper turns single stepping off again.

    .text
    .p2align 2
    .globl _stepper_call
_stepper_call:
    stp     x29, x30, [sp, #-16]!
    mov     x29, sp
    mov     x16, x0
    mov     x0, x1
    .globl _stepper_call_brk
_stepper_call_brk:
    brk     #0x57
    blr     x16
    .globl _stepper_call_return
_stepper_call_return:
    ldp     x29, x30, [sp], #16
    ret
