// x18-test: every form of x18 that the converter puts into an island (ADR-0020 X5), from
// x18-test.S, and the runtime around x18: a syscall, sched_yield, a signal frame and a new thread.
// Every line is fixed by the Arm ARM and Linux.
// Expected output: fixtures/expected/x18-test.out
#define _GNU_SOURCE  // gettid
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <sys/ucontext.h>
#include <time.h>
#include <unistd.h>

uint64_t x18_move(uint64_t value);
uint64_t x18_add_w(uint64_t value);
void x18_load_store(uint64_t cells[5]);
uint64_t x18_base(uint64_t cells[4]);
void x18_pair(uint64_t cells[4]);
uint64_t x18_stack(uint64_t value);
void x18_flags(int64_t a, int64_t b, uint64_t out[4]);
void x18_multiply(uint64_t a, uint64_t b, uint64_t c, uint64_t out[4]);
void x18_bits(uint64_t value, uint64_t out[3]);
uint64_t x18_simd(uint64_t value);
void x18_nzcv(uint64_t out[3]);
void x18_atomics(uint64_t cells[5]);
void x18_dump(uint64_t cells[6]);
uint64_t x18_adrp(void);
uint64_t x18_adr(void);
void x18_literals(uint64_t out[3]);
uint64_t x18_branches(uint64_t value);
uint64_t x18_ll_sc_value(uint64_t *cell);
uint64_t x18_ll_sc_address(uint64_t *cell);
uint64_t x18_ll_sc_w(uint32_t *cell);
uint64_t x18_cmpxchg(uint64_t *cell, uint64_t expected, uint64_t desired);
uint64_t x18_clrex_only(uint64_t *cell);
uint64_t x18_cas_exit(uint64_t *cell, uint64_t expected, uint64_t desired);
uint64_t x18_cas_weak(uint64_t *cell, uint64_t expected, uint64_t desired);
uint64_t x18_swap_exit(uint64_t *cell, uint64_t value);
uint32_t x18_add_bound(uint32_t *cell, uint32_t delta, uint32_t limit);
uint64_t x18_skipped_write(uint64_t *cell);
uint64_t x18_nosave_read(uint64_t *cell);
uint64_t x18_nosave_write(void);
int64_t x18_switch(uint64_t index);
uint64_t x18_syscall(uint64_t value);
uint64_t x18_yield(uint64_t value, uint64_t times);
uint64_t x18_raise(uint64_t value, uint64_t pid, uint64_t tid);
uint64_t x18_read(void);
extern const uint64_t x18_data;

static volatile uint64_t seen_x18;

static void on_usr1(int signal, siginfo_t *info, void *context) {
    (void)signal;
    (void)info;
    ucontext_t *uc = context;
    seen_x18 = uc->uc_mcontext.regs[18];
    uc->uc_mcontext.regs[18] = 0x5eed;
}

static uint64_t thread_x18;

static void *read_x18(void *argument) {
    (void)argument;
    thread_x18 = x18_read();
    return NULL;
}

static const char *yes(int condition) {
    return condition ? "yes" : "no";
}

// (b) ADR-0020 C7: run both no-save islands in a tight loop while a signaller thread delivers SIGALRM to
// this thread every ~1 ms, so a signal lands inside a no-save island and the runtime's leave() reconstructs
// x18 from slot 11 without reading the red zone. The write use's LL/SC cell IS the red-zone address
// (sp - 128), so a stray red-zone access by leave() would corrupt it -- a wrong result would catch it.
// (pthread_kill, as x18-stress uses it; ITIMER_REAL timers do not fire in the runtime.) Passes when every
// result stays correct across at least `storm_wanted` signals.
enum { storm_wanted = 100 };
static volatile sig_atomic_t alarms;
static atomic_int storm_running;
static pthread_t storm_target;

static void on_alarm(int signal) {
    (void)signal;
    ++alarms;
}

static void *storm_signaller(void *argument) {
    (void)argument;
    const struct timespec ms = {0, 1000000};  // ~1 ms between signals
    while (atomic_load(&storm_running)) {
        pthread_kill(storm_target, SIGALRM);
        nanosleep(&ms, NULL);
    }
    return NULL;
}

static int64_t milliseconds_since(const struct timespec *start) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - start->tv_sec) * 1000 + (now.tv_nsec - start->tv_nsec) / 1000000;
}

static char storm_alt_stack[64 * 1024];

