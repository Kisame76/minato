// Data inside .text, as an NDK library has it (ADR-0020 X5 follow-up: a Unity test APK's library is refused at import
// because words of such data decode as x18 uses and svc #imm). Built five times from this one file, the entry points
// carrying SUFFIX: with a .symtab (dit-sym); stripped, with its .eh_frame, and with the dynamic relocations in
// four forms (dit-strip as the linker's default, dit-rela plain RELA with the addends, dit-aps2 Android's packed
// relocations, which the converter refuses and only its scan reads, dit-relr RELR).
// In all of them the literal pools and the jump tables are what the converter must treat as the data they are, and
// every word after or between them, real code that looks like what the data looks like, as code. A stub that only an
// address, a pointer in data, a call or a table names (dit_via_adr, dit_via_bl, dit_via_ptr, dit_add) is only reached,
// never proven: its own pool stays code, so it holds a word the scan can scan; the functions that a trusted root
// names (a sized exported function, DT_INIT, the init and fini arrays) hold a caspal in theirs. Each function that
// ends in a svc #0 must still have it rewritten, so each returns the pid and the test compares it with getpid().
#define CAT2(a, b) a##b
#define CAT(a, b) CAT2(a, b)
#define F(name) CAT(name, SUFFIX)

    .arch   armv8.1-a  // CASPAL

.macro function name
    .text
    .globl  \name
    .type   \name, %function
    .p2align 2
\name:
    .cfi_startproc
.endm

.macro end_function name
    .cfi_endproc
    .size   \name, . - \name
.endm

