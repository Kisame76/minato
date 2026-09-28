// ADR-0036's choreographer-test on the Mac: a scripted host (app_host.h) that runs choreographer-test
// (fixtures/apps/choreographer-test) through activity-host, with the start of ADR-0028's test 3 and the quit
// after the app's ANativeActivity_finish, and plays the frame clock: a thread posts a tick every period, 60 Hz
// at first, with one tick before the program starts. At the app's KEEP_SCREEN_ON, which it asks for after the
// refresh-rate callback's first call, the clock turns to 120 Hz with a tick at once, and after the app's FINISH,
// when its callback is unregistered, back to 60 Hz, 100 ms before the quit. It passes when the app's
// lines are fixtures/expected/choreographer-test.logcat (tools/android-reference.sh --app choreographer-test),
// the device lines show the first callback's refresh-rate calls at 60 and at 120 Hz, three timelines and the
// second callback's first call at 120 Hz, the requests were
// FRAME_RATE 60 (ANativeWindow_setFrameRate at the app's window), KEEP_SCREEN_ON and FINISH, only android_main's
// vsync channel was open at FINISH, and activity-host ended with status 0.
//   choreographer_test <scratch directory> <fixtures bin> <bionic lib> <guest /system> <fixtures/expected>
#include "app_host.h"
#include "channels.h"

#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>

namespace {

using namespace app_host;
using namespace std::chrono_literals;

constexpr std::int64_t sixty = 16'666'667, hundred_twenty = 8'333'333;  // periods in ns

constexpr const char *device_lines =
    "I/choreographer-test-device: refresh rate: period 16666667\n"
    "I/choreographer-test-device: refresh rate: period 8333333\n"
    "I/choreographer-test-device: timelines: 3, preferred 0\n"
    "I/choreographer-test-device: refresh rate, the second callback: period 8333333\n"
    "I/choreographer-test-device: refresh rate: 2 calls, the second callback 1\n";

// The frame clock. A tick and a change of the period happen under the lock, so no tick of the old period
// follows the change.
std::mutex clock_lock;
std::int64_t clock_period = sixty;

void tick() {
    const std::lock_guard hold(clock_lock);
    const auto time = static_cast<std::int64_t>(::clock_gettime_nsec_np(CLOCK_UPTIME_RAW));
    apkrt_vsync_post(time, time + clock_period);
}

/// At KEEP_SCREEN_ON, 120 Hz from now on.
void turn_clock(int type, std::uint64_t argument) {
    if (type != APKRT_REQUEST_KEEP_SCREEN_ON || argument != 1) return;
    {
        const std::lock_guard hold(clock_lock);
        clock_period = hundred_twenty;
    }
    tick();
}

}  // namespace

int main(int argc, char **argv) {
    if (argc != 6) {
        std::fprintf(stderr, "usage: choreographer_test <scratch> <fixtures bin> <bionic lib> <guest system> <expected>\n");
        return 2;
    }
    const Paths paths{argv[1], argv[2], argv[3], argv[4], argv[5]};
    App app{.name = "choreographer-test", .package = "io.github.kisame76.minato.test.choreographer"};
    if (!prepare(app, paths, "choreographer")) return 0;

    Capture capture;
    if (!capture.start()) {
        fail("cannot capture stderr");
        return 1;
    }
    app_host::on_request = turn_clock;
    begin({3.0, width, height, 1, 1, 120, "de", "DE"});
    tick();
    std::atomic<bool> ticking{true};
    std::thread clock([&ticking] {
        while (ticking.load()) {
            std::int64_t period;
            {
                const std::lock_guard hold(clock_lock);
                period = clock_period;
            }
            std::this_thread::sleep_for(std::chrono::nanoseconds(period));
            tick();
        }
    });
    if (!start(app, app.guest + "/base.apk", "choreographer-test", "ANativeActivity_onCreate")) _exit(1);

    // The start, then active: the window's gate opens, and the app begins once it has the focus.
    const std::uint64_t window = apkrt_window_add(nullptr, width, height);
    post(APKRT_EVENT_CREATE);
    post(APKRT_EVENT_START);
    post(APKRT_EVENT_RESUME);
    post(APKRT_EVENT_INPUT_QUEUE_CREATED, 0);
    answered(show_window(window, true), "the start");
    apkrt_window_set_gate(window, 1);

    if (!wait_for([] { return !seen.requests.empty() && seen.requests.back() == "FINISH"; }, 20s)) {
        fail("no FINISH request");
    }
    // The thread that had a choreographer of its own has ended, and its channel with it: only android_main's is open.
    if (const auto open = apkrt::channels::vsync_count(); open != 1) {
        fail(std::to_string(open) + " vsync channels are open, not 1");
    }
    // The app unregistered its refresh-rate callback before it finished: back at 60 Hz, no call comes.
    {
        const std::lock_guard hold(clock_lock);
        clock_period = sixty;
    }
    tick();
    std::this_thread::sleep_for(100ms);
    quit(window);
    if (capture.drain().find("I/choreographer-test: android_main returns\n") == std::string::npos) {
        fail("DESTROY was answered before android_main returned");
    }
    if (!wait_for([] { return seen.ended; }, 10s)) fail("the program did not end after onDestroy");
    ticking.store(false);
    clock.join();

    const std::string log = capture.drain();
    std::lock_guard hold(seen.lock);
    if (seen.status != 0) fail("activity-host ended with status " + std::to_string(seen.status));
    std::string requests;
    for (const auto &request : seen.requests) requests += "[" + request + "]";
    if (requests != "[FRAME_RATE 60][KEEP_SCREEN_ON 1][FINISH]") fail("the requests were " + requests);
    const std::string app_lines = lines_with(log, "I/choreographer-test: ");
    if (app_lines != read_text(paths.expected / "choreographer-test.logcat")) {
        fail("the app's lines differ from Android 15's:\n" + app_lines);
    }
    const std::string device = lines_with(log, "I/choreographer-test-device: ");
    if (device != device_lines) fail("the device lines differ:\n" + device);
    ::dprintf(report, failures == 0 ? "PASS: choreographer-test\n" : "%d failures\n", failures);
    _exit(failures == 0 ? 0 : 1);
}
