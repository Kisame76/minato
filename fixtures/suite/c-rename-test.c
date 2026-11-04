// c-rename-test (ADR-0020 option (c)): x18_c_nested's every x18 use is hard, so the converter renames x18 to
// a free register across the whole function (a c_range, no island, no monitor dependence). Run the converted
// function in a tight loop while a SIGALRM storm rains on this thread: every result must stay *cell + 6,
// proving the (c)-renamed code and its entry/exit sync islands run correctly when a signal lands in them. The
// result is fed from x18 (the rename register after conversion), so a corrupted rename or a mishandled signal
// inside a sync island would change it. Runtime-only (fixtures/tests.txt): see c-rename-test.S for why it must
// not run natively. Expected output: fixtures/expected/c-rename-test.out.
#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

uint64_t x18_c_nested(uint64_t *cell);

enum { storm_wanted = 100 };  // at least this many signals must land while the loop runs correctly
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

int main(void) {
    struct sigaction action;
    memset(&action, 0, sizeof action);
    action.sa_handler = on_alarm;
    action.sa_flags = SA_RESTART;
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
            uint64_t cell = 36;
            if (x18_c_nested(&cell) != 42) ok = 0;  // 36 + 6, fed from x18 inside the (c)-renamed window
        }
    }

    atomic_store(&storm_running, 0);
    pthread_join(signaller, NULL);
    printf("a (c)-renamed function runs correctly under a SIGALRM storm: %s\n",
           (ok && alarms >= storm_wanted) ? "yes" : "no");
    return 0;
}
