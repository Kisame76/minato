// ADR-0036's input-test on the Mac: a scripted host (app_host.h) that runs input-test (fixtures/apps/input-test)
// through activity-host with the start of ADR-0028's test 3, whose INPUT_QUEUE_CREATED carries the ID of an
// input channel, as the App makes one at the first onResume. At the app's KEEP_SCREEN_ON, its "ready", it plays
// the gestures of gestures.txt with apkrt_input_post, as the App posts touches: places in the window's pixels,
// times on CLOCK_UPTIME_RAW, and the samples of a MOVE as its history, 1 ms apart up to the post. Each event's
// own sample has pressure 1, as without force, and a touch major of 120 pixels; the history's pressure is 0.1 less
// and its touch major 10 pixels less for each sample back, so that the device lines show each sample's values.
// It passes when the channel took every event, the app's lines are fixtures/expected/input-test.logcat
// (tools/android-reference.sh --app input-test), its device lines are those below, the requests were
// KEEP_SCREEN_ON and FINISH, activity-host ended with status 0, and the channel was closed with the queue.
//   input_test <scratch directory> <fixtures bin> <bionic lib> <guest /system> <fixtures/expected> <gestures.txt>
#include "app_host.h"

#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace app_host;
using namespace std::chrono_literals;

constexpr float touch_major = 120;

// The frame by the CPU fails, as drawing by the CPU is not in M3 (ADR-0028, decision 7). The getters give what
// the host posted and libandroid's fixed values (ADR-0036, decision 3).
constexpr const char *device_lines =
    "I/input-test-device: a frame by the CPU: ANativeWindow_lock -38\n"
    "I/input-test-device: device 1, source 0x1002, flags 0x0, meta 0x0, buttons 0x0, edge 0x0, classification 0, "
    "action button 0x0\n"
    "I/input-test-device: precision 1.0, 1.0, offset 0.0, 0.0\n"
    "I/input-test-device: pressure 1.000, size 0.000, touch 120.0, 120.0, tool 120.0, 120.0, orientation 0.000\n"
    "I/input-test-device: tap: DOWN at 322.5, 699.0\n"
    "I/input-test-device: tap: 2 events, 0 MOVEs, 0 with history, 0 historical samples\n"
    "I/input-test-device: swipe: DOWN at 258.0, 1398.0\n"
    "I/input-test-device: swipe: the first MOVE with history, sample 0: x 516.0, 2.0 ms before the event, touch major "
    "100.0, pressure 0.80\n"
    "I/input-test-device: swipe: the first MOVE with history, sample 1: x 580.5, 1.0 ms before the event, touch major "
    "110.0, pressure 0.90\n"
    "I/input-test-device: swipe: the first MOVE with history, sample 2: x 645.0, 0.0 ms before the event, touch major "
    "120.0, pressure 1.00\n"
    "I/input-test-device: swipe: the first MOVE with history: raw places as the places, axis values as the values, "
    "touch minor and tool sizes as the touch major, size and orientation 0: yes\n"
    "I/input-test-device: swipe: 6 events, 4 MOVEs, 2 with history, 4 historical samples\n"
    "I/input-test-device: two fingers: DOWN at 387.0, 1118.4\n"
    "I/input-test-device: two fingers: 7 events, 3 MOVEs, 0 with history, 0 historical samples\n"
    "I/input-test-device: lowest free ID: DOWN at 258.0, 1957.2\n"
    "I/input-test-device: lowest free ID: 6 events, 0 MOVEs, 0 with history, 0 historical samples\n";

/// A line of gestures.txt: one motion event.
struct Step {
    int line = 0;
    int delay = 0;            // in ms before it
    std::int32_t action = 0;  // with the pointer's index
    std::vector<std::int32_t> ids;
    std::vector<std::vector<apkrt_pointer_coords>> samples;  // oldest first, a place per pointer in each; the
                                                              // pressure and touch major come with the post
};

/// The action `text` names: DOWN, UP, MOVE, POINTER_DOWN(<index>) or POINTER_UP(<index>); -1 for another,
/// CANCEL included, which the emulator's touchscreen cannot play.
std::int32_t action_of(const std::string &text) {
    if (text == "DOWN") return 0;
    if (text == "UP") return 1;
    if (text == "MOVE") return 2;
    int index = -1;
    char close = 0;
    if (std::sscanf(text.c_str(), "POINTER_DOWN(%d%c", &index, &close) == 2 && close == ')') return 5 | (index << 8);
    if (std::sscanf(text.c_str(), "POINTER_UP(%d%c", &index, &close) == 2 && close == ')') return 6 | (index << 8);
    return -1;
}

/// The events of the gestures at `path`, in the window's pixels; empty after a line it cannot read.
std::vector<Step> read_gestures(const fs::path &path) {
    std::ifstream file(path);
    std::vector<Step> steps;
    std::string line;
    for (int number = 1; std::getline(file, line); ++number) {
        std::istringstream words(line);
        std::string delay, action, word;
        if (!(words >> delay) || delay[0] == '#') continue;
        Step step;
        step.line = number;
        step.delay = std::atoi(delay.c_str());
        words >> action;
        step.action = action_of(action);
        std::vector<apkrt_pointer_coords> sample;
        std::vector<std::int32_t> ids;
        bool sound = step.action >= 0;
        // Each sample lists the same pointers, those of the first.
        const auto end_sample = [&] {
            if (step.samples.empty()) step.ids = ids;
            sound = sound && !ids.empty() && ids == step.ids;
            step.samples.push_back(sample);
            sample.clear();
            ids.clear();
        };
        while (words >> word) {
            if (word == "|") {
                end_sample();
                continue;
            }
            int id = -1;
            float x = 0, y = 0;
            sound = sound && std::sscanf(word.c_str(), "%d:%f,%f", &id, &x, &y) == 3;
            ids.push_back(id);
            sample.push_back({x * static_cast<float>(width), y * static_cast<float>(height), 0, 0});
        }
        end_sample();
        if (!sound) {
            fail(path.string() + ":" + std::to_string(number) + " is no motion event");
            return {};
        }
        steps.push_back(step);
    }
    return steps;
}

