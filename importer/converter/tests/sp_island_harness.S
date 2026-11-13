// Assembly blobs for sp_island_exec_test.cpp, position independent: copied into a JIT buffer around one site.
// Each has its context pointer in a literal at offset 8. Context layout (bytes): in[29] 0, init_sp 232, saved_sp 240,
// out[29] 248, out_sp 480, in_flags 488, out_flags 496. The pre blob saves the callee-saved registers, switches to
// init_sp, sets NZCV and loads x0 to x28 except x18; the post blob dumps x0 to x28, sp and NZCV above the new sp,
// copies them to the context and returns to the caller of the pre blob.
    .text
    .p2align 3
    .globl _sp_pre_begin
    .globl _sp_pre_end
    .globl _sp_post_begin
    .globl _sp_post_end
_sp_pre_begin:
    b       Lpre_start
    nop
Lpre_lit:
    .quad   0
Lpre_start:
    stp     x29,x30, [sp, #-16]!
    stp     x27,x28, [sp, #-16]!
    stp     x25,x26, [sp, #-16]!
    stp     x23,x24, [sp, #-16]!
    stp     x21,x22, [sp, #-16]!
    stp     x19,x20, [sp, #-16]!
    ldr     x0, Lpre_lit
    mov     x1, sp
    str     x1, [x0, #240]
    ldr     x1, [x0, #232]
    mov     sp, x1
    mov     x28, x0
    ldr     x1, [x28, #488]
    msr     nzcv, x1
    ldp     x0, x1, [x28, #0]
    ldp     x2, x3, [x28, #16]
    ldp     x4, x5, [x28, #32]
    ldp     x6, x7, [x28, #48]
    ldp     x8, x9, [x28, #64]
    ldp     x10, x11, [x28, #80]
    ldp     x12, x13, [x28, #96]
    ldp     x14, x15, [x28, #112]
    ldr     x16, [x28, #128]
    ldr     x17, [x28, #136]
    ldr     x19, [x28, #152]
    ldp     x20, x21, [x28, #160]
    ldp     x22, x23, [x28, #176]
    ldp     x24, x25, [x28, #192]
    ldp     x26, x27, [x28, #208]
    ldr     x28, [x28, #224]
_sp_pre_end:
    .p2align 3
_sp_post_begin:
    b       Lpost_start
    nop
Lpost_lit:
    .quad   0
Lpost_start:
    stp     x0, x1, [sp, #16]
    mrs     x0, nzcv
    str     x0, [sp, #8]
    stp     x2, x3, [sp, #32]
    stp     x4, x5, [sp, #48]
    stp     x6, x7, [sp, #64]
    stp     x8, x9, [sp, #80]
    stp     x10, x11, [sp, #96]
    stp     x12, x13, [sp, #112]
    stp     x14, x15, [sp, #128]
    stp     x16, x17, [sp, #144]
    stp     x18, x19, [sp, #160]
    stp     x20, x21, [sp, #176]
    stp     x22, x23, [sp, #192]
    stp     x24, x25, [sp, #208]
    stp     x26, x27, [sp, #224]
    mov     x0, sp
    stp     x28, x0, [sp, #240]
    mov     x1, sp
    ldr     x0, Lpost_lit
    ldp     x2, x3, [x1, #16]
    stp     x2, x3, [x0, #248]
    ldp     x2, x3, [x1, #32]
    stp     x2, x3, [x0, #264]
    ldp     x2, x3, [x1, #48]
    stp     x2, x3, [x0, #280]
    ldp     x2, x3, [x1, #64]
    stp     x2, x3, [x0, #296]
    ldp     x2, x3, [x1, #80]
    stp     x2, x3, [x0, #312]
    ldp     x2, x3, [x1, #96]
    stp     x2, x3, [x0, #328]
    ldp     x2, x3, [x1, #112]
    stp     x2, x3, [x0, #344]
    ldp     x2, x3, [x1, #128]
    stp     x2, x3, [x0, #360]
    ldp     x2, x3, [x1, #144]
    stp     x2, x3, [x0, #376]
    ldp     x2, x3, [x1, #160]
    stp     x2, x3, [x0, #392]
    ldp     x2, x3, [x1, #176]
    stp     x2, x3, [x0, #408]
    ldp     x2, x3, [x1, #192]
    stp     x2, x3, [x0, #424]
    ldp     x2, x3, [x1, #208]
    stp     x2, x3, [x0, #440]
    ldp     x2, x3, [x1, #224]
    stp     x2, x3, [x0, #456]
    ldp     x2, x3, [x1, #240]
    stp     x2, x3, [x0, #472]
    ldr     x2, [x1, #8]
    str     x2, [x0, #496]
    ldr     x1, [x0, #240]
    mov     sp, x1
    ldp     x19,x20, [sp], #16
    ldp     x21,x22, [sp], #16
    ldp     x23,x24, [sp], #16
    ldp     x25,x26, [sp], #16
    ldp     x27,x28, [sp], #16
    ldp     x29,x30, [sp], #16
    ret
_sp_post_end:
