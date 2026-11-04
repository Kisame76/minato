// c-signal-test (ADR-0020 option (c), must-have 6 / design-A step A0): proves the signal path hands a handler the
// guest x18 of a (c)-renamed function from the rename register, and carries the handler's write back to it. The
// function keeps a sentinel in x18 and parks in a spin loop until a flag is set, so a signal is guaranteed to land
// while x18 (the rename register after conversion) holds the sentinel; the C harness's handler reads and writes it.
// Runtime-only (fixtures/tests.txt): like c-rename-test, a (c) function is ABI-correct only after conversion -- the
// raw assembly keeps its value in the platform x18 without saving it -- so it must run converted, under apk-run.
    .arch   armv8.1-a

.macro function name
    .text
    .globl  \name
    .type   \name, %function
    .p2align 2
\name:
.endm

// uint64_t x18_c_spin(volatile uint64_t *release, const uint64_t *sentinel_cell):
//   loads *sentinel_cell into x18, spins until *release != 0, then returns x18.
// Every x18 use lives in ONE exclusive window that never closes and reaches a register branch (the terminating
// `br x9`, x9 holding x30), so the islands refuse all of them (step D keeps the refusal: no store-exclusive is
// reachable to make a no-save island) and option (c) renames x18 to a free register across the whole function. The
// spin loop's back-edge is an internal branch to an interior word, not an entry from outside, so the (c) gate still
// takes the function. The window never does a store-exclusive; not closing the monitor is harmless, and a signal
// clears the monitor but not the rename register, so the sentinel survives every signal.
//
// The C harness gates its handler on pc in [x18_c_spin + 8, x18_c_spin + 24): the first two words (mov, ldxr) load
// the sentinel into the rename register, and from word 2 (the spin loop) through the `br` the register holds it, so
// a handler landing there reads the sentinel and a write reaches the resumed function. The guard `ret` before the
// label gives the entry a hard-transfer predecessor (one .text section, no -ffunction-sections), as in c-rename-test.S.
    .text
    .p2align 2
    ret
function x18_c_spin
    mov     x9, x30                 // +0  keep the return address for the register-branch terminator (no x18)
    ldxr    x18, [x1]               // +4  x18 use: open an exclusive window, x18 = *sentinel_cell (the sentinel)
1:  ldr     x2, [x0]                // +8  x2 = *release (no x18); the spin loop the handler is gated to land in
    cbz     x2, 1b                  // +12 spin while the flag is 0, keeping the sentinel live in x18
    mov     x0, x18                 // +16 x18 use: return the sentinel (or the handler's value) read from x18
    br      x9                      // +20 a register BRANCH the unclosed window reaches -> every use refused -> (c)
    .size   x18_c_spin, . - x18_c_spin
