// fault-site-test's library: a byte read through a table's base and an index, as an engine reads a table, kept out of
// line so that the faulting load lies in this library (the x18 variant is in libfault-site-x18.S). And the engine's own
// SIGSEGV handler, as an engine's crash reporter installs one (libfault-site.h).
#include "libfault-site.h"

#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

__attribute__((noinline)) unsigned char fault_site_read(const unsigned char *base, unsigned long index) {
    return base[index];
}

static struct sigaction engine_old;
static int engine_ending;
static unsigned char engine_stack[64 * 1024];

static void say(const char *text) {
    (void)write(STDOUT_FILENO, text, strlen(text));
}

static void engine_handler(int number, siginfo_t *info, void *raw) {
    say(number == SIGSEGV && (unsigned long)info->si_addr == 0x1087b ? "engine handler: signal 11 at 0x1087b\n"
                                                                     : "engine handler: another signal\n");
    switch (engine_ending) {
        case FAULT_SITE_RETURN:
            sigaction(SIGSEGV, &engine_old, NULL);
            return;
        case FAULT_SITE_RAISE:
            sigaction(SIGSEGV, &engine_old, NULL);
            raise(SIGSEGV);
            return;
        case FAULT_SITE_ABORT:
            abort();
        case FAULT_SITE_RECOVER:
            ((ucontext_t *)raw)->uc_mcontext.pc += 4;
            return;
        default: {
            volatile unsigned char *bad = (volatile unsigned char *)0x2a;
            (void)*bad;
            return;
        }
    }
}

int fault_site_install(int ending) {
    engine_ending = ending;
    stack_t stack;
    memset(&stack, 0, sizeof stack);
    stack.ss_sp = engine_stack;
    stack.ss_size = sizeof engine_stack;
    if (sigaltstack(&stack, NULL) != 0) return -1;
    struct sigaction action;
    memset(&action, 0, sizeof action);
    action.sa_sigaction = engine_handler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    return sigaction(SIGSEGV, &action, &engine_old);
}
