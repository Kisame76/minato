// ADR-0068 step 2e's native-ime-test on the Mac: a scripted host (app_host.h) that runs native-ime-test
// (fixtures/java/native-ime-test) through activity-host with the start of input_test.cpp. The app asks for the soft
// keyboard, which Android 15 ignores for a NativeActivity's view ("is not served"), so no request reaches the host and
// a text record the host posts anyway goes nowhere. Once the app logs "soft input done" the host posts a keyboard
// device, which goes nowhere too, then B and DEL as the AVD's `input keyevent` sends them (ADR-0069 step 3): the
// virtual keyboard's (device -1) with adb's source 0, each down and up at the same time. It passes when the app's
// lines are fixtures/expected/native-ime-test.logcat (the AVD's, tools/android-reference.sh --app native-ime-test),
// its device lines are those below, the only request was FINISH, and activity-host ended with status 0. The APK is
// the one with the input method (tools/build-java-test-app.sh native-ime-test); the library is the fixtures' build of
// the same source. With
// "single" the app starts at native_ime_test_single_onCreate, which takes one event per looper callback and finishes
// B's predispatched down as handled: its lines are single_lines below.
//   native_ime_test <scratch directory> <fixtures bin> <bionic lib> <guest system> <fixtures/expected> <APK>
//                   [single]
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

constexpr std::int32_t device = 1000;  // HardwareKeyboard.deviceID, for the device record
constexpr std::int32_t keyboard = 0x101;  // AINPUT_SOURCE_KEYBOARD

// The AVD's: adb's keys are the virtual keyboard's (-1), each up at its down's time.
constexpr const char *device_lines =
    "I/native-ime-test-device: key down B: device -1, scan code 0, down time = event time\n"
    "I/native-ime-test-device: key up B: device -1, scan code 0, down time = event time\n"
    "I/native-ime-test-device: key down DEL: device -1, scan code 0, down time = event time\n"
    "I/native-ime-test-device: key up DEL: device -1, scan code 0, down time = event time\n";

// The single variant's: B's down ends in its predispatch; every other key comes twice, each delivery woken alone.
constexpr const char *single_lines =
    "I/native-ime-test: ready\n"
    "I/native-ime-test: showSoftInput 0\n"
    "I/native-ime-test: no keys: showSoftInput forced\n"
    "I/native-ime-test: soft input done: 0 key events\n"
    "I/native-ime-test: hideSoftInput 0\n"
    "I/native-ime-test: predispatch handled: down B\n"
    "I/native-ime-test: pre-dispatched\n"
    "I/native-ime-test: key up B, meta 0x0, flags 0x0, repeat 0, source 0x0\n"
    "I/native-ime-test: pre-dispatched\n"
    "I/native-ime-test: key down DEL, meta 0x0, flags 0x0, repeat 0, source 0x0\n"
    "I/native-ime-test: pre-dispatched\n"
    "I/native-ime-test: key up DEL, meta 0x0, flags 0x0, repeat 0, source 0x0\n"
    "I/native-ime-test: done\n";

std::int64_t now() {
    return static_cast<std::int64_t>(::clock_gettime_nsec_np(CLOCK_UPTIME_RAW));
}

void post_key(std::uint64_t channel, std::int32_t key_code) {
    apkrt_key_event key{};
    key.key_code = key_code;
    key.device_id = -1;  // adb's virtual keyboard, source 0
    key.down_time = key.event_time = now();
    key.action = 0;
    if (apkrt_input_post_key(channel, &key) != 1) fail("the channel refused a key's down");
    key.action = 1;
    if (apkrt_input_post_key(channel, &key) != 1) fail("the channel refused a key's up");
}

}  // namespace

int main(int argc, char **argv) {
    if (argc != 7 && argc != 8) {
        std::fprintf(stderr, "usage: native_ime_test <scratch> <fixtures bin> <bionic lib> <guest system> <expected> "
                             "<APK> [single]\n");
        return 2;
    }
    const Paths paths{argv[1], argv[2], argv[3], argv[4], argv[5]};
    App app{.name = "native-ime-test", .package = "io.github.kisame76.minato.test.nativeime", .apk_file = argv[6]};
    const bool single = argc == 8 && std::strcmp(argv[7], "single") == 0;
    if (!prepare(app, paths, single ? "native-ime-single" : "native-ime")) return 0;

    Capture capture;
    if (!capture.start()) {
        fail("cannot capture stderr");
        return 1;
    }
    begin({3.0, width, height, 1, 1, 120, "de", "DE"});
    if (!start(app, app.guest + "/base.apk", "native-ime-test",
               single ? "native_ime_test_single_onCreate" : "ANativeActivity_onCreate")) {
        _exit(1);
    }

    const std::uint64_t window = apkrt_window_add(nullptr, width, height);
    post(APKRT_EVENT_CREATE);
    post(APKRT_EVENT_START);
    post(APKRT_EVENT_RESUME);
    const std::uint64_t channel = apkrt_input_add();
    if (channel == 0) fail("apkrt_input_add gave no channel");
    post(APKRT_EVENT_INPUT_QUEUE_CREATED, channel);
    answered(show_window(window, true), "the start");
    apkrt_window_set_gate(window, 1);

    // A text the host would type if a keyboard had come: the view has no editor, and nothing reaches the app.
    apkrt_text_event text{};
    text.op = APKRT_TEXT_COMMIT;
    text.arg0 = 1;
    text.length = 1;
    text.text[0] = 'h';
    if (apkrt_input_post_text(channel, &text) != 1) fail("the channel refused the text");

    bool soft_done = false;
    for (int tries = 0; tries < 200 && !soft_done; ++tries) {
        soft_done = capture.drain().find("I/native-ime-test: soft input done") != std::string::npos;
        if (!soft_done) std::this_thread::sleep_for(100ms);
    }
    if (soft_done) {
        apkrt_device_event added{};
        added.op = APKRT_DEVICE_ADDED;
        added.device_id = device;
        added.sources = keyboard;
        std::strcpy(added.name, "Minato keyboard");
        if (apkrt_input_post_device(channel, &added) != 1) fail("the channel refused the device");
        std::this_thread::sleep_for(1s);
        post_key(channel, 30);  // AKEYCODE_B
        std::this_thread::sleep_for(1s);
        post_key(channel, 67);  // AKEYCODE_DEL
    } else {
        fail("no \"soft input done\" within 20 s");
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
    const std::string app_lines = lines_with(log, "I/native-ime-test: ");
    if (app_lines != (single ? std::string(single_lines) : read_text(paths.expected / "native-ime-test.logcat"))) {
        fail("the app's lines differ from " + std::string(single ? "the single variant's" : "Android 15's") + ":\n" +
             app_lines);
    }
    const std::string device_log = lines_with(log, "I/native-ime-test-device: ");
    const std::string device_wanted = single ? std::string(device_lines).substr(std::strlen(
        "I/native-ime-test-device: key down B: device -1, scan code 0, down time = event time\n")) : device_lines;
    if (device_log != device_wanted) fail("the device lines differ:\n" + device_log);
    ::dprintf(report, failures == 0 ? "PASS: native-ime-test\n" : "%d failures\n", failures);
    _exit(failures == 0 ? 0 : 1);
}
