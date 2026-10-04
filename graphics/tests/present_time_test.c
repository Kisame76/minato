/* graphics-present-time-test: graphics/host's time for Core Animation (ADR-0041, decision 11), the refresh asked for
   on the refresh grid less half a period. At 60 and 120 Hz, on a grid whose phase is no round number: a time on a
   refresh, 1 ms before and after it (the limits of that refresh), past the limit by 1 ns, 1 ns after a refresh,
   a third of the way between two, before the grid's tick; and on a grid with a negative phase. Then on the grid of
   the runtime's ticks, posted here as the app's display link posts them, with the times that stay as they are and
   decision 11 b)'s presents without a time: a refresh at most 2 periods after the tick's, at 100 and 120 Hz up to
   its limit and 1 ns beyond, also on the tick's refresh and before it.
   Decision 11 c)'s rule, at 60 and 120 Hz, on streams of 10 frames: on time asking 2 periods ahead, 1 period ahead
   (which only the next refresh but one reaches) and 5 periods back, none dropped; a period behind, with the drawable's
   wait, for 2 frames and then caught up, one dropped; a period behind for good, every second frame dropped, or every
   fourth with a guard of 3; the same without a time, none; one refresh asked for three times, only the one whose
   drawable waited dropped; a frame without a time after one far out, which shows after it, so the next frame asking
   for that refresh is dropped; on time across a switch from 60 to 120 Hz, none. Before the first tick a frame goes
   and takes nothing. */
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "APKRuntime/apkrt.h"
#include "present_time.h"

static int failures;

static void expect_time(const char *what, int64_t asked, int64_t wanted) {
    int64_t got = -1;
    const enum minato_present_kind kind = minato_present_time(asked, &got);
    if (kind != MINATO_PRESENT_WITH_TIME || got != wanted) {
        printf("present-time: %s: %s %" PRId64 ", not with the time %" PRId64 "\n", what,
               kind == MINATO_PRESENT_WITH_TIME ? "with the time" : "without a time, with", got, wanted);
        failures++;
    }
}

static void expect_without(const char *what, int64_t asked) {
    int64_t got = -1;
    const enum minato_present_kind kind = minato_present_time(asked, &got);
    if (kind != MINATO_PRESENT_WITHOUT_TIME || got != 0) {
        printf("present-time: %s: %s %" PRId64 ", not without a time\n", what,
               kind == MINATO_PRESENT_WITH_TIME ? "with the time" : "without a time, with", got);
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

/* Decision 11 c)'s rule on a stream of frames, on the grid of `period` from 0. */
struct frame {
    int64_t present, asked, time, waited;
};
enum { kFrames = 10 };
static const int64_t kMs = 1000000;

/* Each frame's fate in order, G for shown and D for dropped, against `wanted`. */
static void expect_fates(const char *what, int64_t period, int guard, const struct frame *frames, const char *wanted) {
    struct minato_present_pace pace = {0};
    char got[kFrames + 1] = {0};
    for (int index = 0; index < kFrames; index++) {
        const struct frame *frame = &frames[index];
        const int shown = minato_present_paced_on_grid(&pace, frame->asked, frame->time, frame->waited, frame->present,
                                                       guard, 0, period);
        got[index] = shown ? 'G' : 'D';
    }
    if (strcmp(got, wanted) != 0) {
        printf("present-time: %s, period %" PRId64 " ns, guard %d: %s, not %s\n", what, period, guard, got, wanted);
        failures++;
    }
}

/* Frame k's time is (k + 10) periods; it is presented `late` after it, a period late from the first frame of `behind`
   up to `until` (exclusive), asks for `ahead` periods after its frame time, and its drawable waited `waited`, a period
   less 1 ms while it is late. */
static void stream(struct frame *frames, int64_t period, int64_t ahead, int behind, int until, int64_t waited) {
    for (int index = 0; index < kFrames; index++) {
        const int64_t frame_time = (index + 10) * period, late = index >= behind && index < until ? period : 0;
        frames[index] = (struct frame){frame_time + late + 1 * kMs, frame_time + ahead * period, 0,
                                       late != 0 ? period - 1 * kMs : waited};
    }
}

static void expect_rule(int64_t period) {
    struct frame frames[kFrames];
    stream(frames, period, 2, 0, 0, 200000);
    expect_fates("on time, 2 periods ahead", period, 0, frames, "GGGGGGGGGG");
    stream(frames, period, 1, 0, 0, 200000);
    expect_fates("on time, 1 period ahead, which only the next refresh but one reaches", period, 0, frames,
                 "GGGGGGGGGG");
    stream(frames, period, -5, 0, 0, 200000);
    expect_fates("5 periods in the past", period, 0, frames, "GGGGGGGGGG");
    stream(frames, period, 2, 0, 2, 0);
    expect_fates("a period behind for 2 frames, then caught up", period, 0, frames, "GDGGGGGGGG");
    stream(frames, period, 2, 0, kFrames, 0);
    expect_fates("a period behind for good", period, 0, frames, "GDGDGDGDGD");
    expect_fates("a period behind for good, a guard of 3", period, 3, frames, "GDGGGDGGGD");
    for (int index = 0; index < kFrames; index++) {
        frames[index].asked = 0;
    }
    expect_fates("a period behind for good, without a time", period, 0, frames, "GGGGGGGGGG");
    /* One refresh 20 periods out asked for three times, with a time: the second's drawable waited, the third's did
       not; the fourth asks for the refresh after. */
    const int64_t far = 30 * period, time = minato_present_time_on_grid(far, 0, period);
    for (int index = 0; index < kFrames; index++) {
        const int64_t later = index > 2 ? (index - 2) * period : 0;
        frames[index] = (struct frame){(index + 10) * period + 1 * kMs, far + later, time + later,
                                       index == 1 ? period - 1 * kMs : 0};
    }
    expect_fates("one refresh asked for three times, then the next ones", period, 0, frames, "GDGGGGGGGG");
    /* A frame without a time after one 20 periods out shows after it, as Core Animation shows drawables in order, so
       the next frame, which asks for that refresh and whose drawable waited, is dropped. */
    for (int index = 0; index < kFrames; index++) {
        const int64_t asked = index == 1 ? 0 : index == 0 ? far : far + (index - 1) * period;
        frames[index] = (struct frame){(index + 10) * period + 1 * kMs, asked,
                                       asked != 0 ? minato_present_time_on_grid(asked, 0, period) : 0,
                                       index == 2 ? period - 1 * kMs : 0};
    }
    expect_fates("a frame without a time after one far out, then one asking for the refresh after", period, 0, frames,
                 "GGDGGGGGGG");
}

/* Frames on time across a switch from 60 to 120 Hz, on one grid's phase: 5 on the 60 Hz grid, then 5 on the 120 Hz
   one, each asking for 2 of its periods after its frame time, with drawables that come at once. The refresh an older
   frame took on the 60 Hz grid is no reason to drop. */
static void expect_switch(void) {
    const int64_t sixty = 16666667, one_twenty = 8333333;
    struct minato_present_pace pace = {0};
    char got[kFrames + 1] = {0};
    for (int index = 0; index < kFrames; index++) {
        const int64_t period = index < 5 ? sixty : one_twenty;
        const int64_t frame_time = index < 5 ? (index + 10) * sixty : 15 * sixty + (index - 5) * one_twenty;
        const int shown = minato_present_paced_on_grid(&pace, frame_time + 2 * period, 0, 200000, frame_time + 1 * kMs,
                                                       0, 0, period);
        got[index] = shown ? 'G' : 'D';
    }
    if (strcmp(got, "GGGGGGGGGG") != 0) {
        printf("present-time: on time across a switch from 60 to 120 Hz: %s, not GGGGGGGGGG\n", got);
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
       session), then grids of 10 ms from 1 s and from 1.007 s, and of 120 Hz from 1 s. */
    expect_time("before the first tick", 2000000000, 2000000000);
    struct minato_present_pace before = {0};
    if (minato_present_paced(&before, 2000000000, 0, 20000000, 2000000000, 0) != 1 || before.taken != 0) {
        printf("present-time: before the first tick, a frame did not go, or took a refresh\n");
        failures++;
    }
    apkrt_vsync_post(1500000000, 1500000000);
    expect_time("after a tick without a period", 2000000000, 2000000000);
    apkrt_vsync_post(1000000000, 1010000000);
    expect_time("on a refresh of the tick's grid", 2000000000, 1995000000);
    expect_time("1 ms and 1 ns after a refresh of the tick's grid", 2001000001, 2005000000);
    expect_without("on the refresh 2 periods after the tick's", 1020000000);
    expect_without("1 ms after it, its limit", 1021000000);
    expect_time("1 ns beyond, the refresh 3 periods after the tick's", 1021000001, 1025000000);
    expect_without("on the tick's refresh", 1000000000);
    expect_without("before the tick's refresh", 900000000);
    expect_without("a time that would land before 0", 1000000);
    expect_time("Android's NATIVE_WINDOW_TIMESTAMP_AUTO", INT64_MIN, INT64_MIN);
    expect_time("a time within a period of INT64_MAX", INT64_MAX - 5000000, INT64_MAX - 5000000);
    apkrt_vsync_post(1007000000, 1017000000);
    expect_time("on a refresh of the next tick's grid", 2007000000, 2002000000);
    expect_time("0, which would land 2 ms after 0 on that grid", 0, 0);
    apkrt_vsync_post(1000000000, 1008333333);
    expect_without("at 120 Hz, 2 periods and 1 ms after the tick's refresh", 1000000000 + 2 * 8333333 + 1000000);
    expect_time("at 120 Hz, 1 ns beyond", 1000000000 + 2 * 8333333 + 1000001, 1000000000 + 3 * 8333333 - 8333333 / 2);
    expect_rule(16666667);
    expect_rule(8333333);
    expect_switch();
    if (failures == 0) {
        printf("present-time: 18 times on the grid, 16 from the runtime's ticks, 6 of them without a time; decision 11 "
               "c)'s rule on 9 streams of 10 frames at 60 and 120 Hz and one across a switch\n");
    }
    return failures == 0 ? 0 : 1;
}
