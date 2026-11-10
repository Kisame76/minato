/* A present's time on the display's refresh grid (ADR-0041, decision 11); see present_time.h. */
#include "present_time.h"

#include "APKRuntime/apkrt.h"

/* How much earlier than the time asked for a frame may show. */
#define EARLY_NS 1000000
/* The refreshes after the tick's that a present without a time reaches (decision 11 b)). */
#define WITHOUT_TIME_PERIODS 2

/* The first refresh at or after `asked` less 1 ms on the grid of `phase` + k x `period`. */
static int64_t refresh_asked(int64_t asked, int64_t phase, int64_t period) {
    /* The periods from the grid's refresh to the first one allowed, rounded up: C's division rounds toward 0, which
       is up for a negative quotient. */
    const int64_t from = asked - EARLY_NS - phase;
    const int64_t periods = from / period + (from % period > 0);
    return phase + periods * period;
}

int64_t minato_present_time_on_grid(int64_t asked, int64_t phase, int64_t period) {
    return refresh_asked(asked, phase, period) - period / 2;
}

enum minato_present_kind minato_present_time(int64_t asked, int64_t *time) {
    int64_t phase = 0, period = 0;
    *time = asked;
    if (asked <= 0 || apkrt_vsync_grid(&phase, &period) != 0 || period <= 0 || asked > INT64_MAX - period) {
        return MINATO_PRESENT_WITH_TIME;
    }
    const int64_t refresh = refresh_asked(asked, phase, period);
    if (refresh <= phase + WITHOUT_TIME_PERIODS * period) {
        *time = 0;
        return MINATO_PRESENT_WITHOUT_TIME;
    }
    if (refresh - period / 2 > 0) {
        *time = refresh - period / 2;
    }
    return MINATO_PRESENT_WITH_TIME;
}

/* The first refresh at or after `at` on the grid of `phase` + k x `period`. */
static int64_t refresh_from(int64_t at, int64_t phase, int64_t period) {
    const int64_t from = at - phase;
    return phase + (from / period + (from % period > 0)) * period;
}

int minato_present_paced_on_grid(struct minato_present_pace *pace, int ruled, int64_t asked, int64_t time,
                                 int64_t waited, int64_t now, int64_t phase, int64_t period) {
    const int timed_ask = asked > 0 && asked <= INT64_MAX - period;
    if (ruled && timed_ask && pace->taken != 0 && waited > period / 2 &&
        refresh_asked(asked, phase, period) <= pace->taken) {
        return 0;
    }
    /* A present with a time shows on its time's refresh, one without on the 2nd refresh after now (decision 11 b)),
       and Core Animation shows them in order. */
    int64_t shows = time > 0 && time <= INT64_MAX - period ? refresh_from(time, phase, period)
                                                           : refresh_from(now + 1, phase, period) + period;
    if (pace->taken != 0 && shows < pace->taken + period) {
        shows = pace->taken + period;
    }
    pace->taken = shows;
    return 1;
}

int minato_present_paced(struct minato_present_pace *pace, int ruled, int64_t asked, int64_t time, int64_t waited,
                         int64_t now) {
    int64_t phase = 0, period = 0;
    if (apkrt_vsync_grid(&phase, &period) != 0 || period <= 0) {
        return 1;
    }
    return minato_present_paced_on_grid(pace, ruled, asked, time, waited, now, phase, period);
}

int64_t minato_present_limit_wait(int64_t last, int64_t now, int64_t period) {
    const int64_t slack = 1000000;
    if (period <= slack || last <= 0 || last > INT64_MAX - period) {
        return 0;
    }
    const int64_t until = last + period - slack;
    return now < until ? until - now : 0;
}

int64_t minato_present_limit_next(int64_t last, int64_t presented, int64_t period) {
    if (period <= 0 || last <= 0 || last > INT64_MAX - 2 * period) {
        return presented;
    }
    const int64_t grid = last + period;
    return presented < grid + period ? grid : presented;
}
