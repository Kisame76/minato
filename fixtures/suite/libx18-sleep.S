// x18-sleep-test's library: byte loads through x18 after the thread slept, as code that keeps a table's base in x18
// (a toolchain that does not reserve it) reads it. Each function sets x18 to the table, calls usleep (the thread
// leaves the CPU; Android keeps x18 for it, iOS and macOS give it back cleared), then reads one byte in one form, and
// gives the caller's x18 back (bionic's shadow call stack). Linked as a library, as an engine's code is.

    .text

// uint8_t x18_sleep_register(const uint8_t *table, uint64_t index): table[index] through [x18, x1].
    .p2align 2
    .globl  x18_sleep_register
    .type   x18_sleep_register, %function
x18_sleep_register:
    stp     x29, x30, [sp, #-32]!
    mov     x29, sp
    str     x18, [sp, #16]
    str     x1, [sp, #24]
    mov     x18, x0
    mov     x0, #2000
    bl      usleep
    ldr     x1, [sp, #24]
    ldrb    w0, [x18, x1]
    ldr     x18, [sp, #16]
    ldp     x29, x30, [sp], #32
    ret
    .size   x18_sleep_register, .-x18_sleep_register

// uint8_t x18_sleep_immediate(const uint8_t *table): table[0x7b] through [x18, #0x7b].
    .p2align 2
    .globl  x18_sleep_immediate
    .type   x18_sleep_immediate, %function
x18_sleep_immediate:
    stp     x29, x30, [sp, #-32]!
    mov     x29, sp
    str     x18, [sp, #16]
    mov     x18, x0
    mov     x0, #2000
    bl      usleep
    ldrb    w0, [x18, #0x7b]
    ldr     x18, [sp, #16]
    ldp     x29, x30, [sp], #32
    ret
    .size   x18_sleep_immediate, .-x18_sleep_immediate

// uint8_t x18_sleep_added(const uint8_t *table): table[0x1087b] through x8 = x18 + 0x10000, then [x8, #0x87b].
    .p2align 2
    .globl  x18_sleep_added
    .type   x18_sleep_added, %function
x18_sleep_added:
    stp     x29, x30, [sp, #-32]!
    mov     x29, sp
    str     x18, [sp, #16]
    mov     x18, x0
    mov     x0, #2000
    bl      usleep
    add     x8, x18, #0x10, lsl #12
    ldrb    w0, [x8, #0x87b]
    ldr     x18, [sp, #16]
    ldp     x29, x30, [sp], #32
    ret
    .size   x18_sleep_added, .-x18_sleep_added

// uint8_t x18_leaf_read(uint64_t index): table[index] through the x18 its caller set, [x18, x0]; it never sets x18.
    .p2align 2
    .globl  x18_leaf_read
    .type   x18_leaf_read, %function
x18_leaf_read:
    ldrb    w0, [x18, x0]
    ret
    .size   x18_leaf_read, .-x18_leaf_read

// uint8_t x18_sleep_callee(const uint8_t *table, uint64_t index): x18 = table, usleep, then x18_leaf_read(index).
    .p2align 2
    .globl  x18_sleep_callee
    .type   x18_sleep_callee, %function
x18_sleep_callee:
    stp     x29, x30, [sp, #-32]!
    mov     x29, sp
    str     x18, [sp, #16]
    str     x1, [sp, #24]
    mov     x18, x0
    mov     x0, #2000
    bl      usleep
    ldr     x0, [sp, #24]
    bl      x18_leaf_read
    ldr     x18, [sp, #16]
    ldp     x29, x30, [sp], #32
    ret
    .size   x18_sleep_callee, .-x18_sleep_callee
