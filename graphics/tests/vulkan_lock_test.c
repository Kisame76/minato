/* graphics.vulkan_lock: "scheduled" (vulkan.m), which the app asks on its main thread at the background, takes the lock
   of the kept devices; a guest thread that holds that lock while it blocks must not keep the main thread past the
   deadline "scheduled" is given: the wait ends with 0, "not scheduled in time", and the background goes on. */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include "minato_graphics.h"
#include "vulkan_host.h"

#define HOLD 1500000000ull
#define DEADLINE 200000000ull
/* The answer comes within the deadline and a little more, not after the hold. */
#define LIMIT 600000000ull

static void *hold(void *unused) {
    (void)unused;
    minato_graphics_vulkan_hold_devices_for_testing(HOLD);
    return NULL;
}

static uint64_t now(void) {
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

int main(void) {
    int failed = 0;
    pthread_t holder;
    pthread_create(&holder, NULL, hold, NULL);
    usleep(100000);  // the holder has the lock
    const uint64_t start = now();
    const int answer = minato_graphics_vulkan_scheduled(start + DEADLINE);
    const uint64_t took = now() - start;
    fprintf(stderr, "vulkan lock: \"scheduled\" answered %d after %llu ms while the lock was held\n", answer,
            (unsigned long long)(took / 1000000));
    if (took > LIMIT) {
        fprintf(stderr, "FAILED  \"scheduled\" waited for the lock past its deadline\n");
        failed = 1;
    }
    if (answer != 0) {
        fprintf(stderr, "FAILED  \"scheduled\" said it was in time without having looked at the queues\n");
        failed = 1;
    }
    pthread_join(holder, NULL);
    // With the lock free again, and no device kept, it is scheduled at once.
    const uint64_t second = now();
    const int free_answer = minato_graphics_vulkan_scheduled(second + DEADLINE);
    if (free_answer != 1 || now() - second > LIMIT) {
        fprintf(stderr, "FAILED  with the lock free \"scheduled\" answered %d\n", free_answer);
        failed = 1;
    }
    if (!failed) {
        fprintf(stderr, "graphics.vulkan_lock: passed\n");
    }
    return failed;
}
