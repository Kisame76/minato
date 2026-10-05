// The session's frame clock (ADR-0036 decision 1, ADR-0041 a)): apk-run's ticks apart from its clock, its sleep and
// its post, so that a test drives them on a clock of its own (frame_clock_test.cpp).
#pragma once

#include <cstdint>

namespace frame_clock {

/// Ticks on a grid of `period` from the clock's first reading, as the app's CADisplayLink: each posts its grid point
/// with the next as its target and sleeps until the grid point after the clock's reading, so a post that takes longer
/// than a period skips refreshes, as a display link does. A wake later than a period posts the latest grid point at
/// or before it, as a display link reports its latest refresh, not the one it slept for (ADR-0041 decision 6). Goes
/// on while `post` answers true.
template <typename Now, typename Sleep, typename Post>
void run(std::int64_t period, Now now, Sleep sleep_for, Post post) {
    std::int64_t tick = now();
    while (post(tick, tick + period)) {
        const std::int64_t at = now();
        tick += ((at - tick) / period + 1) * period;
        sleep_for(tick - at);
        const std::int64_t woke = now();
        if (woke - tick >= period) tick += (woke - tick) / period * period;
    }
}

}  // namespace frame_clock
