// The loops of island-bench (ADR-0020 X6). Each form runs several times in a row per round, once
// as the converter puts it into an island (x18, TPIDR_EL0) and once as a twin that it leaves alone
// (x17, TPIDRRO_EL0). Every function is
//   uint64_t bench_<form>_<variant>(uint64_t rounds, uint64_t *cell)
// and returns what its form computed, which the C part compares with the twin's. As in x18-test,
// each function saves the caller's x18 and gives it back at its end: bionic points x18 at the
// thread's shadow call stack.
    .arch   armv8-a

.macro function name
    .text
    .globl  \name
    .type   \name, %function
    .p2align 4
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

// The loop: x2 counts the rounds down, and each round runs `copies` copies of the form.
.macro rounds copies, form:vararg
    mov     x2, x0
9:
    .rept   \copies
    \form
    .endr
    subs    x2, x2, #1
    b.ne    9b
.endm

// A chain of 64 dependent ADDs per round, one cycle each: the length of a cycle.
function bench_cycles
    mov     x3, #0
    rounds  64, add x3, x3, #1
    mov     x0, x3
    ret

// A read: 3 added 16 times per round.
.macro read_loop register
    save_x18
    mov     \register, #3
    mov     x3, #0
    rounds  16, add x3, x3, \register
    mov     x0, x3
    restore_x18
    ret
.endm
function bench_read_x18
    read_loop x18
function bench_read_x17
    read_loop x17

// A read among 8 independent ADDs, as islands sit among other work in real code: how much of an
// island the core overlaps with it. 3 added 16 times per round.
.macro read_among register
    add     x3, x3, \register
    add     x5, x5, #1
    add     x6, x6, #1
    add     x7, x7, #1
    add     x8, x8, #1
    add     x9, x9, #1
    add     x10, x10, #1
    add     x11, x11, #1
    add     x12, x12, #1
.endm
.macro read_among_loop register
    save_x18
    mov     \register, #3
    mov     x3, #0
    rounds  16, read_among \register
    mov     x0, x3
    restore_x18
    ret
.endm
function bench_read_among_x18
    read_among_loop x18
function bench_read_among_x17
    read_among_loop x17

// A write: the last round's counter, 1, plus 1.
.macro write_loop register
    save_x18
    rounds  16, add \register, x2, #1
    mov     x0, \register
    restore_x18
    ret
.endm
function bench_write_x18
    write_loop x18
function bench_write_x17
    write_loop x17

// A read and write: 1 added 16 times per round.
.macro read_write_loop register
    save_x18
    mov     \register, #0
    rounds  16, add \register, \register, #1
    mov     x0, \register
    restore_x18
    ret
.endm
function bench_read_write_x18
    read_write_loop x18
function bench_read_write_x17
    read_write_loop x17

// ADRP: the page of bench_page.
.macro adrp_loop register
    save_x18
    rounds  16, adrp \register, bench_page
    mov     x0, \register
    restore_x18
    ret
.endm
function bench_adrp_x18
    adrp_loop x18
function bench_adrp_x17
    adrp_loop x17

// Conditional branches on a register that holds 0, each over an ADD: CBZ is taken and skips it,
// CBNZ is not and runs it. Returns how often the ADD ran.
.macro skip_if_zero register
    cbz     \register, 2f
    add     x3, x3, #1
2:
.endm
.macro skip_unless_zero register
    cbnz    \register, 2f
    add     x3, x3, #1
2:
.endm
.macro branch_loop test, register
    save_x18
    mov     \register, #0
    mov     x3, #0
    rounds  16, \test \register
    mov     x0, x3
    restore_x18
    ret
.endm
function bench_cbz_taken_x18
    branch_loop skip_if_zero, x18
function bench_cbz_taken_x17
    branch_loop skip_if_zero, x17
function bench_cbnz_not_taken_x18
    branch_loop skip_unless_zero, x18
function bench_cbnz_not_taken_x17
    branch_loop skip_unless_zero, x17

// An LL/SC window with the register as the value: adds 1 to the cell 16 times per round and returns
// the cell.
.macro increment register
2:  ldxr    \register, [x1]
    add     \register, \register, #1
    stxr    w4, \register, [x1]
    cbnz    w4, 2b
.endm
.macro exclusive_loop register
    save_x18
    rounds  16, increment \register
    ldr     x0, [x1]
    restore_x18
    ret
.endm
function bench_exclusive_x18
    exclusive_loop x18
function bench_exclusive_x17
    exclusive_loop x17

// A jump table, as compilers build a switch: case (counter & 3) adds its number plus 1 to x3.
.macro jump register
    and     x4, x2, #3
    cmp     x4, #3
    b.hi    .Ljoin\@
    adrp    x5, .Ltable\@
    add     x5, x5, :lo12:.Ltable\@
    ldrsw   \register, [x5, x4, lsl #2]
    add     \register, \register, x5
    br      \register
.Lcase0\@:
    add     x3, x3, #1
    b       .Ljoin\@
.Lcase1\@:
    add     x3, x3, #2
    b       .Ljoin\@
.Lcase2\@:
    add     x3, x3, #3
    b       .Ljoin\@
.Lcase3\@:
    add     x3, x3, #4
.Ljoin\@:
    .pushsection .rodata
    .p2align 2
.Ltable\@:
    .word   .Lcase0\@ - .Ltable\@
    .word   .Lcase1\@ - .Ltable\@
    .word   .Lcase2\@ - .Ltable\@
    .word   .Lcase3\@ - .Ltable\@
    .popsection
.endm
.macro jump_loop register
    save_x18
    mov     x3, #0
    rounds  4, jump \register
    mov     x0, x3
    restore_x18
    ret
.endm
function bench_jump_x18
    jump_loop x18
function bench_jump_x17
    jump_loop x17

// The thread pointer, which the converter puts into an island, and TPIDRRO_EL0, which it leaves
// alone: the last value read.
function bench_thread_pointer
    rounds  16, mrs x3, tpidr_el0
    mov     x0, x3
    ret
function bench_tpidrro
    rounds  16, mrs x3, tpidrro_el0
    mov     x0, x3
    ret

    .section .rodata
    .p2align 3
bench_page:
    .quad   0
