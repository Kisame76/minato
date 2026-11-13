// runtime.activity_paused: the behavioural test of libc.so's minato_activity_paused() (the query the SDL swap override
// asks). A scripted host (app_host.h) runs activity-paused-test through activity-host and plays ADR-0032's lifecycle:
// the start, a background and a foreground, then the quit after the app's ANativeActivity_finish. The app logs the
// query at each callback; it must read 0 while resumed and 1 from onPause/onStop, which proves the production path --
// libc exports the query, the loader pointed its flag at the host's toggled int (preload_sdl_swap, with libminato-sdl
// universally preloaded), and apkrt_session_post toggled it. Red if the libc export is dropped: the weak import is
// null and the app logs -1.
//   activity_paused_test <scratch directory> <fixtures bin> <bionic lib> <guest /system> <fixtures/expected>
#include "app_host.h"

#include <unistd.h>

#include <cstdio>
#include <mutex>
#include <string>

namespace {
using namespace app_host;
using namespace std::chrono_literals;
}  // namespace

int main(int argc, char **argv) {
    if (argc != 6) {
        std::fprintf(stderr, "usage: activity_paused_test <scratch> <fixtures> <bionic> <system> <expected>\n");
        return 2;
    }
    const Paths paths{argv[1], argv[2], argv[3], argv[4], argv[5]};
    App app{.name = "activity-paused-test", .package = "io.github.kisame76.minato.test.activitypaused"};
    if (!prepare(app, paths, "activity-paused")) return 0;

    Capture capture;
    if (!capture.start()) {
        fail("cannot capture stderr");
        return 1;
    }
    begin({3.0, width, height, 1, 1, 60, "de", "DE"});
    if (!start(app, app.guest + "/base.apk", "activity-paused-test", "ANativeActivity_onCreate")) _exit(1);

    // The start, then resumed: the window's gate opens once the app has the focus.
    const std::uint64_t first = apkrt_window_add(nullptr, width, height);
    post(APKRT_EVENT_CREATE);
    post(APKRT_EVENT_START);
    post(APKRT_EVENT_RESUME);
    post(APKRT_EVENT_INPUT_QUEUE_CREATED, 0);
    answered(show_window(first, true), "the start");
    apkrt_window_set_gate(first, 1);

    // The background (decision 6): the window goes before onPause, then onPause and onStop (paused reads 1).
    apkrt_set_background(1);
    apkrt_window_remove(first);
    post(APKRT_EVENT_PAUSE);
    post(APKRT_EVENT_WINDOW_FOCUS_CHANGED, 0);
    post(APKRT_EVENT_NATIVE_WINDOW_DESTROYED, first);
    post(APKRT_EVENT_STOP);
    answered(post(APKRT_EVENT_SAVE_INSTANCE_STATE), "the background");

    // The return: a new window, START and RESUME (paused reads 0 again); the app then finishes itself.
    apkrt_set_background(0);
    const std::uint64_t second = apkrt_window_add(nullptr, width, height);
    post(APKRT_EVENT_START);
    post(APKRT_EVENT_RESUME);
    answered(show_window(second, false), "the return");
    apkrt_window_set_gate(second, 1);

    if (!wait_for([] { return !seen.requests.empty() && seen.requests.back() == "FINISH"; }, 20s)) {
        fail("no FINISH request after the return");
    }
    quit(second);
    if (!wait_for([] { return seen.ended; }, 10s)) fail("the program did not end after onDestroy");

    const std::string log = capture.drain();
    {
        const std::lock_guard hold(seen.lock);
        if (seen.status != 0) fail("activity-host ended with status " + std::to_string(seen.status));
    }
    const std::string app_lines = lines_with(log, "I/activity-paused-test: ");
    if (app_lines != read_text(paths.expected / "activity-paused-test.logcat")) {
        fail("the app's paused values differ from the expected sequence:\n" + app_lines);
    }
    ::dprintf(report, failures == 0 ? "PASS: activity-paused-test\n" : "%d failures\n", failures);
    _exit(failures == 0 ? 0 : 1);
}
