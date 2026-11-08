// memory-limit-test: the device check for the memory watermark (ADR-0032 decision 8, apkrt_report_memory_to). It
// allocates and TOUCHES memory in 64 MiB steps, logging each step, until iOS ends the process for using too much
// memory (jetsam). Then last-session.memory (the runtime wrote it 1x/second and on each memory-pressure warning) must
// show a footprint near the limit, a small available, a peak at least the footprint, and warning 1 if iOS warned
// first; with the old code there is no memory file at all.
//
// iPhone ONLY. It is NOT in fixtures/tests.txt, so Mac ctest (tools/run-fixture.cmake) and the AVD
// (tools/android-reference.sh) never build a run for it; tools/device-suite.sh runs it only on a real device, never
// in the Simulator -- only a real iPhone enforces a per-app memory limit and jetsam, so anywhere else this would
// allocate until the host thrashes. A ceiling (kMaxMiB) bounds a stray run so it cannot loop forever; on the iPhone
// jetsam always ends it long before the ceiling.
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

enum {
    kStepMiB = 64,        // one allocation step
    kPageBytes = 16384,   // iOS pages are 16 KiB: touch one byte per page to make the whole step resident
    // A ceiling far above any iPhone memory limit, so jetsam ends it first; it only bounds a stray run.
    kMaxMiB = 16384,
};

// Milliseconds on the monotonic clock, so each step's line carries the time: the last "N MiB resident" line jetsam
// leaves then gives both how far it got and how long it took from the first step.
static long now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

int main(void) {
    long resident_mib = 0;
    const long start_ms = now_ms();
    while (resident_mib < kMaxMiB) {
        const size_t step = (size_t)kStepMiB * 1024 * 1024;
        unsigned char *block = (unsigned char *)malloc(step);
        if (block == NULL) {
            printf("memory-limit-test: malloc of %d MiB failed at %ld MiB resident, %ld ms\n", kStepMiB, resident_mib,
                   now_ms() - start_ms);
            fflush(stdout);
            break;
        }
        for (size_t at = 0; at < step; at += kPageBytes) block[at] = 1;  // touch every page (kept resident on purpose)
        resident_mib += kStepMiB;
        printf("memory-limit-test: %ld MiB resident, %ld ms\n", resident_mib, now_ms() - start_ms);
        fflush(stdout);
    }
    // Reached only if jetsam never came (not on a real iPhone): the ceiling or a failed malloc.
    printf("memory-limit-test: stopped at %ld MiB without being ended, %ld ms\n", resident_mib, now_ms() - start_ms);
    fflush(stdout);
    return 0;
}
