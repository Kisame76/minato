// The forms of x18 for x18-test (ADR-0020 X5): each one the converter puts into an island, as code
// from a toolchain that does not reserve x18 has them (Unity <= 2020.3). The NDK's clang reserves
// x18 on Android, so they are written here. Each function saves the caller's x18 and gives it back
// at its end: bionic points x18 at the thread's shadow call stack.
    .arch   armv8.1-a  // LDADD, SWP, CAS

.macro function name
    .text
    .globl  \name
    .type   \name, %function
    .p2align 2
\name:
.endm

.macro save_x18
    sub     sp, sp, #16
    str     x18, [sp]
.endm

.macro restore_x18
    ldr     x18, [sp]
    add     sp, sp, #16
.endm

// uint64_t x18_move(uint64_t value): value + 1, through x18.
function x18_move
    save_x18
    mov     x18, x0
    add     x18, x18, #1
    mov     x0, x18
    restore_x18
    ret

// uint64_t x18_add_w(uint64_t value): value + 1 in 32 bits, which clears the top half.
function x18_add_w
    save_x18
    mov     x18, x0
    add     w18, w18, #1
    mov     x0, x18
    restore_x18
    ret

// void x18_load_store(uint64_t cells[5]): cells[0] into cells[1]; its byte 1 into cells[2]; its
// half word 3, sign-extended, into cells[3]; its upper word into cells[4].
function x18_load_store
    save_x18
    ldr     x18, [x0]
    str     x18, [x0, #8]
    ldrb    w18, [x0, #1]
    strb    w18, [x0, #16]
    ldrsh   x18, [x0, #6]
    str     x18, [x0, #24]
    ldr     w18, [x0, #4]
    str     x18, [x0, #32]
    restore_x18
    ret

// uint64_t x18_base(uint64_t cells[4]): x18 as the base: cells[2] into cells[1], then a load with
// post-index. Returns how far the post-index moved x18 plus the value loaded.
function x18_base
    save_x18
    add     x18, x0, #8
    ldr     x1, [x18, #8]
    str     x1, [x18]
    ldr     x2, [x18], #16
    sub     x0, x18, x0
    add     x0, x0, x2
    restore_x18
    ret

// void x18_pair(uint64_t cells[4]): cells[0] and cells[1] into cells[3] and cells[2].
function x18_pair
    save_x18
    ldp     x18, x1, [x0]
    stp     x1, x18, [x0, #16]
    restore_x18
    ret

// uint64_t x18_stack(uint64_t value): value, stored and loaded through a stack slot, plus the
// distance of sp + 24 from sp.
function x18_stack
    save_x18
    sub     sp, sp, #16
    mov     x18, x0
    str     x18, [sp, #8]
    mov     x18, xzr
    ldr     x18, [sp, #8]
    mov     x3, x18
    add     x18, sp, #24
    mov     x2, sp
    sub     x2, x18, x2
    add     x0, x3, x2
    add     sp, sp, #16
    restore_x18
    ret

// void x18_flags(int64_t a, int64_t b, uint64_t out[4]): a < b from CMP; the carry of a + b from
// ADDS; CSEL on its zero flag between 7 and 9; whether CCMP then finds 7.
function x18_flags
    save_x18
    mov     x18, x0
    cmp     x18, x1
    cset    x3, lt
    adds    x18, x18, x1
    cset    x4, cs
    mov     x5, #7
    mov     x6, #9
    csel    x18, x5, x6, eq
    ccmp    x18, #7, #0, eq
    cset    x7, eq
    stp     x3, x4, [x2]
    stp     x18, x7, [x2, #16]
    restore_x18
    ret

// void x18_multiply(uint64_t a, uint64_t b, uint64_t c, uint64_t out[4]): a * b + c; its quotient
// and remainder by b; the high half of it times 2^63.
function x18_multiply
    save_x18
    mov     x18, x2
    madd    x18, x0, x1, x18
    str     x18, [x3]
    udiv    x4, x18, x1
    msub    x5, x4, x1, x18
    mov     x6, #0x8000000000000000
    umulh   x18, x18, x6
    stp     x4, x5, [x3, #8]
    str     x18, [x3, #24]
    restore_x18
    ret

// void x18_bits(uint64_t value, uint64_t out[3]): MOVK of 0xbeef into bits 31:16, BFI of 0xab into
// bits 15:8, and UBFX of bits 31:16 again.
function x18_bits
    save_x18
    mov     x18, x0
    movk    x18, #0xbeef, lsl #16
    str     x18, [x1]
    mov     x2, #0xab
    bfi     x18, x2, #8, #8
    str     x18, [x1, #8]
    ubfx    x3, x18, #16, #16
    str     x3, [x1, #16]
    restore_x18
    ret

// uint64_t x18_simd(uint64_t value): value + 1, after FMOV, INS and UMOV through SIMD registers.
function x18_simd
    save_x18
    mov     x18, x0
    fmov    d0, x18
    fmov    x18, d0
    ins     v1.d[1], x18
    umov    x18, v1.d[1]
    add     x18, x18, #1
    mov     x0, x18
    restore_x18
    ret

// void x18_nzcv(uint64_t out[3]): Z and C set through MSR, then the conditions EQ and CS, and NZCV
// read back through MRS.
function x18_nzcv
    save_x18
    mov     x18, #0x60000000
    msr     nzcv, x18
    cset    x1, eq
    cset    x2, cs
    mrs     x18, nzcv
    stp     x1, x2, [x0]
    str     x18, [x0, #16]
    restore_x18
    ret

// void x18_atomics(uint64_t cells[5]): cells[0] holds 10. LDADD of 5 and of 1, SWP, a CAS that
// matches, LDAR and STLR; the values they return go into cells[2] to cells[4].
function x18_atomics
    save_x18
    mov     x18, #5
    ldadd   x18, x2, [x0]
    mov     x3, #1
    ldadd   x3, x18, [x0]
    swp     x18, x4, [x0]
    mov     x5, #99
    cas     x18, x5, [x0]
    str     x18, [x0, #32]
    ldar    x18, [x0]
    add     x6, x0, #8
    stlr    x18, [x6]
    stp     x2, x4, [x0, #16]
    restore_x18
    ret

// void x18_dump(uint64_t cells[6]): x16 to x21 as a register dump stores them, x18 among them.
function x18_dump
    save_x18
    stp     x19, x20, [sp, #-32]!
    str     x21, [sp, #16]
    mov     x16, #16
    mov     x17, #17
    mov     x18, #18
    mov     x19, #19
    mov     x20, #20
    mov     x21, #21
    stp     x16, x17, [x0]
    stp     x18, x19, [x0, #16]
    stp     x20, x21, [x0, #32]
    ldr     x21, [sp, #16]
    ldp     x19, x20, [sp], #32
    restore_x18
    ret

// uint64_t x18_adrp(void): the address of x18_data, from ADRP and ADD.
function x18_adrp
    save_x18
    adrp    x18, x18_data
    add     x18, x18, :lo12:x18_data
    mov     x0, x18
    restore_x18
    ret

// uint64_t x18_adr(void): the address of x18_data, from ADR.
function x18_adr
    save_x18
    adr     x18, x18_data
    mov     x0, x18
    restore_x18
    ret

// void x18_literals(uint64_t out[3]): LDR X, LDR W and LDRSW from literals.
function x18_literals
    save_x18
    ldr     x18, x18_data
    str     x18, [x0]
    ldr     w18, x18_word
    str     x18, [x0, #8]
    ldrsw   x18, x18_word
    str     x18, [x0, #16]
    restore_x18
    ret

// uint64_t x18_branches(uint64_t value): bit 0 when CBZ falls through, 1 when CBNZ does, 2 when
// TBZ on bit 3 does, 3 when TBNZ on bit 0 does.
function x18_branches
    save_x18
    mov     x18, x0
    mov     x0, #0
    cbz     x18, 1f
    orr     x0, x0, #1
1:  cbnz    x18, 2f
    orr     x0, x0, #2
2:  tbz     x18, #3, 3f
    orr     x0, x0, #4
3:  tbnz    w18, #0, 4f
    orr     x0, x0, #8
4:  restore_x18
    ret

// uint64_t x18_ll_sc_value(uint64_t *cell): adds 1 to the cell with x18 as the value; the new value.
function x18_ll_sc_value
    save_x18
1:  ldxr    x18, [x0]
    add     x18, x18, #1
    stxr    w1, x18, [x0]
    cbnz    w1, 1b
    mov     x0, x18
    restore_x18
    ret

// uint64_t x18_ll_sc_address(uint64_t *cell): adds 2 to the cell with x18 as the address; the new value.
function x18_ll_sc_address
    save_x18
    mov     x18, x0
1:  ldaxr   x1, [x18]
    add     x1, x1, #2
    stlxr   w2, x1, [x18]
    cbnz    w2, 1b
    mov     x0, x1
    restore_x18
    ret

// uint64_t x18_ll_sc_w(uint32_t *cell): adds 1 to the cell in 32 bits; the new value.
function x18_ll_sc_w
    save_x18
1:  ldxr    w18, [x0]
    add     w18, w18, #1
    stxr    w1, w18, [x0]
    cbnz    w1, 1b
    mov     x0, x18
    restore_x18
    ret

// uint64_t x18_cmpxchg(uint64_t *cell, uint64_t expected, uint64_t desired): the cell's old value;
// the CLREX right behind the store-exclusive closes the window on both paths.
function x18_cmpxchg
    save_x18
    mov     w3, #0
1:  ldaxr   x18, [x0]
    cmp     x18, x1
    b.ne    2f
    stlxr   w3, x2, [x0]
2:  clrex
    cbnz    w3, 1b
    mov     x0, x18
    restore_x18
    ret

// uint64_t x18_clrex_only(uint64_t *cell): the cell, from a window that a CLREX closes alone.
function x18_clrex_only
    save_x18
    ldxr    x18, [x0]
    clrex
    mov     x0, x18
    restore_x18
    ret

// uint64_t x18_skipped_write(uint64_t *cell): 77 when the cell holds 0, else the cell: the CBZ
// skips the only write of x18 in the window.
function x18_skipped_write
    save_x18
    mov     x18, #77
1:  ldxr    x1, [x0]
    cbz     x1, 2f
    mov     x18, x1
2:  stxr    w2, x1, [x0]
    cbnz    w2, 1b
    mov     x0, x18
    restore_x18
    ret

// int64_t x18_switch(uint64_t index): 10 + index for 0 to 3, else -1, through a jump table and
// br x18, as compilers build a switch.
function x18_switch
    save_x18
    cmp     x0, #3
    b.hi    9f
    adrp    x9, .Lx18_table
    add     x9, x9, :lo12:.Lx18_table
    ldrsw   x18, [x9, x0, lsl #2]
    add     x18, x18, x9
    br      x18
.Lx18_case0:
    mov     x0, #10
    b       8f
.Lx18_case1:
    mov     x0, #11
    b       8f
.Lx18_case2:
    mov     x0, #12
    b       8f
.Lx18_case3:
    mov     x0, #13
    b       8f
9:  mov     x0, #-1
8:  restore_x18
    ret

// uint64_t x18_syscall(uint64_t value): x18 after getpid.
function x18_syscall
    save_x18
    mov     x18, x0
    mov     x8, #172  // __NR_getpid
    svc     #0
    mov     x0, x18
    restore_x18
    ret

// uint64_t x18_yield(uint64_t value, uint64_t times): 0 when x18 kept its value across every
// sched_yield, else 1.
function x18_yield
    save_x18
    mov     x18, x0
    mov     x3, x0
1:  mov     x8, #124  // __NR_sched_yield
    svc     #0
    cmp     x18, x3
    b.ne    2f
    subs    x1, x1, #1
    b.ne    1b
    mov     x0, #0
    b       3f
2:  mov     x0, #1
3:  restore_x18
    ret

// uint64_t x18_raise(uint64_t value, uint64_t pid, uint64_t tid): sends this thread SIGUSR1 with
// x18 = value; x18 after the handler.
function x18_raise
    save_x18
    mov     x18, x0
    mov     x0, x1
    mov     x1, x2
    mov     x2, #10   // SIGUSR1
    mov     x8, #131  // __NR_tgkill
    svc     #0
    mov     x0, x18
    restore_x18
    ret

// uint64_t x18_read(void): x18 as the caller has it.
function x18_read
    mov     x0, x18
    ret

    .section .rodata
    .p2align 3
    .globl  x18_data
x18_data:
    .quad   0x0102030405060708
x18_word:
    .word   0x89abcdef
    .p2align 2
.Lx18_table:
    .word   .Lx18_case0 - .Lx18_table
    .word   .Lx18_case1 - .Lx18_table
    .word   .Lx18_case2 - .Lx18_table
    .word   .Lx18_case3 - .Lx18_table
