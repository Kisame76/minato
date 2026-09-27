/* What graphics-run's test pieces share (window_host.m, metal_hook.m): while minato_test_counting is set,
   the Metal hook counts the command buffers guest threads commit in minato_test_guest_commits; while
   minato_test_asking is set, those of other threads in minato_test_asker_commits. */
#ifndef MINATO_TEST_HOST_H
#define MINATO_TEST_HOST_H

#include <stdatomic.h>

extern atomic_int minato_test_counting, minato_test_guest_commits, minato_test_asking, minato_test_asker_commits;

#endif
