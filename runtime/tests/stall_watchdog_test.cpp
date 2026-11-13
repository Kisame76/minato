// Minato's stall watchdog (not an Android feature): it watches each app surface (a SurfaceView's surface or the native
// window handed to the app). A surface that shows no first frame within the timeout, or a continuous renderer that
// stops presenting, is reported once, so a black screen names where the threads wait. A present on another window (the
// decor) does not count as the surface's frame -- the case a Unity 6 app hit: its decor drew ("Displayed ... +266ms")
// while its SurfaceView never presented. These drive the pure decision tick() with simulated times; the daemon that
// runs it ~1x/s, the native thread dump and the SIGQUIT are the host's side and run on the device, not here.
#include "stall_watchdog.h"

#include "APKRuntime/apkrt.h"
#include "check.h"

#include <pthread.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <mutex>
#include <new>
#include <string>
#include <thread>

using apkrt::stall::dump_threads_now;
using apkrt::stall::paused;
using apkrt::stall::presented;
using apkrt::stall::reset_for_test;
using apkrt::stall::resumed;
using apkrt::stall::set_max_first_frame_ns;
using apkrt::stall::set_return_timeout_ns;
using apkrt::stall::set_return_work_source;
using apkrt::stall::set_timeout_ns;
using apkrt::stall::set_work_source;
using apkrt::stall::surface_created;
using apkrt::stall::surface_removed;
using apkrt::stall::tick;
using apkrt::stall::window_created;

