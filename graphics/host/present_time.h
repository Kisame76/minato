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

/* A layer's pace (ADR-0041 decision 11 c)), zeroed before its first frame: the refresh the last frame Core Animation
   got shows at, 0 before one. */
struct minato_present_pace {
    int64_t taken;
};

/* A frame at its present at `now`, on the grid of the refreshes at `phase` + k x `period` (`period` > 0): 1 when Core
   Animation gets it, 0 when it does not (decision 11 c)). `ruled` is 1 when the rule may withhold the frame, as for
   EGL's presents, whose drawables ANGLE lets go at once, and 0 for Vulkan's, since MoltenVK keeps a dropped drawable
   until it acquires that image again. `asked` is the time its app asked for (0 or less: none), `time` the one Core
   Animation gets (0: none, as minato_present_time gives it), and `waited` how long its drawable took to come, in
   nanoseconds. A ruled frame is not shown when its app asked for a time, an older frame already takes the refresh it
   asked for (the first on the grid at or after the time less 1 ms) and its drawable waited longer than half a period.
   Any other frame goes and takes its refresh: the time's with a time, else the 2nd refresh after `now`, at least the
   one after the older frame's. */
int minato_present_paced_on_grid(struct minato_present_pace *pace, int ruled, int64_t asked, int64_t time,
                                 int64_t waited, int64_t now, int64_t phase, int64_t period);

/* The same on the grid of the runtime's last display-link tick (apkrt_vsync_grid). A frame before the first tick, or on
   a grid without a period, goes and takes nothing. */
int minato_present_paced(struct minato_present_pace *pace, int ruled, int64_t asked, int64_t time, int64_t waited,
                         int64_t now);

/* ADR-0078 decision 4: how long a present at `now` waits under the frame rate limit's `period`, in nanoseconds, the
   window's last present having gone at `last` (0 before one): until `last` + `period` less 1 ms, which leaves an app
   paced at the limit its jitter. 0 once that has passed, without a limit (`period` 0 or less) and before the first
   present. */
int64_t minato_present_limit_wait(int64_t last, int64_t now, int64_t period);

#endif
