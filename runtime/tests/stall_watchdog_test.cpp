// Minato's stall watchdog (not an Android feature): it watches each app surface (a SurfaceView's surface or the native
// window handed to the app). A surface that shows no first frame within the timeout, or a continuous renderer that
// stops presenting, is reported once, so a black screen names where the threads wait. A present on another window (the
// decor) does not count as the surface's frame -- the case a Unity 6 app hit: its decor drew ("Displayed ... +266ms")
// while its SurfaceView never presented. These drive the pure decision tick() with simulated times; the daemon that
// runs it ~1x/s, the native thread dump and the SIGQUIT are the host's side and run on the device, not here.
#include "stall_watchdog.h"

#include "check.h"

#include <string>

using apkrt::stall::paused;
using apkrt::stall::presented;
using apkrt::stall::reset_for_test;
using apkrt::stall::resumed;
using apkrt::stall::set_timeout_ns;
using apkrt::stall::surface_created;
using apkrt::stall::surface_removed;
using apkrt::stall::tick;

TEST(reports_once_when_an_app_surface_shows_no_first_frame) {
    reset_for_test();
    set_timeout_ns(100);
    resumed(1'000);
    surface_created(1, 1'000);  // the app's render surface, created but not yet drawn to
    CHECK(tick(1'050).empty());  // 50 ns in, within the timeout: quiet
    const std::string reason = tick(1'200);  // 200 ns in, no present on the surface ever
    CHECK(reason.find("no first frame") != std::string::npos);
    CHECK(tick(5'000).empty());  // once per session: no second report
}

TEST(a_surface_with_no_frame_is_reported_even_after_another_window_presented) {
    // The Unity 6 case: the decor window draws a frame, but the app's SurfaceView surface never presents. A present on
    // the decor (a window that is not the app surface) must not satisfy the surface's first frame.
    reset_for_test();
    set_timeout_ns(100);
    resumed(1'000);
    surface_created(1, 1'000);  // the SurfaceView surface
    presented(2, 1'010);        // window 2 is the decor, not an app surface: its frame does not count for surface 1
    CHECK(tick(1'050).empty());
    const std::string reason = tick(1'200);  // the surface still has no frame of its own
    CHECK(reason.find("no first frame") != std::string::npos);
}

TEST(a_native_activity_window_with_no_first_frame_is_reported) {
    // A NativeActivity app (Godot, Unreal, other native engines) has no SurfaceView: its app surface IS the session
    // window, which the host marks in addWindow. One window, marked as the surface, that never presents must be
    // reported the same as a SurfaceView that never presents -- a native engine that hangs before its first frame.
    reset_for_test();
    set_timeout_ns(100);
    resumed(1'000);
    surface_created(7, 1'000);  // the session window, handed to the native engine as its render surface
    CHECK(tick(1'050).empty());
    const std::string reason = tick(1'200);  // no present on it within the timeout
    CHECK(reason.find("no first frame") != std::string::npos);
}

TEST(an_app_with_no_app_surface_is_never_reported) {
    // A plain Java UI with no SurfaceView: nothing is an app surface, so a present on its decor arms nothing and the
    // watchdog never fires, however long it idles.
    reset_for_test();
    set_timeout_ns(100);
    resumed(1'000);
    presented(2, 1'010);  // the decor only
    CHECK(tick(9'000).empty());
}

TEST(a_continuous_renderer_that_stops_presenting_is_reported) {
    reset_for_test();
    set_timeout_ns(100);
    resumed(1'000);
    surface_created(1, 1'000);
    // ~30 frames in a tight window: a continuous renderer
    for (std::uint64_t i = 0; i < 30; ++i) presented(1, 1'010 + i);
    CHECK(tick(1'100).empty());  // 61 ns after the last present (1039): within the timeout
    const std::string reason = tick(1'300);  // 261 ns after the last present
    CHECK(reason.find("stopped presenting") != std::string::npos);
}

TEST(a_surface_that_posted_a_few_frames_never_fires_stopped_presenting) {
    reset_for_test();
    set_timeout_ns(100);
    resumed(1'000);
    surface_created(1, 1'000);
    presented(1, 1'010);
    presented(1, 1'020);
    presented(1, 1'030);  // only a few frames, then idle -- not a continuous renderer
    CHECK(tick(5'000).empty());    // long past the timeout, but not continuous: never a stall
    CHECK(tick(9'000).empty());    // still quiet: it did not latch the once-per-session dump either
}

TEST(a_surface_removed_before_it_presents_is_not_reported) {
    // surfaceDestroyed before the first frame (a quick teardown) must not fire: the surface is gone, not stalled.
    reset_for_test();
    set_timeout_ns(100);
    resumed(1'000);
    surface_created(1, 1'000);
    surface_removed(1);
    CHECK(tick(9'000).empty());
}

TEST(a_paused_app_is_never_reported) {
    reset_for_test();
    set_timeout_ns(100);
    resumed(1'000);
    surface_created(1, 1'000);
    paused();
    CHECK(tick(9'000).empty());  // paused: no report however long it waits
}

int main() { return apk::testing::run_all(); }
