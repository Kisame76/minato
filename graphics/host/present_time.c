/* A present's time on the display's refresh grid (ADR-0041, decision 11); see present_time.h. */
#include "present_time.h"

#include "APKRuntime/apkrt.h"

/* How much earlier than the time asked for a frame may show. */
#define EARLY_NS 1000000

int64_t minato_present_time_on_grid(int64_t asked, int64_t phase, int64_t period) {
    /* The periods from the grid's refresh to the first one allowed, rounded up: C's division rounds toward 0, which
       is up for a negative quotient. */
    const int64_t from = asked - EARLY_NS - phase;
    const int64_t periods = from / period + (from % period > 0);
    return phase + periods * period - period / 2;
}

int64_t minato_present_time(int64_t asked) {
    int64_t phase = 0, period = 0;
    if (asked <= 0 || apkrt_vsync_grid(&phase, &period) != 0 || period <= 0 || asked > INT64_MAX - period) {
        return asked;
    }
    const int64_t time = minato_present_time_on_grid(asked, phase, period);
    return time > 0 ? time : asked;
}
