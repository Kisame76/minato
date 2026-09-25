// The loop of x18-stress (ADR-0020 X5): rounds of x18 islands, run by eight threads at once, each
// with a tag of its own in bits 63:48 of x18. The loop makes no syscall and reads no thread pointer.
// The labels mark where x18 carries the tag (stress_loop_begin to stress_loop_end, but for the jump
// sequence, where it holds the target) and which instructions use x18 (stress_x18_sites).

// uint64_t stress_rounds(struct stress *state, uint64_t rounds): 0, or 1 when a branch that x18
// rules out was taken. state points at the thread's x18, a spill slot, the cell of its own window,
// the address of the shared counter, and the four counts of the jump table's cases.
    .text
    .globl  stress_rounds
    .type   stress_rounds, %function
    .p2align 2
stress_rounds:
    sub     sp, sp, #32
    str     x18, [sp]
    str     x19, [sp, #16]
    ldr     x18, [x0]
    add     x5, x0, #16
    ldr     x6, [x0, #24]
    .globl  stress_loop_begin
stress_loop_begin:
.Lx18_1:
    add     x18, x18, #1
.Lx18_2:
    str     x18, [x0, #8]
.Lx18_3:
    ldr     x18, [x0, #8]
.Lx18_4:
    cbz     x18, .Lfailed
.Lx18_5:
    tbnz    x18, #63, .Lfailed
.Lshared:  // the counter all threads share gets the tag
    ldxr    x3, [x6]
.Lx18_6:
    add     x3, x3, x18, lsr #48
    stxr    w4, x3, [x6]
    cbnz    w4, .Lshared
.Lx18_7:
    str     x18, [x5]
.Lx18_8:  // the thread's own window, with x18 as the value
    ldxr    x18, [x5]
.Lx18_9:
    add     x18, x18, #1
.Lx18_10:
    stxr    w4, x18, [x5]
    cbnz    w4, .Lx18_8
.Lx18_11:  // a jump table on bits 2:1 of x18
    ubfx    x9, x18, #1, #2
    cmp     x9, #3
    b.hi    .Lfailed
    adrp    x10, .Lstress_table
    add     x10, x10, :lo12:.Lstress_table
.Lx18_12:
    mov     x19, x18
    .globl  stress_jump_begin
stress_jump_begin:
.Lx18_13:
    ldrsw   x18, [x10, x9, lsl #2]
.Lx18_14:
    add     x18, x18, x10
.Lx18_15:
    br      x18
.Lcase0:
.Lx18_16:
    mov     x18, x19
    ldr     x7, [x0, #32]
    add     x7, x7, #1
    str     x7, [x0, #32]
    b       .Ljoin
.Lcase1:
.Lx18_17:
    mov     x18, x19
    ldr     x7, [x0, #40]
    add     x7, x7, #1
    str     x7, [x0, #40]
    b       .Ljoin
.Lcase2:
.Lx18_18:
    mov     x18, x19
    ldr     x7, [x0, #48]
    add     x7, x7, #1
    str     x7, [x0, #48]
    b       .Ljoin
.Lcase3:
.Lx18_19:
    mov     x18, x19
    ldr     x7, [x0, #56]
    add     x7, x7, #1
    str     x7, [x0, #56]
    b       .Ljoin
.Ljoin:
    .globl  stress_jump_end
stress_jump_end:
    subs    x1, x1, #1
    b.ne    stress_loop_begin
    .globl  stress_loop_end
stress_loop_end:
    str     x18, [x0]
    mov     x0, #0
    b       .Ldone
.Lfailed:
    mov     x0, #1
.Ldone:
    ldr     x19, [sp, #16]
    ldr     x18, [sp]
    add     sp, sp, #32
    ret

    .section .rodata
    .p2align 2
.Lstress_table:
    .word   .Lcase0 - .Lstress_table
    .word   .Lcase1 - .Lstress_table
    .word   .Lcase2 - .Lstress_table
    .word   .Lcase3 - .Lstress_table

// The instructions of the loop that use x18, as offsets from stress_loop_begin.
    .globl  stress_x18_sites
stress_x18_sites:
    .word   .Lx18_1 - stress_loop_begin, .Lx18_2 - stress_loop_begin, .Lx18_3 - stress_loop_begin
    .word   .Lx18_4 - stress_loop_begin, .Lx18_5 - stress_loop_begin, .Lx18_6 - stress_loop_begin
    .word   .Lx18_7 - stress_loop_begin, .Lx18_8 - stress_loop_begin, .Lx18_9 - stress_loop_begin
    .word   .Lx18_10 - stress_loop_begin, .Lx18_11 - stress_loop_begin, .Lx18_12 - stress_loop_begin
    .word   .Lx18_13 - stress_loop_begin, .Lx18_14 - stress_loop_begin, .Lx18_15 - stress_loop_begin
    .word   .Lx18_16 - stress_loop_begin, .Lx18_17 - stress_loop_begin, .Lx18_18 - stress_loop_begin
    .word   .Lx18_19 - stress_loop_begin
    .globl  stress_x18_site_count
stress_x18_site_count:
    .word   19
