/* What graphics/host hands Core Animation for a present asked for at a time (ADR-0041, decision 11), for the
   functions in front of ANGLE's and MoltenVK's (angle.m, vulkan.m): no time for a refresh at most 2 periods after the
   display link's last tick, which Core Animation shows 2 refreshes after the drawable, sooner than any present with a
   time (decision 11 b)); for a later refresh, that refresh on the display's refresh grid, less half a period. Core
   Animation presents at the first refresh after the time it gets, so the frame lands on that refresh, and never more
   than 1 ms before the time asked for. */
#ifndef MINATO_PRESENT_TIME_H
#define MINATO_PRESENT_TIME_H

#include <stdint.h>

/* On the grid of the refreshes at `phase` + k x `period`, in nanoseconds with `period` > 0: the first refresh at or
   after `asked` less 1 ms, less half the period (rounded down). */
int64_t minato_present_time_on_grid(int64_t asked, int64_t phase, int64_t period);

/* How a present goes on. */
enum minato_present_kind {
    MINATO_PRESENT_WITH_TIME,    /* with the time minato_present_time gives */
    MINATO_PRESENT_WITHOUT_TIME  /* without one: ANGLE gets none, MoltenVK 0 */
};

/* A present asked for at `asked`, in nanoseconds of the uptime clock, on the grid of the runtime's last display-link
   tick (apkrt_vsync_grid): without a time, with *time 0, when the refresh it asks for (the first on the grid at or
   after `asked` less 1 ms) lies at most 2 periods after the tick's; otherwise with *time, that refresh less half a
   period. A time of 0 or less goes with that time as it is: 0 is no time for VK_GOOGLE_display_timing, and Android's
   NATIVE_WINDOW_TIMESTAMP_AUTO is negative. So does any time before the first tick, on a grid without a period,
   within a period of INT64_MAX, or one that would land at 0 or before. */
enum minato_present_kind minato_present_time(int64_t asked, int64_t *time);

#endif
