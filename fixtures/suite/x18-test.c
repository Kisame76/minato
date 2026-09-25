// x18-test: every form of x18 that the converter puts into an island (ADR-0020 X5), from
// x18-test.S, and the runtime around x18: a syscall, sched_yield, a signal frame and a new thread.
// Every line is fixed by the Arm ARM and Linux.
// Expected output: fixtures/expected/x18-test.out
#define _GNU_SOURCE  // gettid
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ucontext.h>
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
uint64_t x18_skipped_write(uint64_t *cell);
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