static int nosave_storm(void) {
    // The handler runs on an alt stack so its signal frame does not land on the main stack's red zone,
    // where x18_nosave_write's LL/SC cell sits (sp - 128): that collision is a property of stacks, not of
    // the no-save island, which never touches the red zone.
    stack_t alt = {.ss_sp = storm_alt_stack, .ss_size = sizeof storm_alt_stack, .ss_flags = 0};
    sigaltstack(&alt, NULL);
    struct sigaction action;
    memset(&action, 0, sizeof action);
    action.sa_handler = on_alarm;
    action.sa_flags = SA_RESTART | SA_ONSTACK;
    sigaction(SIGALRM, &action, NULL);
    storm_target = pthread_self();
    atomic_store(&storm_running, 1);
    pthread_t signaller;
    pthread_create(&signaller, NULL, storm_signaller, NULL);

    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    int ok = 1;
    while (ok && alarms < storm_wanted && milliseconds_since(&start) < 10 * 1000) {
        for (int i = 0; i < 10000 && ok; ++i) {
            uint64_t cell = 37;
            if (x18_nosave_read(&cell) != 42) ok = 0;  // the read island, over x5
            if (x18_nosave_write() != 42) ok = 0;      // the write island, cell at sp - 128
        }
    }

    atomic_store(&storm_running, 0);
    pthread_join(signaller, NULL);
    return ok && alarms >= storm_wanted;
}

struct contended {
    uint64_t *counter;
    uint32_t *added;
};

// Contends for a counter by the strong CAS and for a word by the bounded add, both with x18 read in the loop.
static void *contend(void *argument) {
    struct contended *c = argument;
    for (int i = 0; i < 100000; i++) {
        for (;;) {
            const uint64_t old = *(volatile uint64_t *)c->counter;
            if (x18_cas_exit(c->counter, old, old + 1) == old) break;
        }
        x18_add_bound(c->added, 1, 0xffffffffu);
    }
    return NULL;
}

