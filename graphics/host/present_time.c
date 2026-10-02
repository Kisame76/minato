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