namespace {
// A scripted work source for the pull model: tests set the cumulative work the host would report for one surface id
// and bump it to simulate growth (or leave it flat to simulate a render loop that reruns the same work). Any other id
// reads 0, as a Vulkan surface or a window with no live EGL context (the decor) would.
std::uint64_t g_work_id = 0;
std::uint64_t g_work = 0;
std::uint64_t test_work_source(std::uint64_t id) { return id == g_work_id ? g_work : 0; }

// A scripted return work source: the cumulative bytes moved toward one surface id; any other id reads 0.
std::uint64_t g_return_id = 0;
std::uint64_t g_return_work = 0;
std::uint64_t test_return_source(std::uint64_t id) { return id == g_return_id ? g_return_work : 0; }

// An app whose render surface 1 showed a frame and then went to the background, as the host does it: the window and its
// surfaces are removed at the background (apkrt_window_remove stops their watch) and the returning app gets new ones
// with new ids (SessionBridge). The first-frame watch is kept out of the way unless a test lowers its timeout.
void shown_then_backgrounded() {
    reset_for_test();
    set_timeout_ns(1'000'000);
    set_return_timeout_ns(100);
    set_return_work_source(test_return_source);
    g_return_id = 0;
    g_return_work = 0;
    resumed(1'000);
    surface_created(1, 1'000);
    presented(1, 1'010);
    paused();
    surface_removed(1);
}

bool is_return_dump(const std::string &reason) { return reason.find("after the app returned") != std::string::npos; }
}  // namespace

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

TEST(a_surface_growing_its_work_toward_its_first_frame_is_not_reported) {
    // Progress-aware (pull): a slow first load that keeps doing new work toward its first frame -- a shader warm-up,
    // asset extraction, the Ren'Py case that spends seconds reading and uploading before its first swap -- must not
    // read as a stall. Each tick pulls the host's cumulative work; while it grows, the deadline slides, so the surface
    // never presents here yet ticks well past the raw creation-to-now timeout stay quiet.
    // RED before progress-awareness: tick() measured only from created_ns and would fire at tick(1'170).
    reset_for_test();
    set_timeout_ns(100);
    g_work_id = 1;
    set_work_source(test_work_source);
    resumed(1'000);
    surface_created(1, 1'000);
    g_work = 100;
    CHECK(tick(1'090).empty());  // pull 100 (grew from 0): 10 ns since growth, within the timeout
    g_work = 200;
    CHECK(tick(1'170).empty());  // pull 200 (grew): 170 ns past creation (old code fires here), fresh growth: quiet
    g_work = 300;
    CHECK(tick(1'300).empty());  // pull 300 (grew): still within the timeout of the last growth
}

TEST(a_surface_that_reruns_work_without_growing_is_still_reported) {
    // The B1 stall: a render loop keeps issuing GL work -- uploads and draws -- but produces no new result and never
    // presents, so the host's work measure (a high-water of live resident bytes) stays flat. The deadline slides only
    // on growth, so the surface is still reported.
    // RED if a flat pull slid the deadline (any-activity model): the deadline would keep resetting and never fire.
    reset_for_test();
    set_timeout_ns(100);
    g_work_id = 1;
    set_work_source(test_work_source);
    resumed(1'000);
    surface_created(1, 1'000);
    g_work = 500;
    CHECK(tick(1'050).empty());  // pull 500 (grew from 0): fresh growth at 1'050, quiet
    // the loop reruns the same work: g_work stays 500, no growth
    CHECK(tick(1'120).empty());  // pull 500 (flat): 70 ns since the last growth (1'050), within the timeout
    const std::string reason = tick(1'200);  // pull 500 (flat): 150 ns since the last growth -- a stall
    CHECK(reason.find("no first frame") != std::string::npos);
}

TEST(a_surface_that_stops_growing_its_work_before_its_first_frame_is_reported) {
    // Growth slides the deadline but does not suspend the watchdog: once new work toward the first frame stops -- a
    // real hang mid-load -- the surface is reported the timeout after the work last grew.
    reset_for_test();
    set_timeout_ns(100);
    g_work_id = 1;
    set_work_source(test_work_source);
    resumed(1'000);
    surface_created(1, 1'000);
    g_work = 100;
    CHECK(tick(1'100).empty());  // pull 100 (grew): fresh growth at 1'100
    g_work = 200;
    CHECK(tick(1'200).empty());  // pull 200 (grew): fresh growth at 1'200
    // work frozen at 200 from here
    CHECK(tick(1'250).empty());   // 50 ns since the last growth: within the timeout
    const std::string reason = tick(1'400);  // 200 ns since the last growth, still no first frame
    CHECK(reason.find("no first frame") != std::string::npos);
}

TEST(a_surface_is_reported_at_the_hard_cap_even_while_growing) {
    // The hard upper bound: even a surface whose work keeps growing is reported once it has gone too long with no first
    // frame, so a load that advances forever or far too slowly cannot dodge the watchdog.
    reset_for_test();
    set_timeout_ns(100);
    set_max_first_frame_ns(500);
    g_work_id = 1;
    set_work_source(test_work_source);
    resumed(1'000);
    surface_created(1, 1'000);
    g_work = 1;
    CHECK(tick(1'100).empty());   // grew; 100 ns past creation (< 500 cap), fresh growth: quiet
    g_work = 2;
    CHECK(tick(1'200).empty());
    g_work = 3;
    CHECK(tick(1'400).empty());   // 400 ns past creation (< 500 cap), fresh growth: quiet
    g_work = 4;
    const std::string reason = tick(1'510);  // grew, but 510 ns past creation (> 500 cap)
    CHECK(reason.find("no first frame") != std::string::npos);  // the hard cap fires despite fresh growth
}

TEST(a_surface_with_no_work_source_for_its_id_is_reported) {
    // The Unity 6 shape under the pull model, and the Vulkan case: the source reports work for another id (the busy
    // decor, or nothing for a Vulkan surface), so surface 1 reads 0, never grows, and is reported from its creation.
    reset_for_test();
    set_timeout_ns(100);
    g_work_id = 2;                // the source grows for window 2 (the decor), reads 0 for surface 1
    g_work = 999;
    set_work_source(test_work_source);
    resumed(1'000);
    surface_created(1, 1'000);    // the SurfaceView surface, no EGL work of its own
    CHECK(tick(1'050).empty());
    const std::string reason = tick(1'200);  // surface 1 pulls 0: 200 ns past its creation
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

TEST(the_stall_report_names_the_thread_its_state_and_the_gate) {
    // A forced onStop stall: a named thread parked at the background commit gate (apkrt_wait_foreground), so the
    // report must name it, its state, and the gate it waits at (the background commit gate and the thread name tags).
    reset_for_test();
    apkrt_set_background(1);
    std::thread render([] {
        pthread_setname_np("test-render");
        apkrt_wait_foreground();  // parks at the background gate; apkrt_background_gate counts it
    });
    for (int i = 0; i < 400; ++i) {  // wait until the gate counts it parked
        std::uint32_t at_gate = 0;
        apkrt_background_gate(&at_gate, nullptr);
        if (at_gate >= 1) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    // Capture the report: dump_threads_now writes it to stderr (::write(STDERR_FILENO, ...)).
    char path[] = "/tmp/stall-report.XXXXXX";
    const int fd = ::mkstemp(path);
    std::fflush(stderr);
    const int saved = ::dup(STDERR_FILENO);
    ::dup2(fd, STDERR_FILENO);
    dump_threads_now("no first frame 10s after the app surface was created");
    ::fflush(stderr);
    ::dup2(saved, STDERR_FILENO);
    ::close(saved);
    ::close(fd);
    std::ifstream in(path);
    const std::string report((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    ::unlink(path);
    apkrt_set_background(0);  // wake the parked thread before the asserts, so a failing assert never leaves it parked
    render.join();
    CHECK(report.find("[test-render]") != std::string::npos);   // the stalled thread, named
    CHECK(report.find("waiting") != std::string::npos);         // its state
    CHECK(report.find("commit gate") != std::string::npos);     // the gate it waits at
}

TEST(a_return_whose_new_surface_works_with_no_frame_is_reported) {
    // The reset race: the returning app's new SurfaceView surface (made after the resume) reloads its scene -- its
    // return work grows -- and no frame comes within the return timeout.
    shown_then_backgrounded();
    resumed(2'000);
    g_return_id = 2;
    surface_created(2, 2'050);  // joins the return's watch with its work now (0)
    g_return_work = 500;
    CHECK(tick(2'100).empty());  // 100 ns after the resume: not past the timeout yet
    CHECK(is_return_dump(tick(2'200)));
}

TEST(a_return_whose_new_window_came_before_the_resume_is_reported) {
    // A NativeActivity's new window is added at onStart, before the resume: it joins when the watch starts.
    shown_then_backgrounded();
    g_return_id = 2;
    surface_created(2, 1'990);
    resumed(2'000);
    g_return_work = 500;
    CHECK(is_return_dump(tick(2'200)));
}

TEST(a_quiet_return_is_never_reported) {
    // No work since the resume: an app idle at a menu, not a stall, however long no frame comes.
    shown_then_backgrounded();
    resumed(2'000);
    g_return_id = 2;
    g_return_work = 700;  // work done before the surface joined is its baseline, not growth
    surface_created(2, 2'050);
    CHECK(tick(2'200).empty());
    CHECK(tick(900'000).empty());
}

TEST(a_frame_in_time_ends_the_return_watch) {
    shown_then_backgrounded();
    resumed(2'000);
    g_return_id = 2;
    surface_created(2, 2'050);
    presented(2, 2'080);  // the app shows again
    g_return_work = 500;
    CHECK(tick(2'200).empty());
}

TEST(a_frame_on_any_surface_ends_the_return_watch) {
    // Surface 2 presents after the return; surface 3 still loads without a frame of its own: no stall.
    shown_then_backgrounded();
    resumed(2'000);
    surface_created(2, 2'050);
    g_return_id = 3;
    surface_created(3, 2'060);
    presented(2, 2'080);
    g_return_work = 500;  // surface 3 keeps working
    CHECK(tick(2'200).empty());
}

TEST(a_second_stalled_return_in_the_same_session_is_not_reported_again) {
    shown_then_backgrounded();
    resumed(2'000);
    g_return_id = 2;
    surface_created(2, 2'050);
    g_return_work = 500;
    CHECK(is_return_dump(tick(2'200)));
    paused();
    surface_removed(2);
    resumed(3'000);
    g_return_id = 4;
    g_return_work = 0;
    surface_created(4, 3'050);
    g_return_work = 900;
    CHECK(tick(3'200).empty());  // the return watch's latch: once per session
}

TEST(a_resume_before_the_first_frame_is_no_return) {
    // The app went to the background still loading (no frame yet): its next surface is the first-frame watch's.
    reset_for_test();
    set_timeout_ns(1'000'000);
    set_return_timeout_ns(100);
    set_return_work_source(test_return_source);
    g_return_id = 2;
    g_return_work = 0;
    resumed(1'000);
    surface_created(1, 1'000);
    paused();
    surface_removed(1);
    resumed(2'000);
    surface_created(2, 2'050);
    g_return_work = 500;
    CHECK(tick(2'200).empty());
}

TEST(a_pause_that_keeps_its_frame_is_no_return) {
    // The menu's pause: onPause and onResume with the same surface, whose frame stays on screen. A quiet app streaming
    // music from a file grows the return work, but it shows its frame: not a stall.
    reset_for_test();
    set_timeout_ns(1'000'000);
    set_return_timeout_ns(100);
    set_return_work_source(test_return_source);
    g_return_id = 1;
    g_return_work = 0;
    resumed(1'000);
    surface_created(1, 1'000);
    presented(1, 1'010);
    paused();
    resumed(2'000);
    g_return_work = 500;
    CHECK(tick(2'200).empty());
}

TEST(a_surface_the_first_frame_watch_reported_is_not_reported_again) {
    // The return's new surface is also in the first-frame watch; when that one reports it first, the return watch
    // does not dump it a second time.
    shown_then_backgrounded();
    set_timeout_ns(50);  // the first-frame watch reports surface 2 50 ns after it was made (no work source)
    resumed(2'000);
    g_return_id = 2;
    surface_created(2, 2'010);
    g_return_work = 500;
    CHECK(tick(2'070).find("no first frame") != std::string::npos);
    CHECK(tick(2'200).empty());
}

TEST(the_return_watch_reports_after_the_first_frame_dump_was_spent) {
    // Its own latch: the first load already dumped once (has_dumped), and a later stalled return is still reported.
    reset_for_test();
    set_timeout_ns(100);
    set_return_timeout_ns(100);
    set_return_work_source(test_return_source);
    g_return_id = 0;
    g_return_work = 0;
    resumed(1'000);
    surface_created(1, 1'000);
    CHECK(tick(1'200).find("no first frame") != std::string::npos);  // the slow first load: the session's dump
    presented(1, 1'300);
    paused();
    surface_removed(1);
    resumed(2'000);
    g_return_id = 2;
    surface_created(2, 2'050);
    g_return_work = 500;
    CHECK(is_return_dump(tick(2'200)));
}

int main() { return apk::testing::run_all(); }
