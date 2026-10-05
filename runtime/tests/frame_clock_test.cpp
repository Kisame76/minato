// Checks the session's frame clock (frame_clock.h, ADR-0036 decision 1) on a clock of the test's own: the ticks it
// posts when it wakes on time, after a post that takes longer than a period, and after a wake later than a period.
#include "frame_clock.h"

#include "check.h"

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace {

using Posts = std::vector<std::pair<std::int64_t, std::int64_t>>;

/// The first `count` ticks and targets of a clock with a period of 10 that starts at 100. Each post takes `post_takes`
/// of the clock's time, and a sleep wakes `late(n)` after its time, n counting the sleeps from 0.
template <typename Late>
Posts posted(std::size_t count, Late late, std::int64_t post_takes = 0) {
    std::int64_t clock = 100;
    int sleeps = 0;
    Posts posts;
    frame_clock::run(
        10, [&] { return clock; }, [&](std::int64_t wait) { clock += wait + late(sleeps++); },
        [&](std::int64_t tick, std::int64_t target) {
            posts.emplace_back(tick, target);
            clock += post_takes;
            return posts.size() < count;
        });
    return posts;
}

TEST(wakes_on_time_post_every_grid_point) {
    const Posts expected{{100, 110}, {110, 120}, {120, 130}};
    CHECK(posted(3, [](int) { return 0; }) == expected);
}

TEST(a_post_longer_than_a_period_skips_the_grid_points_it_took) {
    // Each post takes 15: the clock reads 115 after the first, and the next tick is at 120.
    const Posts expected{{100, 110}, {120, 130}, {140, 150}};
    CHECK(posted(3, [](int) { return 0; }, 15) == expected);
}

TEST(a_wake_later_than_a_period_posts_the_latest_grid_point_at_or_before_it) {
    // The first sleep, until 110, wakes at 135, after 120 and 130 as well: the tick is 130, as a display link reports
    // the latest refresh, not 110.
    const Posts expected{{100, 110}, {130, 140}, {140, 150}};
    CHECK(posted(3, [](int sleep) { return sleep == 0 ? 25 : 0; }) == expected);
    // Exactly a period late, at 120: that is the tick.
    const Posts exactly{{100, 110}, {120, 130}, {130, 140}};
    CHECK(posted(3, [](int sleep) { return sleep == 0 ? 10 : 0; }) == exactly);
}

}  // namespace

int main() {
    return apk::testing::run_all();
}
