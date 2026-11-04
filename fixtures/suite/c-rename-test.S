// c-rename-test (ADR-0020 option (c)): a function whose every x18 use is hard, so the islands refuse all of
// them and the converter renames x18 to a free caller-saved register across the WHOLE function (a c_range,
// not an island). This fixture is runtime-only (fixtures/tests.txt): a (c) function is ABI-correct only after
// conversion -- the raw assembly keeps its value in the platform x18 and does not save/restore it (it cannot:
// any plain x18 save/restore would be an islandable use that breaks (c)'s all-hard requirement), so it must
// not run natively. apk-run runs the converted code, where x18 is the rename register. The matching C harness
// loops it under a SIGALRM storm.
    .arch   armv8.1-a

.macro function name
    .text
    .globl  \name
    .type   \name, %function
    .p2align 2
\name:
.endm

// uint64_t x18_c_nested(uint64_t *cell): every x18 use lives in ONE exclusive window that never closes and
// reaches a register branch -- the terminating `br x11` (a branch through a register, which the rewriter
// cannot follow; step D keeps the refusal, since no store-exclusive is reachable to make a no-save island).
// There is no other x18 use to island, so option (c) renames x18 to a free register across the function. x11
// holds x30, so `br x11` returns to the caller; BR Xn is a hard transfer the (c) gate accepts as the
// function's end (a RET Xn with N != 30 would not be), and there is no adr/br into the interior. x9, x10,
// x12-x15 are free for the rename. Returns *cell + 6 in x0 (fed from x18 inside the window). A sized symbol is
// required (sized_function_bounds), so .size is set; the `function` macro does not.
//
// A hard transfer sits right before x18_c_nested so nothing falls through into its first instruction: option
// (c)'s gate refuses a function whose preceding word is not a hard transfer (B/BR/RET/ERET), and the C harness
// object the linker places before this one can end with a `bl __stack_chk_fail` stack-protector tail, which is
// not one. This `ret` is unreachable; it is kept because the whole .text is one section (no -ffunction-sections).
    .text
    .p2align 2
    ret
function x18_c_nested
    mov     x3, x0                  // plain first instruction (c_entry_runs_first_instruction); x3 = cell
    mov     x11, x30                // keep the return address in x11 for the register-branch terminator
    ldxr    x18, [x3]               // x18 use: load *cell into x18, opening an exclusive window
    add     x18, x18, #6            // x18 use: x18 = *cell + 6, still inside the never-closing window
    mov     x0, x18                 // x18 use: the result, read into x0, still inside the window
    br      x11                     // a register BRANCH the window reaches -> every use refused -> option (c)
    .size   x18_c_nested, . - x18_c_nested
