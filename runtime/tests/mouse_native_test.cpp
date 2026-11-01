// ADR-0068 decision 5, the mouse step's mouse-native-test on the Mac: a scripted host (app_host.h) that runs
// mouse-native-test (fixtures/apps/mouse-native-test) through activity-host as gamepad_native_test.cpp runs its app,
// and plays the mouse that apk-run's mouse: steps and the App's GCMouse and pointer side play: the device record of a
// mouse, then its moves, buttons and wheels as apkrt_input_post_mouse's reports (tools/mouse-check.sh's list). It
// passes when the app's lines are fixtures/expected/mouse-native-test.logcat (Android 15's, from `uinput`,
// tools/android-reference.sh --app mouse-native-test), the only request was FINISH, and activity-host ended with
// status 0.
//   mouse_native_test <scratch directory> <fixtures bin> <bionic lib> <guest system> <fixtures/expected>
#include "app_host.h"

#include <time.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

namespace {

using namespace app_host;
using namespace std::chrono_literals;

constexpr std::int32_t mouse_device = 1002;
constexpr std::int32_t mouse_source = 0x2002;

std::int64_t now() {
    return static_cast<std::int64_t>(::clock_gettime_nsec_np(CLOCK_UPTIME_RAW));
}

void post_report(std::uint64_t channel, std::int32_t action, std::int32_t buttons,
                 std::initializer_list<apkrt_axis_value> axes) {
    apkrt_axes_event mouse{};
    mouse.action = action;
    mouse.event_time = now();
    mouse.device_id = mouse_device;
    mouse.source = mouse_source;
    mouse.tool_type = 3;
    mouse.button_state = buttons;
    for (const auto &axis : axes) mouse.axes[mouse.axis_count++] = axis;
    if (apkrt_input_post_mouse(channel, &mouse) != 1) fail("the channel refused a mouse report");
}

void move(std::uint64_t channel, float x, float y, float dx, float dy) {
    post_report(channel, 7, 0, {{0, x}, {1, y}, {27, dx}, {28, dy}});
}
void press(std::uint64_t channel, std::int32_t buttons) { post_report(channel, 11, buttons, {}); }
void release(std::uint64_t channel, std::int32_t buttons) { post_report(channel, 12, buttons, {}); }
void scroll(std::uint64_t channel, float vertical, float horizontal) {
    post_report(channel, 8, 0, {{9, vertical}, {10, horizontal}});
}

}  // namespace

int main(int argc, char **argv) {
    if (argc != 6) {
        std::fprintf(stderr,
                     "usage: mouse_native_test <scratch> <fixtures bin> <bionic lib> <guest system> <expected>\n");
        return 2;
    }
    const Paths paths{argv[1], argv[2], argv[3], argv[4], argv[5]};
    App app{.name = "mouse-native-test", .package = "io.github.kisame76.minato.test.mousenative"};
    if (!prepare(app, paths, "mouse-native")) return 0;
    Capture capture;
    if (!capture.start()) {
        fail("cannot capture stderr");
        return 1;
    }
    begin({3.0, width, height, 1, 1, 120, "de", "DE"});
    if (!start(app, app.guest + "/base.apk", "mouse-native-test", "ANativeActivity_onCreate")) _exit(1);

    const std::uint64_t window = apkrt_window_add(nullptr, width, height);
    post(APKRT_EVENT_CREATE);
    post(APKRT_EVENT_START);
    post(APKRT_EVENT_RESUME);
    const std::uint64_t channel = apkrt_input_add();
    if (channel == 0) fail("apkrt_input_add gave no channel");
    post(APKRT_EVENT_INPUT_QUEUE_CREATED, channel);
    answered(show_window(window, true), "the start");
    apkrt_window_set_gate(window, 1);

    bool ready = false;
    for (int tries = 0; tries < 200 && !ready; ++tries) {
        ready = capture.drain().find("I/mouse-native-test: ready: mouse") != std::string::npos;
        if (!ready) std::this_thread::sleep_for(100ms);
    }
    if (!ready) {
        fail("no \"ready: mouse\" within 20 s");
    } else {
        apkrt_device_event added{};
        added.op = APKRT_DEVICE_ADDED;
        added.device_id = mouse_device;
        added.sources = mouse_source;
        added.vendor_id = 0x46d;
        added.product_id = 0xc077;
        std::strcpy(added.name, "Minato Test Mouse");
        if (apkrt_input_post_device(channel, &added) != 1) fail("the channel refused the device");
        std::this_thread::sleep_for(500ms);
        move(channel, 630, 1220, 30, 20);
        move(channel, 640, 1220, 10, 0);
        press(channel, 1);
        move(channel, 650, 1230, 10, 10);
        release(channel, 0);
        press(channel, 2);
        release(channel, 0);
        press(channel, 4);
        release(channel, 0);
        press(channel, 1);
        press(channel, 3);
        release(channel, 1);
        release(channel, 0);
        press(channel, 8);
        release(channel, 0);
        press(channel, 16);
        release(channel, 0);
        scroll(channel, 1, 0);
        scroll(channel, -2, 0);
        scroll(channel, 0, 1);
        scroll(channel, 0, -1);
        scroll(channel, 1, 1);
        // The mouse goes while it hovers: its hover exit is the 46th motion event, which ends the app.
        added.op = APKRT_DEVICE_REMOVED;
        if (apkrt_input_post_device(channel, &added) != 1) fail("the channel refused the device's removal");
    }
    if (!wait_for([] { return !seen.requests.empty(); }, 30s)) fail("no FINISH request");
    quit(window);
    if (!wait_for([] { return seen.ended; }, 10s)) fail("the program did not end after onDestroy");

    const std::string log = capture.drain();
    std::lock_guard hold(seen.lock);
    if (seen.status != 0) fail("activity-host ended with status " + std::to_string(seen.status));
    std::string requests;
    for (const auto &request : seen.requests) requests += "[" + request + "]";
    if (requests != "[FINISH]") fail("the requests were " + requests);
    const std::string app_lines = lines_with(log, "I/mouse-native-test: ");
    if (app_lines != read_text(paths.expected / "mouse-native-test.logcat")) {
        fail("the app's lines differ from Android 15's:\n" + app_lines);
    }
    ::dprintf(report, failures == 0 ? "PASS: mouse-native-test\n" : "%d failures\n", failures);
    _exit(failures == 0 ? 0 : 1);
}
