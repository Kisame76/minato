// x18-st2-test: a store of two vectors with sp as the base and x18 as the post-index register, as code, in a
// function of its own with unwind tables and no symbol (the program is linked with -s: a stripped library, as an
// engine library of an APK often is).
// st2 {v6.2s, v7.2s}, [sp], x18 with x18 = 16 stores the interleaved elements 1, 2, 1, 2 at the old sp and moves sp
// by x18. Android runs it as it is; a converter must keep the stored words, sp's move and the caller's x18 right.
// Expected output: fixtures/expected/x18-st2-test.out
#include <stdint.h>
#include <stdio.h>

uint64_t st2_sp_x18(uint64_t *first_words);

__asm__(".text\n.p2align 2\n.type st2_sp_x18,%function\nst2_sp_x18:\n"
        "    .cfi_startproc\n"
        "    mov x10, x18\n"                  // x18 is the platform register: put it back before returning
        "    sub sp, sp, #32\n"
        "    .cfi_adjust_cfa_offset 32\n"
        "    mov x9, sp\n"                    // the base the store uses
        "    mov x18, #16\n"                  // the post-index amount
        "    movi v6.2s, #1\n"
        "    movi v7.2s, #2\n"
        "    .inst 0x0c928be6\n"              // st2 {v6.2s, v7.2s}, [sp], x18
        "    .cfi_adjust_cfa_offset -16\n"
        "    mov x11, sp\n"
        "    sub x11, x11, x9\n"              // how far sp moved
        "    ldr x12, [x9]\n"                 // the first two stored words
        "    str x12, [x0]\n"
        "    add sp, sp, #16\n"               // 32 taken, 16 given back by the store: the rest
        "    .cfi_adjust_cfa_offset -16\n"
        "    mov x18, x10\n"
        "    mov x0, x11\n"
        "    ret\n"
        "    .cfi_endproc\n"
        "    .size st2_sp_x18, .-st2_sp_x18\n");

static const char *yes(int condition) {
    return condition ? "yes" : "no";
}

int main(void) {
    uint64_t first = 0;
    const uint64_t moved = st2_sp_x18(&first);
    printf("st2 with base sp and post-index x18: sp moved by 16: %s\n", yes(moved == 16));
    printf("st2 with base sp and post-index x18: stored words 1 then 2: %s\n", yes(first == 0x0000000200000001ull));
    return 0;
}
