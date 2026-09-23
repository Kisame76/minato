// signal-test: signals (M2 step 5c) as bionic programs use them: handlers with siginfo, masks,
// pending and queued signals, SA_RESETHAND, SA_NODEFER, sa_mask, faults (SIGSEGV, SIGBUS, SIGTRAP,
// SIGILL) with their codes and addresses, handlers that change the context they return to, the
// alternate stack, signals between threads, interrupted system calls with and without SA_RESTART,
// sigsuspend, sigwait, ignored signals and abort. Every line is fixed by Linux and bionic.
// Expected output: fixtures/expected/signal-test.out
#define _GNU_SOURCE  // sigset helpers, REG_* names
#include <errno.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ucontext.h>
#include <unistd.h>

static volatile sig_atomic_t handled;
static volatile int last_signal, last_code, from_this_process, blocked_inside, other_blocked_inside;
static volatile void *last_address;

static void on_signal(int signal, siginfo_t *info, void *context) {
    (void)context;
    ++handled;
    last_signal = signal;
    last_code = info->si_code;
    from_this_process = info->si_pid == getpid();
    sigset_t now;
    sigprocmask(SIG_BLOCK, NULL, &now);
    blocked_inside = sigismember(&now, signal);
    other_blocked_inside = sigismember(&now, SIGUSR2);
}

static void install(int signal, void (*handler)(int, siginfo_t *, void *), int flags, int also_block) {
    struct sigaction action;
    memset(&action, 0, sizeof action);
    action.sa_sigaction = handler;
    action.sa_flags = SA_SIGINFO | flags;
    sigemptyset(&action.sa_mask);
    if (also_block) sigaddset(&action.sa_mask, also_block);
    sigaction(signal, &action, NULL);
}

static int is_blocked(int signal) {
    sigset_t now;
    sigprocmask(SIG_BLOCK, NULL, &now);
    return sigismember(&now, signal);
}

static void basics(void) {
    install(SIGUSR1, on_signal, 0, SIGUSR2);
    raise(SIGUSR1);
    printf("raise: handled %d, signal %d, code %d, from this process %d, blocked inside %d, sa_mask inside %d, "
           "blocked after %d\n",
           handled, last_signal, last_code, from_this_process, blocked_inside, other_blocked_inside,
           is_blocked(SIGUSR1));

    sigset_t block;
    sigemptyset(&block);
    sigaddset(&block, SIGUSR1);
    sigprocmask(SIG_BLOCK, &block, NULL);
    handled = 0;
    kill(getpid(), SIGUSR1);
    sigset_t pending;
    sigpending(&pending);
    printf("kill while blocked: handled %d, pending %d\n", handled, sigismember(&pending, SIGUSR1));
    sigprocmask(SIG_UNBLOCK, &block, NULL);
    printf("after unblocking: handled %d, code %d (SI_USER)\n", handled, last_code);

    install(SIGUSR1, on_signal, SA_NODEFER, 0);
    raise(SIGUSR1);
    printf("SA_NODEFER: blocked inside %d\n", blocked_inside);

    install(SIGUSR1, on_signal, SA_RESETHAND, 0);
    handled = 0;
    raise(SIGUSR1);
    struct sigaction now;
    sigaction(SIGUSR1, NULL, &now);
    printf("SA_RESETHAND: handled %d, then the default: %d\n", handled, now.sa_handler == SIG_DFL);

    signal(SIGUSR2, SIG_IGN);
    raise(SIGUSR2);
    printf("ignored SIGUSR2: still here, kill(getpid(), 0) = %d\n", kill(getpid(), 0));
    signal(SIGUSR2, SIG_DFL);
    const int refused = sigaction(SIGKILL, &now, NULL);
    printf("sigaction(SIGKILL): %d, errno %d\n", refused, errno);
}

static sigjmp_buf recover;

static void on_fault(int signal, siginfo_t *info, void *context) {
    (void)context;
    last_signal = signal;
    last_code = info->si_code;
    last_address = info->si_addr;
    siglongjmp(recover, 1);
}

