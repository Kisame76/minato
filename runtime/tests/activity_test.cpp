// ADR-0028's test 3 on the Mac: a scripted host (app_host.h) that plays ADR-0032's lifecycle machine for
// activity-test (fixtures/apps/activity-test), as the App does in game mode. It begins the session with a fixed
// configuration, starts /system/bin/activity-host, and posts the events of the start, of HOME, of the return
// and of the quit that follows the app's ANativeActivity_finish, in the order Android 15 calls the callbacks
// (tools/android-reference.sh --app activity-test), with the window's steps of decision 6 around them. It
// passes when the app's lines of the guest's log (stderr, logcat's tag format) are
// fixtures/expected/activity-test.logcat, its device lines are those of the configuration given here, the
// requests were KEEP_SCREEN_ON and FINISH, the three folders exist, and activity-host ended with status 0 once
// onDestroy was answered. A sixth argument names a variant: polling starts the app at
// activity_test_polling_onCreate, whose onStart polls the main thread's looper first, and must give the same
// log; leftovers leaves NATIVE_WINDOW_DESTROYED out, so activity-host destroys the window before the next one
// and before onDestroy, as NativeActivity does; missing-library, missing-function and missing-apk start
// activity-host with what is not there and pass when it reports the reason as START_FAILED and ends with
// status 1.
//   activity_test <scratch directory> <fixtures bin> <bionic lib> <guest /system> <fixtures/expected> [<case>]
#include "app_host.h"

#include <unistd.h>

#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace app_host;
using namespace std::chrono_literals;

// What the device lines must say for that configuration (decision 9): density 3 × 160, 430 × 932 dp, a
// normal and long screen, portrait, and the window and content rectangle in pixels.
constexpr const char *device_lines =
    "I/activity-test-device: configuration: mcc 0, mnc 0, locale de-DE, density 480, orientation 1, touchscreen 3, "
    "keyboard 1, keys hidden 2, navigation 1, navigation hidden 0\n"
    "I/activity-test-device: configuration: screen size 2, long 2, round 1, 430 x 932 dp, smallest 430 dp, "
    "layout direction 1, UI mode 1, night 1, grammatical gender 0\n"
    "I/activity-test-device: window 1: 1290 x 2796\n"
    "I/activity-test-device: window 1: host window 1\n"
    "I/activity-test-device: content rect: 6, 120, 1284, 2760\n"
    "I/activity-test-device: window 2: 1290 x 2796\n"
    "I/activity-test-device: window 2: host window 2\n";

/// Android's lines as activity-host gives them when the host leaves NATIVE_WINDOW_DESTROYED out: the window
/// goes right before the next one comes, and at DESTROY right after onStop, before the input queue.
std::string without_window_destroyed(const std::string &reference) {
    std::vector<std::string> lines;
    std::istringstream input(reference);
    for (std::string line; std::getline(input, line);) lines.push_back(line);
    const std::string term = "I/activity-test: APP_CMD_TERM_WINDOW", init = "I/activity-test: APP_CMD_INIT_WINDOW",
                      stop = "I/activity-test: APP_CMD_STOP";
    const auto index_of = [&](const std::string &line, int occurrence) {
        for (std::size_t index = 0; index < lines.size(); ++index) {
            if (lines[index] == line && --occurrence == 0) return index;
        }
        return lines.size();
    };
    lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(index_of(term, 1)));
    lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(index_of(init, 2)), term);
    lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(index_of(term, 2)));
    lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(index_of(stop, 2) + 1), term);
    std::string result;
    for (const auto &line : lines) result += line + "\n";
    return result;
}

}  // namespace

