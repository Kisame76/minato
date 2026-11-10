/* What graphics-run's test pieces share (window_host.m, metal_hook.m, test_control.m): while
   minato_test_counting is set, the Metal hook counts the command buffers guest threads commit in
   minato_test_guest_commits; while minato_test_asking is set, those of other threads in
   minato_test_asker_commits. minato_test_window is the window window_host.m added, or 0.
   After minato_test_watch_presents, minato_test_presented_at is how the hook saw the first drawable presented
   since: its presentAtTime: time in nanoseconds of the uptime clock (1 for 0), -1 for one presented without a time
   (present), or 0 before one; minato_test_presented_nth the same for the n-th from 0, of the first 16.
   With MINATO_METAL_HOOK=count, minato_test_fence_waits counts every waitForFence:beforeStages: of a render encoder,
   whichever thread encodes it: MoltenVK's barrier fences, which it encodes only with Metal argument buffers, at the
   start of every render encoder (its events and queries wait in blit encoders). */
#ifndef MINATO_TEST_HOST_H
#define MINATO_TEST_HOST_H

#include <stdatomic.h>
#include <stdint.h>

extern atomic_int minato_test_counting, minato_test_guest_commits, minato_test_asking, minato_test_asker_commits;
extern atomic_int minato_test_fence_waits;
extern uint64_t minato_test_window;

void minato_test_watch_presents(void);
int64_t minato_test_presented_at(void);
int64_t minato_test_presented_nth(uint64_t n);

/* With MINATO_WINDOW_BUFFERS=fake, puts window_host.m's fake buffers in the place of graphics/host's, after
   minato_graphics_set_providers set those. */
void minato_test_use_fake_buffers(void);

#endif