// uint32_t dit_pool(unsigned i): word i of a literal pool in .text, read PC-relatively by an index that a compare
// bounds, as a compiler reads a table. The words are an svc #0x1234, a caspal and an st2 with x18 as the base, and an
// ldr into x18 as data, never run.
function F(dit_pool)
    cmp     w0, #3
    b.hi    9f
    adr     x1, 1f
    ldr     w0, [x1, w0, uxtw #2]
    ret
9:  mov     w0, #0
    ret
    .p2align 2
1:  .word   0xd4024681  // svc #0x1234
    .word   0x4862fe44  // caspal x2, x3, x4, x5, [x18]
    .word   0x4c008240  // st2 { v0.16b, v1.16b }, [x18]
    .word   0xf9400012  // ldr x18, [x0]
end_function F(dit_pool)

// uint32_t dit_literal(void): an `ldr w0, literal` of a word in .text that decodes as svc #0x1234.
function F(dit_literal)
    ldr     w0, 1f
    ret
    .p2align 2
1:  .word   0xd4024681
end_function F(dit_literal)

// int dit_switch(int i): a jump table in .text of offsets to three cases, as a compiler makes one.
function F(dit_switch)
    cmp     w0, #2
    b.hi    9f
    adr     x1, 1f
    ldrsw   x2, [x1, w0, uxtw #2]
    add     x1, x1, x2
    br      x1
2:  mov     w0, #10
    ret
3:  mov     w0, #20
    ret
4:  mov     w0, #30
    ret
9:  mov     w0, #-1
    ret
    .p2align 2
1:  .word   2b - 1b
    .word   3b - 1b
    .word   4b - 1b
end_function F(dit_switch)

// long dit_getpid(void): real code, written as words after a data range with no $x between, as musl's inline
// asm syscall stubs sit in LLVM's output: mov x8, #172; svc #0; ret. The assembler marks all of them as data.
function F(dit_getpid)
    .word   0xd2801588  // mov x8, #172 (getpid)
    .word   0xd4000001  // svc #0
    .word   0xd65f03c0  // ret
end_function F(dit_getpid)

// The counter-cases: real code that sits right after a loaded literal pool and that nothing but an address, a call
// or a pointer in data names. Each is called and must still return the pid, i.e. its svc #0 must be rewritten: a
// split that took the stub for the pool's continuation would leave the svc in place and crash on iOS.
// long dit_via_adr(void): the stub's address is taken with adr and called with blr.
function F(dit_via_adr)
    stp     x29, x30, [sp, #-16]!
    adr     x1, 1f
    ldr     w0, [x1]
    adr     x9, 2f
    blr     x9
    ldp     x29, x30, [sp], #16
    ret
    .p2align 2
1:  .word   0xd4024681  // svc #0x1234 as data
2:
    ldr     w9, 3f
    mov     x8, #172
    svc     #0
    ret
    .p2align 2
// A pool of the stub's own: a stub only an address names is weakly reached, so its pool is code to the scan, and
// the word is one it can scan.
3:  .word   0x12345678
end_function F(dit_via_adr)

// long dit_via_bl(void): the stub, written as instructions, is only called with bl: reached, not proven.
function F(dit_via_bl)
    stp     x29, x30, [sp, #-16]!
    adr     x1, 1f
    ldr     w0, [x1]
    bl      2f
    ldp     x29, x30, [sp], #16
    ret
    .p2align 2
1:  .word   0xd4024681
2:
    ldr     w9, 3f
    mov     x8, #172
    svc     #0
    ret
    .p2align 2
3:  .word   0x12345678  // a callee is not proven by its caller: weakly reached, so its pool is code to the scan
end_function F(dit_via_bl)

// long dit_via_ptr(void): the stub is reached through a pointer in .data (a relative relocation; a local symbol too
// in dit-sym).
function F(dit_via_ptr)
    stp     x29, x30, [sp, #-16]!
    adr     x1, 1f
    ldr     w0, [x1]
    adrp    x9, F(dit_ptr_slot)
    ldr     x9, [x9, :lo12:F(dit_ptr_slot)]
    blr     x9
    ldp     x29, x30, [sp], #16
    ret
    .p2align 2
1:  .word   0xd4024681
F(dit_ptr_stub):
    ldr     w9, 3f
    mov     x8, #172
    svc     #0
    ret
    .p2align 2
// A pool of the stub's own: a stub only an address names is weakly reached, so its pool is code to the scan, and
// the word is one it can scan.
3:  .word   0x12345678
end_function F(dit_via_ptr)

    .data
    .p2align 3
F(dit_ptr_slot):
    .quad   F(dit_ptr_stub)
    .text

// long dit_via_table(void): the stub is a local function (hidden: no dynamic symbol) with its own FDE, placed right
// after the pool, and named only by a 32-bit offset in .rodata, as a table of functions names them. Stripped, the FDE
// is all that says code starts there.
function F(dit_via_table)
    stp     x29, x30, [sp, #-16]!
    adr     x1, 1f
    ldr     w0, [x1]
    adrp    x9, F(dit_rel_slot)
    add     x9, x9, :lo12:F(dit_rel_slot)
    ldrsw   x2, [x9]
    add     x9, x9, x2
    blr     x9
    ldp     x29, x30, [sp], #16
    ret
    .p2align 2
1:  .word   0xd4024681
end_function F(dit_via_table)

function F(dit_hidden_stub)
    .hidden F(dit_hidden_stub)
    ldr     w9, 3f
    mov     x8, #172
    svc     #0
    ret
    .p2align 2
3:  .word   0x12345678  // only its FDE (a root without proof) names the stub: reached, not proven, so its pool is code
end_function F(dit_hidden_stub)

    .section .rodata
    .p2align 2
F(dit_rel_slot):
    .word   F(dit_hidden_stub) - F(dit_rel_slot)
    .text

// Layouts that made an earlier version of the split go wrong: each must give the pid.

// long dit_highword(void): an 8-byte literal whose high word decodes as `ldr x0, <+12>`, pointing into the next
// function's body; the next function is real code behind its own FDE.
function F(dit_highword_pool)
    ldr     x0, 1f
    ret
    .p2align 3
1:  .quad   0x5800006012345678
end_function F(dit_highword_pool)

function F(dit_highword)
    mov     x8, #172
    nop
    svc     #0
    ret
end_function F(dit_highword)

// long dit_behind(int i): a jump table in .text, relative to itself, with its cases behind it, reached only through
// the table: case 0 is mov x8, #172; svc #0; ret; case 1 returns -2.
function F(dit_behind)
    cmp     w0, #1
    b.hi    9f
    adr     x1, 1f
    ldrsw   x2, [x1, w0, uxtw #2]
    add     x1, x1, x2
    br      x1
    .p2align 2
1:  .word   2f - 1b
    .word   3f - 1b
2:  mov     x8, #172
    svc     #0
    ret
3:  mov     x0, #-2
    ret
9:  mov     x0, #-1
    ret
end_function F(dit_behind)

// long dit_add(void): the adr + blr stub, with an add that reads the adr result first.
function F(dit_add)
    stp     x29, x30, [sp, #-16]!
    adr     x1, 1f
    ldr     w0, [x1]
    adr     x9, 2f
    add     x10, x9, #12
    blr     x9
    ldp     x29, x30, [sp], #16
    ret
    .p2align 2
1:  .word   0xd4024681
2:  mov     x8, #172
    svc     #0
    ret
end_function F(dit_add)

// long dit_ldur(void): a header word in front of code, read with ldur at a negative offset from the code's address,
// which is then jumped to.
function F(dit_ldur)
    adr     x1, 2f
    ldur    w9, [x1, #-4]
    br      x1
    .p2align 2
1:  .word   0x00001234
2:  mov     x8, #172
    svc     #0
    ret
end_function F(dit_ldur)

// int dit_char(int i): a byte table of strings in .text read by an unbounded index, and a pointer in .data into its
// middle (not word aligned); dit_after is real code behind its own FDE.
function F(dit_char)
    adr     x1, 1f
    ldrb    w0, [x1, w0, uxtw]
    ret
    .p2align 2
1:  .asciz  "alpha"
F(dit_second):
    .asciz  "beta"
    .p2align 2
end_function F(dit_char)

function F(dit_after)
    mov     x8, #172
    svc     #0
    ret
end_function F(dit_after)

    .data
    .p2align 3
F(dit_second_ptr):
    .quad   F(dit_second)
    .text

// The roots of code that nothing else names, each without an FDE: dit_nofde (global, so a sized dynamic symbol, a
// trusted root, with a caspal pool that is refused if it is scanned as code), the library's init function (DT_INIT,
// hidden, trusted, the same) and its entry point (e_entry, hidden: a root without proof, so a benign pool); and, in the
// library that keeps its .symtab only, dit_dead, a hidden function nothing calls whose FUNC symbol is all that says
// code starts there.
    .text
    .globl  F(dit_nofde)
    .type   F(dit_nofde), %function
    .p2align 2
F(dit_nofde):
    ldr     w9, 3f
    mov     x8, #172
    svc     #0
    ret
    .p2align 2
3:  .word   0x4862fe44
    .size   F(dit_nofde), . - F(dit_nofde)

    .globl  F(dit_init)
    .hidden F(dit_init)
    .type   F(dit_init), %function
    .p2align 2
F(dit_init):
    ldr     w9, 3f
    ret
    .p2align 2
3:  .word   0x4862fe44
    .size   F(dit_init), . - F(dit_init)

    .globl  F(dit_entry)
    .hidden F(dit_entry)
    .type   F(dit_entry), %function
    .p2align 2
F(dit_entry):
    ldr     w9, 3f
    ret
    .p2align 2
3:  .word   0x12345678  // e_entry is a root without proof: reached, not proven, so the pool is code
    .size   F(dit_entry), . - F(dit_entry)

#ifdef HAS_SYMTAB
    .hidden F(dit_dead)
    .type   F(dit_dead), %function
    .p2align 2
F(dit_dead):
    ldr     w9, 3f
    ret
    .p2align 2
3:  .word   0x4862fe44
    .size   F(dit_dead), . - F(dit_dead)
#endif

// A pointer in .data to the high word of a pool: a weak root. That word decodes as `ldr x0, <+12>`, naming the svc of
// the function behind the pool; reached weakly it only blocks, it must not load, so the svc stays code and is
// rewritten.
// long dit_weak_victim(void) is that function: mov x8, #172; nop; svc #0; ret.
function F(dit_weak_pool)
    ldr     x0, 1f
    ret
    .p2align 3
1:  .word   0x12345678
F(dit_weak_target):
    .word   0x58000060  // ldr x0, <+12>
end_function F(dit_weak_pool)

function F(dit_weak_victim)
    mov     x8, #172
    nop
    svc     #0
    ret
end_function F(dit_weak_victim)

    .data
    .p2align 3
F(dit_weak_ptr):
    .quad   F(dit_weak_target)
    .text

// The init and fini arrays name strong roots: a constructor and a destructor, hidden, without an FDE, each with a pool
// of its own (a caspal on [x18]) that is refused if scanned as code. They run when the library is loaded and unloaded.
    .text
    .p2align 2
F(dit_ctor):
    ldr     w9, 3f
    ret
    .p2align 2
3:  .word   0x4862fe44
    .size   F(dit_ctor), . - F(dit_ctor)
    .type   F(dit_ctor), %function

    .p2align 2
F(dit_dtor):
    ldr     w9, 3f
    ret
    .p2align 2
3:  .word   0x4862fe44
    .type   F(dit_dtor), %function
    .size   F(dit_dtor), . - F(dit_dtor)

    .section .init_array, "aw"
    .p2align 3
    .quad   F(dit_ctor)
    .section .fini_array, "aw"
    .p2align 3
    .quad   F(dit_dtor)
    .text

// long dit_noret(void): the pid; on a cold path after a cbz that is never taken, a call to abort, which never returns,
// and behind it a pool (a caspal on [x18]) that the function loads: the call ends the run, so the pool is not reached.
function F(dit_noret)
    ldr     w9, 1f
    mov     x8, #172
    svc     #0
    cbz     x0, 2f
    ret
2:  stp     x29, x30, [sp, #-16]!
    bl      abort
    .p2align 2
1:  .word   0x4862fe44
end_function F(dit_noret)

// long dit_weak_x18(void): the pid; behind the svc (a barrier, so what follows is only reached) a cbz on xzr that is
// always taken jumps over a word that decodes as `st2 {v6.2s, v7.2s}, [sp], x18`: the one form that steps sp by x18,
// which no compiler writes. Never run, never proven: the converter keeps it as it is and does not refuse the library.
function F(dit_weak_x18)
    mov     x8, #172
    svc     #0
    cbz     xzr, 1f
    .word   0x0c928be6
1:  ret
end_function F(dit_weak_x18)
