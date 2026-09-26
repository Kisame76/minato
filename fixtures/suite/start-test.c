// start-test: where a program's start-up code runs (ADR-0028, decision 5). linker64 calls the ifunc
// resolvers and the constructors on the program's main thread, before main, and so must the runtime.
// The resolver blocks SIGUSR1 and the constructor SIGUSR2, each on the thread it runs on; main shows
// which of the two its own thread has blocked. Every line is fixed by Linux and bionic.
// Expected output: fixtures/expected/start-test.out
#include <signal.h>
#include <stdio.h>
#include <sys/syscall.h>

// The resolver runs before libc is set up, without a thread pointer: a raw system call, no errno, and
// nothing on its stack that a stack protector would guard.
static unsigned long resolver_set = 1UL << (SIGUSR1 - 1);

__attribute__((no_stack_protector)) static void block_raw(void) {
    register long x0 __asm__("x0") = SIG_BLOCK;
    register long x1 __asm__("x1") = (long)&resolver_set;
    register long x2 __asm__("x2") = 0;
    register long x3 __asm__("x3") = sizeof resolver_set;
    register long x8 __asm__("x8") = __NR_rt_sigprocmask;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x8) : "memory");
}

static int answer(void) {
    return 42;
}

__attribute__((no_stack_protector)) static int (*resolve_answer(void))(void) {
    block_raw();
    return answer;
}

int start_test_answer(void) __attribute__((ifunc("resolve_answer")));

__attribute__((constructor)) static void block_in_constructor(void) {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR2);
    sigprocmask(SIG_BLOCK, &set, NULL);
}

int main(void) {
    sigset_t blocked;
    sigprocmask(SIG_BLOCK, NULL, &blocked);
    printf("the ifunc answers %d\n", start_test_answer());
    printf("the ifunc resolver ran on the main thread: %s\n", sigismember(&blocked, SIGUSR1) ? "yes" : "no");
    printf("the constructor ran on the main thread: %s\n", sigismember(&blocked, SIGUSR2) ? "yes" : "no");
    return 0;
}