static void faults(void) {
    install(SIGSEGV, on_fault, 0, 0);
    install(SIGBUS, on_fault, 0, 0);
    install(SIGTRAP, on_fault, 0, 0);
    install(SIGILL, on_fault, 0, 0);
    if (sigsetjmp(recover, 1) == 0) *(volatile int *)0 = 1;
    printf("null write: signal %d, code %d (SEGV_MAPERR), address is NULL %d\n", last_signal, last_code,
           last_address == NULL);

    char *page = mmap(NULL, 16384, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (sigsetjmp(recover, 1) == 0) *(volatile char *)(page + 8) = 1;
    printf("write to a read-only page: signal %d, code %d (SEGV_ACCERR), address matches %d\n", last_signal, last_code,
           last_address == page + 8);
    munmap(page, 16384);

    static uint64_t words[4];
    char *misaligned = (char *)words + 1;
    if (sigsetjmp(recover, 1) == 0) {
        uint64_t value;
        __asm__ volatile("ldxr %0, [%1]" : "=r"(value) : "r"(misaligned) : "memory");
    }
    printf("misaligned exclusive load: signal %d, code %d (BUS_ADRALN), address matches %d\n", last_signal, last_code,
           last_address == misaligned);

    if (sigsetjmp(recover, 1) == 0) __builtin_trap();
    printf("__builtin_trap: signal %d, code %d (TRAP_BRKPT)\n", last_signal, last_code);
    if (sigsetjmp(recover, 1) == 0) __asm__ volatile(".inst 0x00000000");
    printf("undefined instruction: signal %d, code %d (ILL_ILLOPC)\n", last_signal, last_code);
    printf("the mask after siglongjmp: SIGSEGV blocked %d\n", is_blocked(SIGSEGV));
}

// A handler that skips the faulting instruction by moving pc on, as ART's implicit null checks
// change the context; everything else must come back as it was.
static void skip_instruction(int signal, siginfo_t *info, void *raw) {
    (void)signal;
    (void)info;
    ucontext_t *context = raw;
    context->uc_mcontext.pc += 4;
    context->uc_mcontext.regs[20] = 0x2020;
}

static void changed_context(void) {
    install(SIGSEGV, skip_instruction, 0, 0);
    uint64_t after = 0, kept = 0, changed = 0;
    __asm__ volatile(
        "mov x19, #0x1919\n"
        "mov x20, #0\n"
        "mov x9, #0\n"
        "str wzr, [x9]\n"  // faults; the handler moves pc past it
        "mov %0, #1\n"
        "mov %1, x19\n"
        "mov %2, x20\n"
        : "=r"(after), "=r"(kept), "=r"(changed)
        :
        : "x9", "x19", "x20", "memory");
    printf("handler moved pc on: continued %d, x19 kept %#llx, x20 set by the handler %#llx\n", (int)after,
           (unsigned long long)kept, (unsigned long long)changed);
}

static char *alternate;
static volatile int on_alternate;
static void check_stack(int signal, siginfo_t *info, void *context) {
    (void)signal;
    (void)info;
    (void)context;
    char local;
    on_alternate = &local >= alternate && &local < alternate + 65536;
}

static void alternate_stack(void) {
    alternate = malloc(65536);
    stack_t stack = {.ss_sp = alternate, .ss_flags = 0, .ss_size = 65536};
    sigaltstack(&stack, NULL);
    install(SIGUSR2, check_stack, SA_ONSTACK, 0);
    raise(SIGUSR2);
    printf("SA_ONSTACK: handler on the alternate stack %d\n", on_alternate);
    install(SIGUSR2, check_stack, 0, 0);
    raise(SIGUSR2);
    printf("without SA_ONSTACK: on the alternate stack %d\n", on_alternate);
    stack.ss_flags = SS_DISABLE;
    sigaltstack(&stack, NULL);
}

static int queued_values[8];
static volatile int queued_count;
static void on_queued(int signal, siginfo_t *info, void *context) {
    (void)signal;
    (void)context;
    if (queued_count < 8) queued_values[queued_count++] = info->si_value.sival_int;
    last_code = info->si_code;
}

static void real_time(void) {
    const int signal = SIGRTMIN + 2;
    install(signal, on_queued, 0, 0);
    sigset_t block;
    sigemptyset(&block);
    sigaddset(&block, signal);
    sigprocmask(SIG_BLOCK, &block, NULL);
    for (int value = 1; value <= 3; ++value) sigqueue(getpid(), signal, (union sigval){.sival_int = value});
    printf("queued while blocked: handled %d\n", queued_count);
    sigprocmask(SIG_UNBLOCK, &block, NULL);
    printf("real-time signal after unblocking: handled %d times, values %d %d %d in order, code %d (SI_QUEUE)\n",
           queued_count, queued_values[0], queued_values[1], queued_values[2], last_code);
}

static int pipe_ends[2];
static void on_interrupt(int signal, siginfo_t *info, void *context) {
    (void)signal;
    (void)info;
    (void)context;
    ++handled;
}

static void *read_pipe(void *argument) {
    (void)argument;
    char byte = 0;
    const ssize_t result = read(pipe_ends[0], &byte, 1);
    return (void *)(intptr_t)(result == 1 ? byte : -errno);
}

static void *wait_for_signal(void *argument) {
    (void)argument;
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR2);
    int received = 0;
    sigwait(&set, &received);
    return (void *)(intptr_t)received;
}

