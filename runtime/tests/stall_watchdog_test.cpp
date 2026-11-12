// Minato's stall watchdog (not an Android feature): it watches each app surface (a SurfaceView's surface or the native
// window handed to the app). A surface that shows no first frame within the timeout, or a continuous renderer that
// stops presenting, is reported once, so a black screen names where the threads wait. A present on another window (the
// decor) does not count as the surface's frame -- the case a Unity 6 app hit: its decor drew ("Displayed ... +266ms")
// while its SurfaceView never presented. These drive the pure decision tick() with simulated times; the daemon that
// runs it ~1x/s, the native thread dump and the SIGQUIT are the host's side and run on the device, not here.
#include "stall_watchdog.h"

#include "check.h"

#include <csignal>
#include <mutex>
#include <new>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

using apkrt::stall::paused;
using apkrt::stall::presented;
using apkrt::stall::reset_for_test;
using apkrt::stall::resumed;
using apkrt::stall::set_timeout_ns;
using apkrt::stall::surface_created;
using apkrt::stall::surface_removed;
using apkrt::stall::tick;
using apkrt::stall::window_created;

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

TEST(a_java_window_that_animated_and_then_stays_still_is_not_reported) {
    // A Java app's window (its decor, drawn by hwui) presents only when something changed: a fade-in gives a burst of
    // frames, then a still page, or a WebView's page, which draws in the host's view, gives none. That is no stall;
    // the ANR watchdog covers a main thread that hangs.
    reset_for_test();
    set_timeout_ns(100);
    resumed(1'000);
    window_created(3, 1'000);
    for (std::uint64_t i = 0; i < 40; ++i) presented(3, 1'010 + i);  // the fade-in: as many frames as a game's
    CHECK(tick(5'000).empty());  // long still afterwards
}

TEST(a_java_window_with_no_first_frame_is_reported) {
    // A Java app whose window never draws its first frame still hangs at its start.
    reset_for_test();
    set_timeout_ns(100);
    resumed(1'000);
    window_created(3, 1'000);
    CHECK(tick(1'050).empty());
    CHECK(tick(1'200).find("no first frame") != std::string::npos);
}

namespace {
/// Runs `body` in a forked child and returns its wait status, so a terminate/abort is contained and observed, not a
/// flaky exit race in the parent. `body` is noexcept: an exception escaping it calls std::terminate -> abort, exactly
/// as a detached std::thread's function does when it throws (the watchdog daemon's lambda) -- so the child models the
/// daemon's fate rather than letting this test's run_all() catch the exception.
int child_status(void (*body)() noexcept) {
    std::fflush(nullptr);  // flush stdio before fork, so an aborting child does not re-emit the parent's buffered lines
    const pid_t pid = ::fork();
    if (pid == 0) {
        body();
        ::_exit(0);  // reached only when body() returned without throwing
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    return status;
}
}  // namespace

TEST(the_watchdog_lock_step_aborts_on_a_destroyed_mutex_but_not_on_a_never_destroyed_one) {
    // The detached watchdog daemon locks `lock` (a namespace-static std::mutex) in tick() about once a second and is
    // never joined. If a host test's process exit destroys `lock` while a tick is in flight, tick()'s lock_guard locks
    // a destroyed std::mutex -> Darwin libc++'s checked pthread mutex returns EINVAL -> std::system_error; escaping the
    // daemon's thread function it reaches std::terminate -> abort (the .ips backtrace of the runtime.program flake).
    // The fix makes `lock` (and `surfaces`) never-destroyed (`std::mutex &lock = *new std::mutex;`), so the daemon
    // always locks a live mutex. Reproduced deterministically with the exact operation tick() runs -- a
    // lock_guard<std::mutex> inside a noexcept body (the daemon's throw-is-terminate contract) -- in a child process.

    // Destroyed (what an owned `std::mutex lock;` becomes at exit): the lock step terminates.
    const int destroyed = child_status([]() noexcept {
        alignas(std::mutex) unsigned char storage[sizeof(std::mutex)];
        auto *m = ::new (storage) std::mutex;
        m->~mutex();                                 // run its destructor, as static destruction would
        const std::lock_guard<std::mutex> hold(*m);  // tick()'s lock step on the now-destroyed mutex: EINVAL -> throw
    });
    CHECK(WIFSIGNALED(destroyed) && WTERMSIG(destroyed) == SIGABRT);

    // Never destroyed (the fix: a leaked heap mutex): the same lock step is clean.
    const int leaked = child_status([]() noexcept {
        auto *m = new std::mutex;                    // never destroyed, like `std::mutex &lock = *new std::mutex;`
        const std::lock_guard<std::mutex> hold(*m);
    });
    CHECK(WIFEXITED(leaked) && WEXITSTATUS(leaked) == 0);
}

int main() { return apk::testing::run_all(); }
