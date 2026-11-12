// x18-caspalt-test: a compare-and-swap pair in the unprivileged form (FEAT_LSUI, Armv9.6) whose second pair is
// x18, x19, as code in a function of its own. CASPALT is undefined on every CPU without that feature, so it raises
// SIGILL there and never writes x18: a converter must keep it verbatim (the same trap as on Android) or refuse the
// library whole.
// Expected output: fixtures/expected/x18-caspalt-test.out
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>

void caspalt_x18(void);

__asm__(".text\n.p2align 2\n.type caspalt_x18,%function\ncaspalt_x18:\n"
        "    .cfi_startproc\n"
        "    mov x3, sp\n"
        "    .inst 0x49dcdc72\n"              // caspalt x28, x29, x18, x19, [x3]
        "    ret\n"
        "    .cfi_endproc\n"
        "    .size caspalt_x18, .-caspalt_x18\n");

static sigjmp_buf trap;

static void on_sigill(int signal) {
    (void)signal;
    siglongjmp(trap, 1);
}

int main(void) {
    struct sigaction action = {.sa_handler = on_sigill};
    sigaction(SIGILL, &action, NULL);
    int trapped = 0;
    if (sigsetjmp(trap, 1) == 0) {
        caspalt_x18();
    } else {
        trapped = 1;
    }
    printf("caspalt x28, x29, x18, x19 raises SIGILL on a CPU without FEAT_LSUI: %s\n", trapped ? "yes" : "no");
    return 0;
}