int main(int argc, char **argv) {
    const std::string variant = argc == 7 ? argv[6] : "";
    const bool failing = variant.starts_with("missing-"), leftovers = variant == "leftovers";
    if ((argc != 6 && argc != 7) || (argc == 7 && variant != "polling" && variant != "leftovers" &&
                                     variant != "missing-library" && variant != "missing-function" &&
                                     variant != "missing-apk")) {
        std::fprintf(stderr, "usage: activity_test <scratch> <fixtures bin> <bionic lib> <guest system> <expected> "
                             "[polling | leftovers | missing-library | missing-function | missing-apk]\n");
        return 2;
    }
    const Paths paths{argv[1], argv[2], argv[3], argv[4], argv[5]};
    App app{.name = "activity-test", .package = "io.github.kisame76.minato.test.activity"};
    if (!prepare(app, paths, "activity" + variant)) return 0;

    Capture capture;
    if (!capture.start()) {
        fail("cannot capture stderr");
        return 1;
    }
    begin({3.0, width, height, 1, 1, 120, "de", "DE"});

    const std::string apk = app.guest + (variant == "missing-apk" ? "/missing.apk" : "/base.apk");
    const char *const library = variant == "missing-library" ? "missing" : "activity-test";
    const char *const function = variant == "missing-function" ? "missing_onCreate"
                                 : variant == "polling"        ? "activity_test_polling_onCreate"
                                                               : "ANativeActivity_onCreate";
    if (!start(app, apk, library, function)) _exit(1);

    // An app that cannot start: the reason reaches the host before the program ends with status 1.
    if (failing) {
        if (!wait_for([] { return seen.ended; }, 10s)) fail("activity-host did not end");
        const std::string reason = variant == "missing-library"
                                       ? "START_FAILED dlopen failed: library \"libmissing.so\" not found"
                                   : variant == "missing-function"
                                       ? "START_FAILED undefined symbol: missing_onCreate"
                                       : "START_FAILED cannot read the APK " + app.guest + "/missing.apk";
        std::lock_guard hold(seen.lock);
        if (seen.status != 1) fail("activity-host ended with status " + std::to_string(seen.status));
        if (seen.requests.size() != 1 || seen.requests[0] != reason) {
            fail("the requests were not [" + reason + "]: " + (seen.requests.empty() ? "none" : seen.requests[0]));
        }
        if (failures == 0) ::dprintf(report, "PASS: %s\n", variant.c_str());
        _exit(failures == 0 ? 0 : 1);
    }

    // The start, then active: the window's gate opens.
    const std::uint64_t first = apkrt_window_add(nullptr, width, height);
    post(APKRT_EVENT_CREATE);
    post(APKRT_EVENT_START);
    post(APKRT_EVENT_RESUME);
    post(APKRT_EVENT_INPUT_QUEUE_CREATED, 0);
    answered(show_window(first, true), "the start");
    apkrt_window_set_gate(first, 1);

    // HOME: the background steps of decision 6, then the events from onPause on.
    apkrt_set_background(1);
    if (apkrt_wait_scheduled(1.0) != 1) fail("\"scheduled\" without providers did not answer yes");
    apkrt_window_remove(first);
    post(APKRT_EVENT_PAUSE);
    post(APKRT_EVENT_WINDOW_FOCUS_CHANGED, 0);
    if (!leftovers) post(APKRT_EVENT_NATIVE_WINDOW_DESTROYED, first);
    post(APKRT_EVENT_STOP);
    answered(post(APKRT_EVENT_SAVE_INSTANCE_STATE), "the background");
    // An answer comes once the callback returned: native_app_glue's onSaveInstanceState waits for the app's
    // thread, which logs the command first.
    if (capture.drain().find("I/activity-test: APP_CMD_SAVE_STATE\n") == std::string::npos) {
        fail("SAVE_INSTANCE_STATE was answered before its callback returned");
    }

    // The return, with a new window.
    apkrt_set_background(0);
    const std::uint64_t second = apkrt_window_add(nullptr, width, height);
    post(APKRT_EVENT_START);
    post(APKRT_EVENT_RESUME);
    answered(show_window(second, false), "the return");
    apkrt_window_set_gate(second, 1);

    // The app finishes itself a second after its second window gained the focus; then the quit sequence.
    if (!wait_for([] { return !seen.requests.empty() && seen.requests.back() == "FINISH"; }, 10s)) {
        fail("no FINISH request");
    }
    quit(second, !leftovers);
    // The glue's onDestroy returns once android_main has returned.
    if (capture.drain().find("I/activity-test: android_main returns\n") == std::string::npos) {
        fail("DESTROY was answered before its callback returned");
    }
    if (!wait_for([] { return seen.ended; }, 10s)) fail("the program did not end after onDestroy");

    const std::string log = capture.drain();
    std::lock_guard hold(seen.lock);
    if (seen.status != 0) fail("activity-host ended with status " + std::to_string(seen.status));
    std::string requests;
    for (const auto &request : seen.requests) requests += "[" + request + "]";
    if (requests != "[KEEP_SCREEN_ON 1][FINISH]") fail("the requests were " + requests);
    const std::string app_lines = lines_with(log, "I/activity-test: ");
    const std::string reference = read_text(paths.expected / "activity-test.logcat");
    if (app_lines != (leftovers ? without_window_destroyed(reference) : reference)) {
        fail("the app's lines differ from Android 15's:\n" + app_lines);
    }
    const std::string device = lines_with(log, "I/activity-test-device: ");
    if (device != device_lines) fail("the device lines differ:\n" + device);
    for (const auto &folder : {app.data / "files", app.external / "files", app.obb}) {
        if (!fs::is_directory(folder)) fail(folder.string() + " is not a folder");
    }
    ::dprintf(report, failures == 0 ? "PASS: activity-test\n" : "%d failures\n", failures);
    _exit(failures == 0 ? 0 : 1);
}
