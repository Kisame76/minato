// c-signal-test (ADR-0020 option (c), must-have 6 / design-A step A0): the runtime's signal path must hand a
// handler the guest x18 of a (c)-renamed function from the rename register (not the stale TSD slot 11), and must
// carry the handler's write to x18 back to that register when the handler returns. x18_c_spin keeps a sentinel in
// x18 and parks in a spin loop (c-signal-test.S) until *release is set; it can leave only once the handler sets it,
// so a SIGALRM of the storm is guaranteed to land while the thread spins with the sentinel live in x18. The handler
// (SA_SIGINFO), when the pc is in the spin window, reads regs[18] -- it must equal the sentinel -- writes SENTINEL2
// there, and releases the loop; the function then returns x18, which must be SENTINEL2. Before A0 the handler reads
// slot 11 (stale) and its write is lost, so both checks fail. Runtime-only: see c-signal-test.S. Expected output:
// fixtures/expected/c-signal-test.out.
#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <ucontext.h>

uint64_t x18_c_spin(volatile uint64_t *release, const uint64_t *sentinel_cell);

enum { sentinel = 0xA5A5000000000001ull, sentinel2 = 0x5A5A000000000002ull };
// The spin window the handler is gated to: from word 2 (the spin loop) through the `br`, where the rename register
// holds the sentinel (c-signal-test.S). Words 0-1 (mov, ldxr) load it first and are skipped.
enum { spin_from = 8, spin_to = 24 };

static volatile uint64_t release_flag;
static uint64_t sentinel_cell = sentinel;
static volatile sig_atomic_t fired;       // the handler acted inside the spin window
static uint64_t seen_x18;                  // regs[18] the handler was given there
static atomic_int storm_running;
static pthread_t storm_target;

static void on_alarm(int signal, siginfo_t *info, void *ucontext) {
    (void)signal;
    (void)info;
    if (fired) return;
    ucontext_t *uc = (ucontext_t *)ucontext;
    uint64_t pc = uc->uc_mcontext.pc;
    uintptr_t base = (uintptr_t)&x18_c_spin;
    if (pc < base + spin_from || pc >= base + spin_to) return;  // not in the spin window: let the loop run on
    seen_x18 = uc->uc_mcontext.regs[18];  // delivery: must be the sentinel (from the rename register), not slot 11
    uc->uc_mcontext.regs[18] = sentinel2;  // return: the function must see this in the rename register
    fired = 1;
    release_flag = 1;  // break the spin; read by the function only after the handler returns (same thread)
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

int main(void) {
    struct sigaction action;
    memset(&action, 0, sizeof action);
    action.sa_sigaction = on_alarm;
    action.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGALRM, &action, NULL);
    storm_target = pthread_self();
    atomic_store(&storm_running, 1);
    pthread_t signaller;
    pthread_create(&signaller, NULL, storm_signaller, NULL);

    // Parks in the spin loop with the sentinel in x18 until the handler releases it; returns x18 afterwards.
    uint64_t returned = x18_c_spin(&release_flag, &sentinel_cell);

    atomic_store(&storm_running, 0);
    pthread_join(signaller, NULL);

    int ok = fired && seen_x18 == sentinel && returned == sentinel2;
    printf("signal handler reads and writes the guest x18 of a (c)-renamed function: %s\n", ok ? "yes" : "no");
    return 0;
}