int main(void) {
    printf("mov, add and mov through x18: %" PRIu64 "\n", x18_move(41));
    printf("add w18 of 0x1fffffffe and 1: 0x%" PRIx64 "\n", x18_add_w(0x1fffffffe));

    uint64_t cells[8] = {0x8877665544332211};
    x18_load_store(cells);
    printf("loads and stores of x18: 0x%" PRIx64 " 0x%" PRIx64 " 0x%" PRIx64 " 0x%" PRIx64 "\n", cells[1], cells[2],
           cells[3], cells[4]);
    uint64_t based[4] = {0, 0, 100, 0};
    printf("x18 as the base, with post-index: %" PRIu64 "\n", x18_base(based));
    uint64_t pair[4] = {5, 6, 0, 0};
    x18_pair(pair);
    printf("ldp and stp with x18: %" PRIu64 " %" PRIu64 "\n", pair[2], pair[3]);
    printf("x18 next to sp: %" PRIu64 "\n", x18_stack(1000));

    uint64_t out[4] = {0};
    x18_flags(-1, 1, out);
    printf("cmp, adds, csel and ccmp on x18: %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 "\n", out[0], out[1], out[2],
           out[3]);
    x18_multiply(6, 7, 5, out);
    printf("madd, udiv, msub and umulh with x18: %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 "\n", out[0], out[1],
           out[2], out[3]);
    x18_bits(0x1122334455667788, out);
    printf("movk, bfi and ubfx on x18: 0x%" PRIx64 " 0x%" PRIx64 " 0x%" PRIx64 "\n", out[0], out[1], out[2]);
    printf("fmov, ins and umov with x18: 0x%" PRIx64 "\n", x18_simd(0x0123456789abcdef));
    x18_nzcv(out);
    printf("msr and mrs nzcv through x18: %" PRIu64 " %" PRIu64 " 0x%" PRIx64 "\n", out[0], out[1], out[2]);

    uint64_t atomics[5] = {10};
    x18_atomics(atomics);
    printf("ldadd, swp, cas, ldar and stlr with x18: %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 "\n",
           atomics[2], atomics[3], atomics[4], atomics[0], atomics[1]);
    uint64_t dump[6] = {0};
    x18_dump(dump);
    printf("a register dump stores x18 between its neighbours:");
    for (int index = 0; index < 6; ++index) printf(" %" PRIu64, dump[index]);
    printf("\n");

    printf("adrp and add into x18 give the address of a symbol: %s\n", yes(x18_adrp() == (uint64_t)&x18_data));
    printf("adr into x18 gives the address of a symbol: %s\n", yes(x18_adr() == (uint64_t)&x18_data));
    x18_literals(out);
    printf("ldr x18, ldr w18 and ldrsw x18 from literals: 0x%" PRIx64 " 0x%" PRIx64 " 0x%" PRIx64 "\n", out[0], out[1],
           out[2]);
    printf("cbz, cbnz, tbz and tbnz on x18: 0x%" PRIx64 " for 9, 0x%" PRIx64 " for 0\n", x18_branches(9),
           x18_branches(0));

    uint64_t cell = 41;
    printf("an LL/SC window with x18 as the value: %" PRIu64 "\n", x18_ll_sc_value(&cell));
    printf("an LL/SC window with x18 as the address: %" PRIu64 "\n", x18_ll_sc_address(&cell));
    uint32_t word = 0xffffffff;
    printf("an LL/SC window in 32 bits: 0x%" PRIx64 "\n", x18_ll_sc_w(&word));
    cell = 7;
    const uint64_t first = x18_cmpxchg(&cell, 7, 8), after_first = cell;
    const uint64_t second = x18_cmpxchg(&cell, 7, 9), after_second = cell;
    printf("cmpxchg through x18: %s %" PRIu64 " for %" PRIu64 ", then %s %" PRIu64 "\n", first == 7 ? "swapped" : "missed",
           first, after_first, second == 7 ? "swapped" : "kept", after_second);
    printf("a window a CLREX closes: %" PRIu64 "\n", x18_clrex_only(&cell));
    uint64_t zero = 0, five = 5;
    printf("a window that skips its only write of x18: %" PRIu64 ", and one that makes it: %" PRIu64 "\n",
           x18_skipped_write(&zero), x18_skipped_write(&five));
    uint64_t nosave_cell = 37;
    printf("a no-save island for a live LL/SC read use: %" PRIu64 "\n", x18_nosave_read(&nosave_cell));
    printf("a no-save island for a live LL/SC write use at sp-128: %" PRIu64 "\n", x18_nosave_write());
    printf("the no-save islands run correctly under a SIGALRM storm: %s\n", yes(nosave_storm()));
    printf("a jump table through br x18:");
    for (uint64_t index = 0; index < 5; ++index) printf(" %" PRId64, x18_switch(index));
    printf("\n");

    const uint64_t kept = 0x1234567887654321;
    const uint64_t after_syscall = x18_syscall(kept);
    if (after_syscall == kept) {
        printf("x18 across a syscall: kept\n");
    } else {
        printf("x18 across a syscall: 0x%" PRIx64 "\n", after_syscall);
    }
    printf("x18 across 1000 sched_yield: %s\n", x18_yield(0xabcdef, 1000) == 0 ? "kept" : "changed");

    // ADR-0020 X7: the loops that compilers make for atomics, with x18 read in them and a branch out of the loop.
    cell = 7;
    const uint64_t cas_hit = x18_cas_exit(&cell, 7, 100), cas_after_hit = cell;
    const uint64_t cas_miss = x18_cas_exit(&cell, 7, 200), cas_after_miss = cell;
    printf("a strong CAS with x18 as the new value: old %" PRIu64 " now %" PRIu64 ", then old %" PRIu64 " now %" PRIu64
           "\n", cas_hit, cas_after_hit, cas_miss, cas_after_miss);
    cell = 5;
    const uint64_t weak_hit = x18_cas_weak(&cell, 5, 6), weak_after_hit = cell;
    const uint64_t weak_miss = x18_cas_weak(&cell, 5, 7), weak_after_miss = cell;
    printf("a weak CAS with x18 as the new value: status %" PRIu64 " now %" PRIu64 ", then status %" PRIu64
           " now %" PRIu64 "\n", weak_hit, weak_after_hit, weak_miss, weak_after_miss);
    cell = 0;
    const uint64_t swap_zero = x18_swap_exit(&cell, 33), swap_zero_after = cell;
    cell = 9;
    const uint64_t swap_nine = x18_swap_exit(&cell, 33), swap_nine_after = cell;
    printf("an exchange with x18, left alone on 0: old %" PRIu64 " now %" PRIu64 ", old %" PRIu64 " now %" PRIu64 "\n",
           swap_zero, swap_zero_after, swap_nine, swap_nine_after);
    uint32_t bounded = 3;
    const uint32_t b1 = x18_add_bound(&bounded, 4, 10), b2 = x18_add_bound(&bounded, 4, 10);
    const uint32_t b3 = x18_add_bound(&bounded, 4, 10);
    printf("an add with x18 up to a bound: old %u, %u, %u, now %u\n", b1, b2, b3, bounded);
    {
        uint64_t counter = 0;
        uint32_t added = 0;
        pthread_t workers[8];
        struct contended contended = {&counter, &added};
        for (int i = 0; i < 4; i++) pthread_create(&workers[i], NULL, contend, &contended);
        for (int i = 0; i < 4; i++) pthread_join(workers[i], NULL);
        printf("4 threads, 100000 passes each: strong CAS sum %" PRIu64 ", bounded add sum %u\n", counter, added);
    }

    struct sigaction action;
    memset(&action, 0, sizeof action);
    action.sa_sigaction = on_usr1;
    action.sa_flags = SA_SIGINFO;
    sigaction(SIGUSR1, &action, NULL);
    const uint64_t after_handler = x18_raise(0x1818aaaa, (uint64_t)getpid(), (uint64_t)gettid());
    printf("the handler found x18 in its frame: 0x%" PRIx64 ", the thread went on with the handler's: 0x%" PRIx64 "\n",
           seen_x18, after_handler);

    pthread_t thread;
    pthread_create(&thread, NULL, read_x18, NULL);
    pthread_join(thread, NULL);
    const uint64_t main_x18 = x18_read();
    printf("a new thread's x18 is set: %s, and is not its creator's: %s\n", yes(thread_x18 != 0),
           yes(thread_x18 != main_x18));
    return 0;
}