static void between_threads(void) {
    pipe(pipe_ends);
    install(SIGUSR1, on_interrupt, 0, 0);
    handled = 0;
    pthread_t reader;
    pthread_create(&reader, NULL, read_pipe, NULL);
    usleep(50 * 1000);
    pthread_kill(reader, SIGUSR1);
    void *result;
    pthread_join(reader, &result);
    printf("read interrupted without SA_RESTART: handled %d, result %d (-EINTR)\n", handled, (int)(intptr_t)result);

    install(SIGUSR1, on_interrupt, SA_RESTART, 0);
    handled = 0;
    pthread_create(&reader, NULL, read_pipe, NULL);
    usleep(50 * 1000);
    pthread_kill(reader, SIGUSR1);
    usleep(50 * 1000);
    write(pipe_ends[1], "r", 1);
    pthread_join(reader, &result);
    printf("read with SA_RESTART: handled %d, then read '%c'\n", handled, (char)(intptr_t)result);

    sigset_t block;
    sigemptyset(&block);
    sigaddset(&block, SIGUSR2);
    pthread_sigmask(SIG_BLOCK, &block, NULL);  // the waiter inherits it; sigwait needs it blocked
    pthread_t waiter;
    pthread_create(&waiter, NULL, wait_for_signal, NULL);
    usleep(50 * 1000);
    pthread_kill(waiter, SIGUSR2);
    pthread_join(waiter, &result);
    printf("sigwait in another thread got signal %d\n", (int)(intptr_t)result);
    pthread_sigmask(SIG_UNBLOCK, &block, NULL);
}

static pthread_t main_thread;
static void *signal_main_later(void *argument) {
    (void)argument;
    usleep(50 * 1000);
    pthread_kill(main_thread, SIGUSR1);
    return NULL;
}

static void suspend(void) {
    install(SIGUSR1, on_interrupt, 0, 0);
    sigset_t block, empty;
    sigemptyset(&block);
    sigaddset(&block, SIGUSR1);
    sigemptyset(&empty);
    sigprocmask(SIG_BLOCK, &block, NULL);
    handled = 0;
    main_thread = pthread_self();
    pthread_t sender;
    pthread_create(&sender, NULL, signal_main_later, NULL);
    const int result = sigsuspend(&empty);
    const int error = errno;
    pthread_join(sender, NULL);
    printf("sigsuspend: %d errno %d, handled %d, mask restored %d\n", result, error, handled, is_blocked(SIGUSR1));
    sigprocmask(SIG_UNBLOCK, &block, NULL);
}

static void on_abort(int signal, siginfo_t *info, void *context) {
    (void)info;
    (void)context;
    last_signal = signal;
    siglongjmp(recover, 1);
}

static void aborting(void) {
    install(SIGABRT, on_abort, 0, 0);
    if (sigsetjmp(recover, 1) == 0) abort();
    printf("abort reached its handler: signal %d\n", last_signal);
}

int main(void) {
    basics();
    faults();
    changed_context();
    alternate_stack();
    real_time();
    between_threads();
    suspend();
    aborting();
    printf("done\n");
    return 0;
}
