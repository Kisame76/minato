// Minato's stall watchdog (not an Android feature): a resumed app that shows no first frame within the timeout, or
// stops presenting after it had, is reported once, so a black screen names where the threads wait. These drive the
// pure decision tick() with simulated times; the daemon that runs it ~1x/s, the native thread dump and the SIGQUIT
// are the host's side and run on the device, not here.
#include "stall_watchdog.h"

#include "check.h"

#include <string>

using apkrt::stall::paused;
using apkrt::stall::presented;
using apkrt::stall::reset_for_test;
using apkrt::stall::resumed;
using apkrt::stall::set_timeout_ns;
using apkrt::stall::tick;

TEST(reports_once_when_a_resumed_app_shows_no_first_frame) {
    reset_for_test();
    set_timeout_ns(100);
    resumed(1'000);
    CHECK(tick(1'050).empty());  // 50 ns in, within the timeout: quiet
    const std::string reason = tick(1'200);  // 200 ns in, no present ever
    CHECK(reason.find("no first frame") != std::string::npos);
    CHECK(tick(5'000).empty());  // once per session: no second report
}

TEST(a_continuous_renderer_that_stops_presenting_is_reported) {
    reset_for_test();
    set_timeout_ns(100);
    resumed(1'000);
    for (std::uint64_t i = 0; i < 30; ++i) presented(1'010 + i);  // ~30 frames in a tight window: a continuous renderer
    CHECK(tick(1'100).empty());  // 61 ns after the last present (1039): within the timeout
    const std::string reason = tick(1'300);  // 261 ns after the last present
    CHECK(reason.find("stopped presenting") != std::string::npos);
}

TEST(an_idle_ui_that_posted_a_few_frames_never_fires_stopped_presenting) {
    reset_for_test();
    set_timeout_ns(100);
    resumed(1'000);
    presented(1'010);
    presented(1'020);
    presented(1'030);  // only a few frames, then idle -- a Java UI that posts nothing while nothing changes
    CHECK(tick(5'000).empty());    // long past the timeout, but not a continuous renderer: never a stall
    CHECK(tick(9'000).empty());    // still quiet: it did not latch the once-per-session dump either
}

TEST(a_paused_app_is_never_reported) {
    reset_for_test();
    set_timeout_ns(100);
    resumed(1'000);
    paused();
    CHECK(tick(9'000).empty());  // paused: no report however long it waits
}

int main() { return apk::testing::run_all(); }
