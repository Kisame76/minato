/* graphics-present-time-test: graphics/host's time for Core Animation (ADR-0041, decision 11), the refresh asked for
   on the refresh grid less half a period. At 60 and 120 Hz, on a grid whose phase is no round number: a time on a
   refresh, 1 ms before and after it (the limits of that refresh), past the limit by 1 ns, 1 ns after a refresh,
   a third of the way between two, before the grid's tick; and on a grid with a negative phase. Then on the grid of
   the runtime's ticks, posted here as the app's display link posts them, with the times that stay as they are. */
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

#include "APKRuntime/apkrt.h"
#include "present_time.h"

static int failures;

static void expect_time(const char *what, int64_t asked, int64_t wanted) {
    const int64_t got = minato_present_time(asked);
    if (got != wanted) {
        printf("present-time: %s: %" PRId64 ", not %" PRId64 "\n", what, got, wanted);
        failures++;
    }
}

static void expect(const char *what, int64_t period, int64_t asked, int64_t phase, int64_t refresh) {
    const int64_t got = minato_present_time_on_grid(asked, phase, period), wanted = refresh - period / 2;
    if (got != wanted) {
        printf("present-time: %s, period %" PRId64 " ns: %" PRId64 ", not %" PRId64 "\n", what, period, got, wanted);
        failures++;
    }
}

int main(void) {
    const int64_t phase = 10000123457, periods[] = {16666667, 8333333};
    for (size_t index = 0; index < sizeof periods / sizeof *periods; index++) {
        const int64_t period = periods[index];
#define REFRESH(k) (phase + (k) * period)
        expect("on a refresh", period, REFRESH(3), phase, REFRESH(3));
        expect("1 ms before a refresh", period, REFRESH(3) - 1000000, phase, REFRESH(3));
        expect("1 ms after a refresh", period, REFRESH(3) + 1000000, phase, REFRESH(3));
        expect("1 ms and 1 ns after a refresh", period, REFRESH(3) + 1000001, phase, REFRESH(4));
        expect("1 ms and 1 ns after the refresh before", period, REFRESH(2) + 1000001, phase, REFRESH(3));
        expect("1 ns after a refresh", period, REFRESH(3) + 1, phase, REFRESH(3));
        expect("a third of the way between two refreshes", period, REFRESH(2) + period / 3, phase, REFRESH(3));
        expect("before the grid's tick", period, REFRESH(-2) + 500000, phase, REFRESH(-2));
#undef REFRESH
    }
    /* The refreshes at -15333333, -7000000, 1333333 and 9666666 ns. */
    expect("a negative phase, after it", 8333333, 5000000, -7000000, 9666666);
    expect("a negative phase, before it", 8333333, -20000000, -7000000, -15333333);

    /* On the grid of the runtime's last tick: none before the first one, nor after a tick without a period (no
       session), then grids of 10 ms from 1 s and from 1.007 s. */
    expect_time("before the first tick", 2000000000, 2000000000);
    apkrt_vsync_post(1500000000, 1500000000);
    expect_time("after a tick without a period", 2000000000, 2000000000);
    apkrt_vsync_post(1000000000, 1010000000);
    expect_time("on a refresh of the tick's grid", 2000000000, 1995000000);
    expect_time("1 ms and 1 ns after a refresh of the tick's grid", 2001000001, 2005000000);
    expect_time("a time that would land before 0", 1000000, 1000000);
    expect_time("Android's NATIVE_WINDOW_TIMESTAMP_AUTO", INT64_MIN, INT64_MIN);
    expect_time("a time within a period of INT64_MAX", INT64_MAX - 5000000, INT64_MAX - 5000000);
    apkrt_vsync_post(1007000000, 1017000000);
    expect_time("on a refresh of the next tick's grid", 2007000000, 2002000000);
    expect_time("0, which would land 2 ms after 0 on that grid", 0, 0);
    if (failures == 0) {
        printf("present-time: 18 times on the grid, 9 from the runtime's ticks\n");
    }
    return failures == 0 ? 0 : 1;
}
