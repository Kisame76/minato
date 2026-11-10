/* graphics-app-test's side of the Metal hook (test_host.h): the counters, which window_host.m defines in
   graphics-run, and the two calls app_test.mm makes from C++ around the background. */
#include "test_host.h"

atomic_int minato_test_counting, minato_test_guest_commits, minato_test_asking, minato_test_asker_commits;
uint64_t minato_test_window;

/* From now on the hook counts the command buffers guest threads commit, from 0. */
void minato_app_test_count_commits(void) {
    atomic_store(&minato_test_guest_commits, 0);
    atomic_store(&minato_test_counting, 1);
}

/* Stops the count: the command buffers counted. */
int minato_app_test_commits(void) {
    atomic_store(&minato_test_counting, 0);
    return atomic_load(&minato_test_guest_commits);
}

/* MoltenVK's barrier fence waits the hook counted (test_host.h). */
int minato_app_test_fence_waits(void) {
    return atomic_load(&minato_test_fence_waits);
}
