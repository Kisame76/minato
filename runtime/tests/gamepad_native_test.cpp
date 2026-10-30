// ADR-0068 decision 5, step 4's gamepad-native-test on the Mac: a scripted host (app_host.h) that runs
// gamepad-native-test (fixtures/apps/gamepad-native-test) through activity-host as native_ime_test.cpp runs its app,
// and plays the game controller that apk-run's pad: steps and the App's GCController side play: the device record of an
// Xbox Wireless Controller, each button's down and up as key records of source 0x501, and the sticks, triggers and hat
// as axes records of source 0x01000010, each report followed by the rest. It passes when the app's lines are
// fixtures/expected/gamepad-native-test.logcat (Android 15's, from `uinput`, tools/android-reference.sh --app
// gamepad-native-test), its motion device lines are those the AVD gave, the only request was FINISH, and activity-host
// ended with status 0. The keys' scan codes are the evdev codes of a device that has them, which a host record may
// leave 0; they are not compared.
//   gamepad_native_test <scratch directory> <fixtures bin> <bionic lib> <guest system> <fixtures/expected>
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

constexpr std::int32_t pad = 1001;
constexpr std::int32_t gamepad = 0x501, joystick = 0x01000010;
constexpr std::int32_t move = 2;

std::int64_t now() {
    return static_cast<std::int64_t>(::clock_gettime_nsec_np(CLOCK_UPTIME_RAW));
}

void post_key(std::uint64_t channel, std::int32_t key_code) {
    apkrt_key_event key{};
    key.key_code = key_code;
    key.device_id = pad;
    key.source = gamepad;
    key.down_time = key.event_time = now();
    key.action = 0;
    if (apkrt_input_post_key(channel, &key) != 1) fail("the channel refused a key's down");
    key.action = 1;
    if (apkrt_input_post_key(channel, &key) != 1) fail("the channel refused a key's up");
}

/// One report of the stick, trigger or hat, and the rest after it (no axis set).
void post_axes(std::uint64_t channel, std::initializer_list<apkrt_axis_value> values) {
    apkrt_axes_event axes{};
    axes.action = move;
    axes.device_id = pad;
    axes.source = joystick;
    axes.event_time = now();
    for (const auto &value : values) axes.axes[axes.axis_count++] = value;
    if (apkrt_input_post_axes(channel, &axes) != 1) fail("the channel refused an axes record");
    axes.axis_count = 0;
    axes.event_time = now();
    if (apkrt_input_post_axes(channel, &axes) != 1) fail("the channel refused an axes rest");
}

}  // namespace

int main(int argc, char **argv) {
    if (argc != 6) {
        std::fprintf(stderr, "usage: gamepad_native_test <scratch> <fixtures bin> <bionic lib> <guest system> "
                             "<expected>\n");
        return 2;
    }
    const Paths paths{argv[1], argv[2], argv[3], argv[4], argv[5]};
    App app{.name = "gamepad-native-test", .package = "io.github.kisame76.minato.test.gamepadnative"};
    if (!prepare(app, paths, "gamepad-native")) return 0;

    Capture capture;
    if (!capture.start()) {
        fail("cannot capture stderr");
        return 1;
    }
    begin({3.0, width, height, 1, 1, 120, "de", "DE"});
    if (!start(app, app.guest + "/base.apk", "gamepad-native-test", "ANativeActivity_onCreate")) _exit(1);

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
        ready = capture.drain().find("I/gamepad-native-test: ready: pad") != std::string::npos;
        if (!ready) std::this_thread::sleep_for(100ms);
    }
    if (!ready) {
        fail("no \"ready: pad\" within 20 s");
    } else {
        apkrt_device_event added{};
        added.op = APKRT_DEVICE_ADDED;
        added.device_id = pad;
        added.sources = 0x1000511;
        added.vendor_id = 0x45e;
        added.product_id = 0x2e0;
        std::strcpy(added.name, "Minato Test Pad");
        if (apkrt_input_post_device(channel, &added) != 1) fail("the channel refused the device");
        std::this_thread::sleep_for(500ms);
        for (const std::int32_t key : {96, 97, 99, 100, 102, 103, 108, 109, 110, 106, 107}) post_key(channel, key);
        post_axes(channel, {{0, 0.5f}});                          // AXIS_X
        post_axes(channel, {{1, -1.0f}});                         // AXIS_Y
        post_axes(channel, {{11, 0.25f}});                        // AXIS_Z
        post_axes(channel, {{14, -0.5f}});                        // AXIS_RZ
        post_axes(channel, {{17, 0.5f}});                         // AXIS_LTRIGGER
        post_axes(channel, {{18, 1.0f}});                         // AXIS_RTRIGGER
        post_axes(channel, {{15, -1.0f}});                        // AXIS_HAT_X
        post_axes(channel, {{16, 1.0f}});                         // AXIS_HAT_Y
        post_axes(channel, {{0, 1.0f}, {1, -0.5f}});              // both sticks' axes
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
    const std::string app_lines = lines_with(log, "I/gamepad-native-test: ");
    if (app_lines != read_text(paths.expected / "gamepad-native-test.logcat")) {
        fail("the app's lines differ from Android 15's:\n" + app_lines);
    }
    // The AVD's motion device lines: the pad's, no tool, buttons, flags or precision, the down time not the event time,
    // and the place the stick's X and Y.
    const std::string common = "I/gamepad-native-test-device: motion: device pad, tool 0, buttons 0x0, meta 0x0, "
                               "flags 0x0, edge 0x0, classification 0, down time != event time, precision 0 0, "
                               "pointer 0, place ";
    std::string wanted;
    for (int at = 0; at < 18; ++at) {
        const char *place = at == 0 ? "0.5 0 raw 0.5 0" : at == 2 ? "0 -1 raw 0 -1" : at == 16 ? "1 -0.5 raw 1 -0.5"
                                                                                                  : "0 0 raw 0 0";
        wanted += common + place + "\n";
    }
    std::string motion_lines;
    {
        const std::string device_log = lines_with(log, "I/gamepad-native-test-device: ");
        std::size_t from = 0;
        while (from < device_log.size()) {
            const std::size_t end = device_log.find('\n', from);
            const std::string line = device_log.substr(from, end - from + 1);
            if (line.find("device: motion:") != std::string::npos) motion_lines += line;
            from = end + 1;
        }
    }
    if (motion_lines != wanted) fail("the motion device lines differ:\n" + motion_lines);
    ::dprintf(report, failures == 0 ? "PASS: gamepad-native-test\n" : "%d failures\n", failures);
    _exit(failures == 0 ? 0 : 1);
}
