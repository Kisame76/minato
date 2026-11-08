// c3b-contention-probe: does a load, or a store, to a granule unrelated to an LL/SC cell, between its load- and its
// store-exclusive, keep the sums exact and the loop making progress when 4 or 8 threads contend for the cell?
// (ADR-0020 X7 (c1), after the single-thread c3b-monitor-probe.) The island of an x18 use that lies in a window the
// converter cannot relocate reads TSD slot 11 (or writes it) between the guest's exclusives; that access is of the
// kind measured here. Three modes of one add loop (1, 4 and 8 threads, 2,000,000 passes each):
//   plain  ldxr; add; stxr
//   load   ldxr; ldr [other line]; add; stxr
//   store  ldxr; str [other line]; add; stxr
// Each prints the sum with the sum it must have; the counts of failed store-exclusives go with it, which differ from
// run to run (fixtures/tests.txt marks the program `measures`, so no runner compares its output). The exit status is
// 1 where a sum is wrong or a loop does not end within its bound, which is what the measurement is for.
#include <inttypes.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define PASSES 2000000
#define MAX_THREADS 8
#define RETRY_BOUND 100000000ull  // failed attempts of one pass that count as no progress

enum { PLAIN, LOAD, STORE };

static uint64_t cell __attribute__((aligned(256)));
static uint64_t unrelated[MAX_THREADS][64] __attribute__((aligned(256)));  // one 512-byte block per thread
static int mode;
static uint64_t failures[MAX_THREADS];
static int stuck;

static void *worker(void *argument) {
    const long id = (long)argument;
    uint64_t *other = &unrelated[id][0];
    uint64_t failed = 0;
    for (long pass = 0; pass < PASSES; pass++) {
        uint64_t old, status, tries = 0;
        do {
            if (mode == PLAIN) {
                __asm__ volatile("ldxr %0, [%2]\n\tadd %0, %0, #1\n\tstxr %w1, %0, [%2]"
                                 : "=&r"(old), "=&r"(status) : "r"(&cell) : "memory");
            } else if (mode == LOAD) {
                uint64_t unused;
                __asm__ volatile("ldxr %0, [%3]\n\tldr %2, [%4]\n\tadd %0, %0, #1\n\tstxr %w1, %0, [%3]"
                                 : "=&r"(old), "=&r"(status), "=&r"(unused) : "r"(&cell), "r"(other) : "memory");
            } else {
                __asm__ volatile("ldxr %0, [%3]\n\tstr %0, [%4]\n\tadd %0, %0, #1\n\tstxr %w1, %0, [%3]"
                                 : "=&r"(old), "=&r"(status) : "r"(&cell), "r"(&cell), "r"(other) : "memory");
            }
            if (status != 0 && ++tries > RETRY_BOUND) {
                stuck = 1;
                return NULL;
            }
        } while (status != 0);
        failed += tries;
    }
    failures[id] = failed;
    return NULL;
}

int main(void) {
    static const char *const names[] = {"plain", "load of another line", "store to another line"};
    static const int counts[] = {1, 4, 8};
    int bad = 0;
    for (mode = PLAIN; mode <= STORE; mode++) {
        for (int c = 0; c < 3; c++) {
            const int threads = counts[c];
            pthread_t pool[MAX_THREADS];
            cell = 0;
            for (int i = 0; i < MAX_THREADS; i++) failures[i] = 0;
            for (long i = 0; i < threads; i++) pthread_create(&pool[i], NULL, worker, (void *)i);
            for (int i = 0; i < threads; i++) pthread_join(pool[i], NULL);
            uint64_t failed = 0;
            for (int i = 0; i < threads; i++) failed += failures[i];
            const uint64_t expected = (uint64_t)threads * PASSES;
            printf("%s, %d thread%s: sum %" PRIu64 " expected %" PRIu64 " %s, failed store-exclusives %" PRIu64 "\n",
                   names[mode], threads, threads == 1 ? "" : "s", cell, expected,
                   cell == expected ? "ok" : "WRONG", failed);
            if (cell != expected || stuck) bad = 1;
        }
    }
    printf(bad ? "c3b-contention-probe: a sum is wrong or a loop did not end\n" : "c3b-contention-probe: all sums exact\n");
    return bad;
}
