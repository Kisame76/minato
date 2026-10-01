// ADR-0038's test 3 on the Mac (step c): the scripted host (app_host.h) turns rotation-test (fixtures/apps/rotation-test)
// as the App does (decision 1.4). Once the app logs "ready", it posts CONFIGURATION_CHANGED, gives the same window its
// new size with apkrt_window_set_size, and posts NATIVE_WINDOW_RESIZED, CONTENT_RECT_CHANGED and
// NATIVE_WINDOW_REDRAW_NEEDED: to landscape, and once the app has seen that window, back to portrait, as
// tools/android-reference.sh turns the emulator. Then comes the quit that follows the app's ANativeActivity_finish. It
// passes when the app's lines are fixtures/expected/rotation-test.logcat, its device lines show the window's sizes and
// the configuration's start values throughout (Android 15 keeps them through a turn the activity handles), the only
// request was FINISH, and activity-host ended with status 0 once onDestroy was answered.
//   rotation_test <scratch directory> <fixtures bin> <bionic lib> <guest /system> <fixtures/expected>
#include "app_host.h"

#include <unistd.h>

#include <cstdio>
#include <string>
#include <thread>

namespace {

using namespace app_host;
using namespace std::chrono_literals;

// The configuration's start values (decision 9 of ADR-0028), which the turns leave as they are, and the window's sizes.
constexpr const char *device_lines =
    "I/rotation-test-device: start: orientation port, screen 430 x 932 dp\n"
    "I/rotation-test-device: init: window 1290 x 2796\n"
    "I/rotation-test-device: resized: window 1290 x 2796\n"
    "I/rotation-test-device: content rect: 6, 120, 1284, 2760\n"
    "I/rotation-test-device: redraw: orientation port, screen 430 x 932 dp\n"
    "I/rotation-test-device: changed: orientation port\n"
    "I/rotation-test-device: resized: window 2796 x 1290\n"
    "I/rotation-test-device: content rect: 177, 0, 2619, 1227\n"
    "I/rotation-test-device: redraw: orientation port, screen 430 x 932 dp\n"
    "I/rotation-test-device: changed: orientation port\n"
    "I/rotation-test-device: resized: window 1290 x 2796\n"
    "I/rotation-test-device: content rect: 6, 120, 1284, 2760\n"
    "I/rotation-test-device: redraw: orientation port, screen 430 x 932 dp\n"
    "I/rotation-test-device: before the finish: window 1290 x 2796\n";

/// A turn as the App plays it: the configuration changes, the same window takes its new size, then the window's
/// events, with the content's rectangle `left`, `top`, `right`, `bottom`. Waits for the answer to the redraw.
void turn(std::uint64_t window, std::int32_t new_width, std::int32_t new_height, std::int32_t left, std::int32_t top,
          std::int32_t right, std::int32_t bottom) {
    post(APKRT_EVENT_CONFIGURATION_CHANGED);
    if (apkrt_window_set_size(window, new_width, new_height) != 0) fail("apkrt_window_set_size refused a turn");
    post(APKRT_EVENT_NATIVE_WINDOW_RESIZED, window, static_cast<std::uint64_t>(new_width),
         static_cast<std::uint64_t>(new_height));
    post(APKRT_EVENT_CONTENT_RECT_CHANGED, pair(left, top), pair(right, bottom));
    answered(post(APKRT_EVENT_NATIVE_WINDOW_REDRAW_NEEDED, window), "a turn");
}

/// Waits up to ten seconds until the guest's log holds `line`.
bool logged(Capture &capture, const std::string &line) {
    for (int round = 0; round < 1000; ++round) {
        if (capture.drain().find(line) != std::string::npos) return true;
        std::this_thread::sleep_for(10ms);
    }
    return false;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc != 6) {
        std::fprintf(stderr, "usage: rotation_test <scratch> <fixtures bin> <bionic lib> <guest system> <expected>\n");
        return 2;
    }
    const Paths paths{argv[1], argv[2], argv[3], argv[4], argv[5]};
    App app{.name = "rotation-test", .package = "io.github.kisame76.minato.test.rotation"};
    if (!prepare(app, paths, "rotation")) return 0;

    Capture capture;
    if (!capture.start()) {
        fail("cannot capture stderr");
        return 1;
    }
    begin({3.0, width, height, 1, 1, 120, "en", "US"});
    if (!start(app, app.guest + "/base.apk", "rotation-test", "ANativeActivity_onCreate")) _exit(1);

    // The start in portrait, then active: the window's gate opens.
    const std::uint64_t window = apkrt_window_add(nullptr, width, height);
    post(APKRT_EVENT_CREATE);
    post(APKRT_EVENT_START);
    post(APKRT_EVENT_RESUME);
    post(APKRT_EVENT_INPUT_QUEUE_CREATED, 0);
    answered(show_window(window, true), "the start");
    apkrt_window_set_gate(window, 1);

    // To landscape once the app is ready, and back to portrait once it has seen its window in landscape.
    if (!logged(capture, "I/rotation-test: ready\n")) fail("the app did not get ready");
    turn(window, height, width, 177, 0, height - 177, width - 63);
    if (!logged(capture, "I/rotation-test: resized: window landscape\n")) fail("the app did not see the landscape window");
    turn(window, width, height, 6, 120, width - 6, height - 36);

    // The app finishes itself a second after the second change of its configuration; then the quit sequence.
    if (!wait_for([] { return !seen.requests.empty() && seen.requests.back() == "FINISH"; }, 10s)) {
        fail("no FINISH request");
    }
    quit(window);
    if (!wait_for([] { return seen.ended; }, 10s)) fail("the program did not end after onDestroy");

    const std::string log = capture.drain();
    std::lock_guard hold(seen.lock);
    if (seen.status != 0) fail("activity-host ended with status " + std::to_string(seen.status));
    std::string requests;
    for (const auto &request : seen.requests) requests += "[" + request + "]";
    if (requests != "[FINISH]") fail("the requests were " + requests);
    const std::string app_lines = lines_with(log, "I/rotation-test: ");
    if (app_lines != read_text(paths.expected / "rotation-test.logcat")) {
        fail("the app's lines differ from Android 15's:\n" + app_lines);
    }
    const std::string device = lines_with(log, "I/rotation-test-device: ");
    if (device != device_lines) fail("the device lines differ:\n" + device);
    ::dprintf(report, failures == 0 ? "PASS: rotation-test\n" : "%d failures\n", failures);
    _exit(failures == 0 ? 0 : 1);
}
