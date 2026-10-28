// c3b-monitor-probe: does a store to an unrelated granule, placed between a load-exclusive and its
// store-exclusive, clear this PE's local monitor? (ADR-0020 C3b, the on-device monitor check.)
//
// Why this matters. Step C's C3b is deferred for a device check. For an x18 use inside an LL/SC
// window that does not close within the 64-word cap, the island would materialise x18 between the
// guest's `ldxr` and `stxr`: a store that spills x18 to TSD slot 11 (when x18 is written in the
// window) or a load of slot 11 (when x18 is read). That memory access lands in a granule unrelated
// to the window's own exclusive cell. The Arm ARM leaves it IMPLEMENTATION-DEFINED whether a store
// to a location not covered by the reservation granule clears the local monitor. If this PE clears
// on such a store, the island's spill makes every `stxr` fail; the guest's retry loop (`cbnz` back
// to the `ldxr`) re-arms, spills again, fails again -- a livelock on real silicon. So this must be
// measured on the A17 (the decision device) with the M4 as a reference, before C3b is accepted.
//
// The experiment. Four variants of one LL/SC window whose ldxr..stxr span is 76 words (> 64, so it
// matches a window past the relocation cap). The variants differ in EXACTLY ONE instruction placed
// between the load- and store-exclusive; everything else -- the 72 padding words, the increment --
// is identical, so interrupt exposure is the same and only the one access varies:
//   clean    nop             negative control: a working LL/SC must succeed.
//   clrex    clrex           positive control: a KNOWN monitor clear; the probe MUST detect it.
//   c3b_store str xT, [tsd]  the real question -- the island's x18 spill (store sub-case).
//   c3b_load  ldr xT, [tsd]  the read sub-case -- a load should never clear the monitor.
// `cell` (the exclusive target) and `tsd` (the unrelated-granule target) sit on separate 16 KiB
// pages, so they are in different reservation granules by any realistic granule size. Each outer
// iteration retries the window up to CAP times (so the probe itself cannot livelock); it counts
// first-try successes, successes after a retry (an asynchronous clear, e.g. a timer interrupt, that
// a retry recovers from), and livelocks (CAP consecutive failures -- the deterministic-clear
// signature). A livelocked iteration writes nothing, so `cell` ends equal to the number of
// successful increments.
//
// Red/green, fixed before the sitting.
//   KEEPS (green for C3b): c3b_store has livelocked == 0 and a first-try rate close to clean's.
//     The unrelated-granule store does not clear the monitor here; the 75 C3b sites convert with the
//     existing island machinery, no ABI change.
//   CLEARS (red for C3b): c3b_store livelocks on (almost) every iteration while clean does not.
//     The store clears the monitor; the island's TSD spill would livelock. Fall back to a TSD-free
//     island (hold x18 in a spare register across the window) or C5's resumable islands.
// The KEEPS/CLEARS verdict is the measurement and goes to stdout (fixtures/tests.txt marks the
// program `measures`, so no runner compares its stdout). The program exits 1 only if its own soundness
// checks fail: the clean control must never livelock and must mostly succeed (a working LL/SC); the
// clrex positive control must livelock on (almost) every iteration (the probe can see a clear);
// and for every variant cell == first_try_ok + retried_ok (the arithmetic held). If the positive
// control does not show a clear, the probe cannot detect one and its c3b verdict is meaningless --
// hence the hard check.
//
// Iterations: 1,000,000 per variant by default (sub-second when healthy; capped when clearing).
// Pass a count as argv[1] for a longer run at the sitting.
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

struct probe_stats {
    uint64_t iterations;      // +0,  filled by C
    uint64_t first_try_ok;    // +8
    uint64_t retried_ok;      // +16
    uint64_t livelocked;      // +24
    uint64_t total_failures;  // +32
    uint64_t cell_final;      // +40
};