/// Posts `steps` to the input channel `channel`, each after its delay.
void play(const std::vector<Step> &steps, std::uint64_t channel) {
    std::int64_t down_time = 0;
    for (const auto &step : steps) {
        std::this_thread::sleep_for(std::chrono::milliseconds(step.delay));
        const auto now = static_cast<std::int64_t>(::clock_gettime_nsec_np(CLOCK_UPTIME_RAW));
        if (step.action == 0) down_time = now;
        std::vector<std::int64_t> times;
        std::vector<apkrt_pointer_coords> places;
        const auto count = static_cast<std::int64_t>(step.samples.size());
        for (std::int64_t sample = 0; sample < count; ++sample) {
            const std::int64_t back = count - 1 - sample;
            times.push_back(now - back * 1'000'000);
            for (auto place : step.samples[static_cast<std::size_t>(sample)]) {
                place.pressure = 1.0f - 0.1f * static_cast<float>(back);
                place.touch_major = touch_major - 10 * static_cast<float>(back);
                places.push_back(place);
            }
        }
        if (apkrt_input_post(channel, step.action, down_time, static_cast<std::uint32_t>(step.ids.size()),
                             step.ids.data(), static_cast<std::uint32_t>(times.size()), times.data(),
                             places.data()) != 1) {
            fail("the channel refused the event of line " + std::to_string(step.line));
        }
    }
}

bool requested(const std::string &request) {
    return std::find(seen.requests.begin(), seen.requests.end(), request) != seen.requests.end();
}

}  // namespace

int main(int argc, char **argv) {
    if (argc != 7) {
        std::fprintf(stderr, "usage: input_test <scratch> <fixtures bin> <bionic lib> <guest system> <expected> "
                             "<gestures.txt>\n");
        return 2;
    }
    const Paths paths{argv[1], argv[2], argv[3], argv[4], argv[5]};
    const auto steps = read_gestures(argv[6]);
    if (steps.empty()) {
        fail(std::string("no gestures in ") + argv[6]);
        return 1;
    }
    App app{.name = "input-test", .package = "io.github.kisame76.minato.test.input"};
    if (!prepare(app, paths, "input")) return 0;

    Capture capture;
    if (!capture.start()) {
        fail("cannot capture stderr");
        return 1;
    }
    begin({3.0, width, height, 1, 1, 120, "de", "DE"});
    if (!start(app, app.guest + "/base.apk", "input-test", "ANativeActivity_onCreate")) _exit(1);

    // The start with the input channel, then active: the window's gate opens, and the app is ready once it
    // has the focus.
    const std::uint64_t window = apkrt_window_add(nullptr, width, height);
    post(APKRT_EVENT_CREATE);
    post(APKRT_EVENT_START);
    post(APKRT_EVENT_RESUME);
    const std::uint64_t channel = apkrt_input_add();
    if (channel == 0) fail("apkrt_input_add gave no channel");
    post(APKRT_EVENT_INPUT_QUEUE_CREATED, channel);
    answered(show_window(window, true), "the start");
    apkrt_window_set_gate(window, 1);

    if (wait_for([] { return requested("KEEP_SCREEN_ON 1"); }, 20s)) {
        play(steps, channel);
    } else {
        fail("no KEEP_SCREEN_ON request");
    }
    // A stalled app finishes 30 s after its "ready".
    if (!wait_for([] { return requested("FINISH"); }, 45s)) fail("no FINISH request");
    quit(window);
    if (capture.drain().find("I/input-test: android_main returns\n") == std::string::npos) {
        fail("DESTROY was answered before android_main returned");
    }
    // activity-host destroyed the queue before onDestroy, which ended the channel.
    const std::int32_t id = 0;
    const auto time = static_cast<std::int64_t>(::clock_gettime_nsec_np(CLOCK_UPTIME_RAW));
    const apkrt_pointer_coords place{1, 1, 1, touch_major};
    if (apkrt_input_post(channel, 0, time, 1, &id, 1, &time, &place) != 0) fail("the channel is open after onDestroy");
    if (!wait_for([] { return seen.ended; }, 10s)) fail("the program did not end after onDestroy");

    const std::string log = capture.drain();
    std::lock_guard hold(seen.lock);
    if (seen.status != 0) fail("activity-host ended with status " + std::to_string(seen.status));
    std::string requests;
    for (const auto &request : seen.requests) requests += "[" + request + "]";
    if (requests != "[KEEP_SCREEN_ON 1][FINISH]") fail("the requests were " + requests);
    const std::string app_lines = lines_with(log, "I/input-test: ");
    if (app_lines != read_text(paths.expected / "input-test.logcat")) {
        fail("the app's lines differ from Android 15's:\n" + app_lines);
    }
    const std::string device = lines_with(log, "I/input-test-device: ");
    if (device != device_lines) fail("the device lines differ:\n" + device);
    ::dprintf(report, failures == 0 ? "PASS: input-test\n" : "%d failures\n", failures);
    _exit(failures == 0 ? 0 : 1);
}
