// stw-test: Boehm's stop-the-world with SIGPWR and SIGXCPU among six threads, without ART (ADR-0033 test 4, part
// c alone; stw.h). Three threads collect in turn, a fourth spins without a syscall, a fifth sleeps in nanosleep and
// a sixth waits on a semaphore that only the end of the next stop posts. Each stop must stop every other thread,
// which must stay still until the restart; a lost signal shows as a stop or restart that is not answered within
// 10 s. Every line is fixed by Linux and bionic; the counts of where the stops found the threads are not printed.
// Expected output: fixtures/expected/stw-test.out
#define _GNU_SOURCE  // gettid
#include <stdlib.h>

#include "stw.h"

enum {
    collectors = 3,
    stops = 300,
    threads = collectors + 3,
    patience_seconds = 120,  // for the whole run
};

static _Atomic int finished;

static void *collect(void *unused) {
    (void)unused;
    struct stw_thread *self = stw_register();
    while (stw_try_collect(self, stops, true)) {
        stw_work(self, 2000);
        stw_sleep_us(self, 1000);
    }
    stw_unregister(self);
    atomic_fetch_add(&finished, 1);
    return NULL;
}

static void *spin(void *unused) {
    (void)unused;
    struct stw_thread *self = stw_register();
    while (!atomic_load(&stw.done)) stw_work(self, 1000);
    stw_unregister(self);
    atomic_fetch_add(&finished, 1);
    return NULL;
}

static void *sleep_often(void *unused) {
    (void)unused;
    struct stw_thread *self = stw_register();
    while (!atomic_load(&stw.done)) stw_sleep_us(self, 100);
    stw_unregister(self);
    atomic_fetch_add(&finished, 1);
    return NULL;
}

static void *wait_for_stops(void *unused) {
    (void)unused;
    struct stw_thread *self = stw_register();
    while (!atomic_load(&stw.done)) stw_wait_for_next_stop(self);
    stw_unregister(self);
    atomic_fetch_add(&finished, 1);
    return NULL;
}

static const char *yes(bool value) {
    return value ? "yes" : "no";
}

int main(void) {
    stw_init();
    void *(*const bodies[threads])(void *) = {spin, sleep_often, wait_for_stops, collect, collect, collect};
    pthread_t started[threads];
    for (int i = 0; i < threads; i++) pthread_create(&started[i], NULL, bodies[i], NULL);
    // A thread a lost signal left in its handler never ends, so the end is awaited, not joined.
    const long deadline = stw_now_ns() + patience_seconds * 1000000000L;
    while (atomic_load(&finished) < threads && !atomic_load(&stw.failed)) {
        if (stw_now_ns() > deadline) stw_fail("no end within %d s", patience_seconds);
        const struct timespec pause = {0, 10 * 1000 * 1000};
        nanosleep(&pause, NULL);
    }
    if (atomic_load(&stw.failed)) {
        printf("FAIL: %s\n", stw.failure);
        fflush(stdout);
        _exit(1);
    }
    for (int i = 0; i < threads; i++) pthread_join(started[i], NULL);
    printf("%d threads: %d collect in turn, one spins, one sleeps, one waits on a semaphore\n", threads, collectors);
    printf("%u stops, each with every other thread stopped and restarted: %s\n", stw.stops, yes(stw.stops == stops));
    printf("the world stood still in each stop: %s\n", yes(stw.moved == 0));
    printf("SIGXCPU stayed blocked in the stop handler, also after sigsuspend: %s\n",
           yes(atomic_load(&stw.unblocked) == 0));
    printf("a restart that came before the handler's sigsuspend waited for it, pending: %s\n",
           yes(stw_sum_late(false) > 0 && stw_sum_late(true) == stw_sum_late(false)));
    printf("the stops found threads computing, sleeping and waiting: %s\n",
           yes(stw_found(stw_computing) > 0 && stw_found(stw_sleeping) > 0 && stw_found(stw_waiting) > 0));
    return 0;
}