// One variant's whole outer+retry loop in a single asm block, so the compiler inserts nothing
// between the load- and store-exclusive. `access` is the one instruction under test. x0..x3 are the
// pinned inputs (unmodified); x4..x16 are clobbered temporaries. x18 is never touched (bionic's
// shadow call stack lives there), and no callee-saved register is used. CAP is 256: an asynchronous
// clear is recovered by the next try, so a window that never recovers within 256 tries is a
// deterministic clear, and the probe moves on instead of spinning forever.
#define PROBE_VARIANT(fn, access)                                                                 \
    __attribute__((noinline)) static void fn(uint64_t iterations, uint64_t *cell, uint64_t *tsd,  \
                                             struct probe_stats *s) {                              \
        register uint64_t r0 __asm__("x0") = iterations;                                          \
        register uint64_t *r1 __asm__("x1") = cell;                                               \
        register uint64_t *r2 __asm__("x2") = tsd;                                                \
        register struct probe_stats *r3 __asm__("x3") = s;                                         \
        __asm__ volatile(                                                                         \
            "       str     xzr, [x1]\n"            /* cell = 0 */                                 \
            "       mov     x9, xzr\n"              /* first_try_ok */                             \
            "       mov     x10, xzr\n"             /* retried_ok */                               \
            "       mov     x11, xzr\n"             /* livelocked */                               \
            "       mov     x12, xzr\n"             /* total_failures */                           \
            "       mov     x13, x0\n"              /* outer counter */                            \
            "       mov     x5, xzr\n"              /* padding accumulator A */                    \
            "       mov     x7, xzr\n"              /* padding accumulator B */                    \
            "       mov     x6, xzr\n"              /* value stored by the store variant */        \
            "1:     cbz     x13, 4f\n"              /* outer loop */                               \
            "       mov     x16, xzr\n"             /* tries this iteration */                     \
            "2:     ldxr    x4, [x1]\n"             /* retry: load-exclusive */                    \
            "       .rept   36\n"                                                                  \
            "       add     x5, x5, #1\n"                                                          \
            "       .endr\n"                                                                       \
            "       " access "\n"                   /* the access under test */                    \
            "       .rept   36\n"                                                                  \
            "       add     x7, x7, #1\n"                                                          \
            "       .endr\n"                                                                       \
            "       add     x4, x4, #1\n"                                                          \
            "       stxr    w8, x4, [x1]\n"         /* store-exclusive */                          \
            "       add     x16, x16, #1\n"                                                        \
            "       cbz     w8, 3f\n"               /* w8 == 0: success */                         \
            "       add     x12, x12, #1\n"         /* total_failures++ */                         \
            "       cmp     x16, #256\n"            /* CAP */                                       \
            "       b.lo    2b\n"                                                                  \
            "       add     x11, x11, #1\n"         /* livelocked++ */                             \
            "       b       5f\n"                                                                  \
            "3:     cmp     x16, #1\n"                                                             \
            "       b.ne    6f\n"                                                                  \
            "       add     x9, x9, #1\n"           /* first_try_ok++ */                           \
            "       b       5f\n"                                                                  \
            "6:     add     x10, x10, #1\n"         /* retried_ok++ */                             \
            "5:     sub     x13, x13, #1\n"                                                        \
            "       b       1b\n"                                                                  \
            "4:     str     x9,  [x3, #8]\n"                                                       \
            "       str     x10, [x3, #16]\n"                                                      \
            "       str     x11, [x3, #24]\n"                                                      \
            "       str     x12, [x3, #32]\n"                                                      \
            "       ldr     x4,  [x1]\n"                                                           \
            "       str     x4,  [x3, #40]\n"                                                      \
            : "+r"(r0), "+r"(r1), "+r"(r2), "+r"(r3)                                               \
            :                                                                                      \
            : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x16", "memory",     \
              "cc");                                                                               \
        (void)r0; (void)r1; (void)r2; (void)r3;                                                    \
    }

PROBE_VARIANT(run_clean, "nop")
PROBE_VARIANT(run_clrex, "clrex")
PROBE_VARIANT(run_store, "str x6, [x2]")
PROBE_VARIANT(run_load, "ldr x6, [x2]")

typedef void (*variant)(uint64_t, uint64_t *, uint64_t *, struct probe_stats *);

struct run {
    const char *name;
    variant fn;
    const char *note;
};

// `cell` and `tsd` on separate 16 KiB pages: different reservation granules by any realistic size.
static _Alignas(16384) uint64_t cell;
static _Alignas(16384) uint64_t tsd;

static const char *verdict(const struct probe_stats *store, uint64_t iterations) {
    if (store->livelocked == 0 && store->first_try_ok >= iterations / 2) return "KEEPS";
    if (store->livelocked >= iterations - iterations / 10) return "CLEARS";
    return "INCONCLUSIVE";
}

int main(int argc, char **argv) {
    uint64_t iterations = 1000000;
    if (argc > 1) {
        char *end = NULL;
        unsigned long long value = strtoull(argv[1], &end, 10);
        if (end == argv[1] || *end != '\0' || value == 0) {
            fprintf(stderr, "usage: %s [iterations > 0]\n", argv[0]);
            return 1;
        }
        iterations = (uint64_t)value;
    }

    const struct run runs[] = {
        {"clean (nop)", run_clean, "negative control: a working LL/SC"},
        {"clrex", run_clrex, "positive control: a known monitor clear"},
        {"c3b store (str to the TSD granule)", run_store, "the island's x18 spill"},
        {"c3b load (ldr from the TSD granule)", run_load, "the read sub-case"},
    };
    enum { count = sizeof runs / sizeof runs[0] };
    struct probe_stats stats[count];

    for (int i = 0; i < count; ++i) {
        stats[i].iterations = iterations;
        runs[i].fn(iterations, &cell, &tsd, &stats[i]);
    }

    printf("c3b-monitor-probe: an unrelated-granule access between a load- and store-exclusive, "
           "%" PRIu64 " iterations, retry cap 256, ldxr..stxr span 76 words\n", iterations);
    printf("%-38s %12s %12s %12s %12s  %s\n", "variant", "first-try", "retried", "livelocked",
           "stxr-fails", "note");
    for (int i = 0; i < count; ++i) {
        printf("%-38s %12" PRIu64 " %12" PRIu64 " %12" PRIu64 " %12" PRIu64 "  %s\n", runs[i].name,
               stats[i].first_try_ok, stats[i].retried_ok, stats[i].livelocked,
               stats[i].total_failures, runs[i].note);
    }

    // Soundness checks: the probe is only trustworthy if the controls behave as the architecture
    // requires. These, not the KEEPS/CLEARS verdict, decide the exit status.
    int failed = 0;
    const struct probe_stats *clean = &stats[0];
    const struct probe_stats *clrex = &stats[1];
    if (clean->livelocked != 0 || clean->first_try_ok < iterations / 2) {
        fprintf(stderr, "clean control did not behave as a working LL/SC (first-try %" PRIu64
                        ", livelocked %" PRIu64 ")\n", clean->first_try_ok, clean->livelocked);
        failed = 1;
    }
    if (clrex->livelocked < iterations - iterations / 10) {
        fprintf(stderr, "clrex positive control did not show a clear (livelocked %" PRIu64
                        " of %" PRIu64 "); the probe cannot detect a monitor clear\n",
                clrex->livelocked, iterations);
        failed = 1;
    }
    for (int i = 0; i < count; ++i) {
        if (stats[i].cell_final != stats[i].first_try_ok + stats[i].retried_ok) {
            fprintf(stderr, "%s: cell %" PRIu64 " != first-try %" PRIu64 " + retried %" PRIu64 "\n",
                    runs[i].name, stats[i].cell_final, stats[i].first_try_ok, stats[i].retried_ok);
            failed = 1;
        }
    }

    printf("C3b verdict (store sub-case): %s\n", verdict(&stats[2], iterations));
    printf("C3b verdict (load sub-case):  %s\n", verdict(&stats[3], iterations));
    return failed;
}
