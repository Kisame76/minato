// ADR-0029 B2 and ADR-0030 V5 on the Mac, and for the live variants ADR-0032's test 8 with ADR-0036 a): a scripted
// host that plays the App's lifecycle for one of the test apps of fixtures/apps through activity-host, with
// graphics/host's providers of ANGLE and MoltenVK and windows on graphics/host's layers (the App's, ADR-0037
// decision 2) the size of an iPhone 15 Pro Max's screen (430 x 932 points at scale 3). It converts activity-host,
// the system's libraries, the front libs and the app's library into containers signed ad hoc, mounts the guest
// paths, begins the session, starts the app with the function its manifest names (android.app.func_name, else
// ANativeActivity_onCreate), and posts the events in the
// order the App's SessionBridge gives them.
// - gles-test and vulkan-test (<mode> is the presents): the start with its window and the focus, then the gate
//   opened; after the app's ANativeActivity_finish the focus lost, the gate closed, the window removed, onPause,
//   onNativeWindowDestroyed, onStop and onDestroy. It passes when the app's lines of the guest's log (stderr in
//   logcat's tag format) are fixtures/expected/<app>.logcat, Android 15's, the only request was FINISH, the window
//   counted <presents> presents, onNativeWindowDestroyed and onDestroy were answered within 500 ms, and
//   activity-host ended with status 0. The app's device lines are printed, not compared. The window's layer is
//   still opaque at the end, as no request said otherwise.
// - gles-test with <mode> translucent:<presents>, the opaque layer's part G (V5's late frames): the same, started at
//   ANativeActivity_onCreateTranslucent, which asks for WINDOW_FORMAT_RGBA_8888 in onCreate. It passes as above
//   when the requests were WINDOW_OPAQUE 0 and FINISH, and the window's layer is not opaque at the end.
// - gles-live and vulkan-live (<mode> live), which draw until onDestroy: a thread stands in for the App's frame clock,
//   with ticks at the rate the window prefers (FRAME_RATE; 120 without a preference) while the window's gate is open.
//   Their manifests' activities handle orientation and screenSize (configChanges), so the window turns as the App turns
//   it (ADR-0038 c): the layer's new bounds, onConfigurationChanged, the window's new size (apkrt_window_set_size),
//   onNativeWindowResized, onContentRectChanged, onNativeWindowRedrawNeeded. The start; 6 s of frames, over one change
//   of the rate, with a turn to landscape after 2 s and back after 4 s; HOME as iOS gives it, the scene inactive (the
//   focus lost, the gate closed) and 300 ms later in the background (the window removed, onPause,
//   onNativeWindowDestroyed, onStop and onSaveInstanceState with the gates open, then the background flag and
//   "scheduled" with 1 s to go); 0.5 s away; the return with a new window (the foreground, onStart, onResume, the
//   window's events, the focus, the gate opened); 2 s of frames; Quit. It passes when both windows counted presents,
//   "scheduled" said yes, every onNativeWindowDestroyed, onDestroy and the turns' onNativeWindowRedrawNeeded was
//   answered within 500 ms, each turn gave vulkan-live a present that answered VK_SUBOPTIMAL_KHR and a new swapchain at
//   the window's new size and gles-live its surface at that size within 2 frames of APP_CMD_WINDOW_RESIZED (the apps'
//   device lines), guest threads committed no command buffer from "scheduled" to the foreground (metal_hook.m with
//   MINATO_METAL_HOOK=count), the only requests were FRAME_RATE, with 120 and 60 among them, the app's onDestroy line
//   says "windows 2, failures 0", and activity-host ended with status 0.
// - present-gles and present-vulkan (<mode> present), ADR-0041 test 3: present-test's library, which asks for its
//   presents' times. The start with the frame clock; the app's frames at 60 and then 120 frames a second until its
//   own ANativeActivity_finish; the App's quit. It passes when the window counted 1200 presents, the requests were
//   FRAME_RATE 60, FRAME_RATE 120 and FINISH, onNativeWindowDestroyed and onDestroy were answered within 500 ms, the
//   app's lines are fixtures/expected/<app>.logcat, Android 15's, and its present.csv holds the frames in order
//   with each time asked for its interval of periods after the frame time. Timing is judged on the iPhone only.
// - bench-gles and bench-vulkan (<mode> bench:<load>), ADR-0037 a): the parity benchmark's APKs from
//   tools/build-bench.sh (<fixtures build>), their manifests in graphics/bench/android (<expected>). bench.conf asks
//   for 120 Hz, the load, 9 s and the CRC of frame 1000, which the scene also draws and presents once before its
//   first frame. Ticks in lockstep with the presents: the first once the scene asked for its rate, tick k then k
//   periods of 120 Hz after it and 0.5 or 1 ms late (odd or even k), each once frame k-1 is presented, until the
//   app's own ANativeActivity_finish; then the App's quit. It passes when the window counted 1081 presents, the
//   requests were KEEP_SCREEN_ON 1, FRAME_RATE 120 and FINISH, onNativeWindowDestroyed and onDestroy were
//   answered within 500 ms, and activity-host ended with status 0; bench_test.py checks the rest.
// - The same with <mode> record:<load> (ADR-0037 b), graphics.recorder): a run of 1 s, 120 frames, which the window's
//   layer records, and a nextDrawable of the test's own before and after the recording. It passes as above with 120
//   presents, and when the recording holds every drawable once and in order (recording_check.h).
// - The same with <mode> cap:<load>:<cap>[:<unlock>] (ADR-0066 step 4): bench.conf adds the engine cap and no check,
//   the APK registers its targetFrameRate setter with libbench-scripting and draws only on the ticks its limiter
//   takes; with an unlock the runtime preloads libminato-frame-rate.so, whose wrapper raises the cap. Ticks on which
//   the engine draws wait for its present, the others pass after 2 ms. It passes as above with 1080 over every
//   presents, every being the fewest whole periods of 120 Hz, within half a period, that the rate the engine holds
//   takes (the cap raised to the unlock, -1 drawing at Unity's 30).
// - With MINATO_ANGLE_BACKEND=vulkan (ADR-0044 c)), any of the above runs on the ES 3.1 path, ANGLE's Vulkan backend on
//   MoltenVK, once graphics/host's run-time check passed, as the session runs an app whose manifest requires ES 3.1.
//   It passes as above, and with gles-test's and gles31-test's <presents> when ANGLE made one swapchain and its layer's
//   drawables have the app's buffer size, 256 x 256, and MoltenVK encoded no fence waits (Metal's hook in count mode).
// - present-limit-gles and present-limit-vulkan with <mode> limit:<rate>:<low>:<high> (ADR-0078 decision 4): the frame
//   rate limit set to <rate> (0 = none) before the program, as the App's session sets it, and no frame clock; the apps
//   present without pacing of their own until their own ANativeActivity_finish. It passes when the app's device line
//   says between <low> and <high> presents a second, the only request was FINISH, onNativeWindowDestroyed and
//   onDestroy were answered within 500 ms, and activity-host ended with status 0.
//   graphics-app-test <scratch> <app> <fixtures build> <bionic lib> <guest system> <front libs> <expected>
//                     <ANGLE's libGLESv2.dylib> <libMoltenVK.dylib>
//                     <presents | live | present | bench:<load> | record:<load> | cap:<load>:<cap>[:<unlock>] |
//                      limit:<rate>:<low>:<high>>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#include "APKRuntime/apkrt.h"
#include "apkconv/codesign.h"
#include "apkconv/container.h"
#include "minato_graphics.h"
#include "minato_layer.h"
#include "patience.h"
#include "recording_check.h"

#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// app_counts.c: the Metal hook's counts of the command buffers guest threads commit and of fence waits (test_host.h).
extern "C" void minato_app_test_count_commits(void);
extern "C" int minato_app_test_commits(void);
extern "C" int minato_app_test_fence_waits(void);

namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

// An iPhone 15 Pro Max in portrait: 430 x 932 points at scale 3.
constexpr double scale = 3;
constexpr std::int32_t width = 1290, height = 2796;

int failures = 0;
int report = 2;  // the real stderr, also once descriptor 2 is the capture's pipe
bool vulkan = false;  // MINATO_ANGLE_BACKEND=vulkan: the ES 3.1 path (ADR-0044 c))

void fail(const std::string &what) {
    ::dprintf(report, "FAIL: %s\n", what.c_str());
    ++failures;
}

/// The rate the window prefers, as FRAME_RATE's argument gives it: a float's bits, 0 for none.
std::atomic<std::uint32_t> preferred_rate{0};

/// What the session's handlers saw, written on guest threads.
struct Seen {
    std::mutex lock;
    std::condition_variable changed;
    std::vector<std::string> requests;
    bool ended = false;
    int status = -1;
};
Seen seen;

/// The layer of the window added last.
MinatoMetalLayer *last_layer;

/// The window's opacity as the runtime reports it (WINDOW_OPAQUE), opaque until a report says otherwise, as the App's
/// GameSession keeps it: each new window's layer starts with it.
std::atomic<bool> window_opaque{true};

/// FRAME_RATE comes under the window registry's lock (apkrt.h), so this calls none of its functions. WINDOW_OPAQUE
/// sets the layer on the main queue, as the App does, which the main thread's waits serve.
void on_request(void *, int type, std::uint64_t argument, const char *text) {
    const auto bits = static_cast<std::uint32_t>(argument);
    if (type == APKRT_REQUEST_FRAME_RATE) preferred_rate = bits;
    if (type == APKRT_REQUEST_WINDOW_OPAQUE) {
        const bool opaque = argument != 0;
        window_opaque = opaque;
        dispatch_async(dispatch_get_main_queue(), ^{
            last_layer.opaque = opaque;
        });
    }
    char rate[32];
    std::snprintf(rate, sizeof rate, "%g", static_cast<double>(std::bit_cast<float>(bits)));
    std::string request = type == APKRT_REQUEST_FINISH           ? "FINISH"
                          : type == APKRT_REQUEST_KEEP_SCREEN_ON ? "KEEP_SCREEN_ON " + std::to_string(argument)
                          : type == APKRT_REQUEST_START_FAILED   ? std::string("START_FAILED ") + (text ? text : "")
                          : type == APKRT_REQUEST_FRAME_RATE     ? std::string("FRAME_RATE ") + rate
                          : type == APKRT_REQUEST_WINDOW_OPAQUE  ? "WINDOW_OPAQUE " + std::to_string(argument)
                                                                 : "request " + std::to_string(type);
    const std::lock_guard hold(seen.lock);
    seen.requests.push_back(std::move(request));
    seen.changed.notify_all();
}

/// Records the status and returns, so that the thread sleeps and the main thread checks what happened.
void on_end(void *, int status) {
    const std::lock_guard hold(seen.lock);
    seen.ended = true;
    seen.status = status;
    seen.changed.notify_all();
}

/// Waits up to `limit` until `predicate` holds, serving the main queue meanwhile as the App's main thread does:
/// MoltenVK on macOS reads a layer's screen on the main queue (screenMVK through mvkDispatchToMainAndWait, e.g. in
/// vkGetRefreshCycleDurationGOOGLE), which would wait forever on a main thread that only sleeps. On iOS MoltenVK
/// reads UIScreen.mainScreen without the main queue. `limit` counts this thread's rounds (patience.h).
template <typename Predicate>
bool wait_for(Predicate predicate, std::chrono::seconds limit) {
    Patience patience(limit);
    for (;;) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0, true);
        std::unique_lock hold(seen.lock);
        if (seen.changed.wait_for(hold, 10ms, predicate)) return true;
        if (!patience.more(10ms)) return false;
    }
}

/// Whether `request` came.
bool requested(const std::string &request) {
    const std::lock_guard hold(seen.lock);
    return std::find(seen.requests.begin(), seen.requests.end(), request) != seen.requests.end();
}

/// The requests so far, each in brackets.
std::string requests_text() {
    const std::lock_guard hold(seen.lock);
    std::string text;
    for (const auto &request : seen.requests) text += "[" + request + "]";
    return text;
}

/// The guest's stderr: a pipe in place of descriptor 2 before the program starts, read by a thread that
/// keeps every byte and passes it on to the real stderr.
struct Capture {
    int real = -1;
    int read_end = -1;
    std::mutex lock;
    std::string text;
    int drains = 0;

    bool start() {
        int ends[2];
        if (::pipe(ends) != 0) return false;
        real = ::dup(2);
        if (real < 0 || ::dup2(ends[1], 2) < 0) return false;
        ::close(ends[1]);
        report = real;
        read_end = ends[0];
        std::thread([this] {
            char buffer[4096];
            for (ssize_t count; (count = ::read(read_end, buffer, sizeof buffer)) > 0;) {
                (void)!::write(real, buffer, static_cast<std::size_t>(count));
                const std::lock_guard hold(lock);
                text.append(buffer, static_cast<std::size_t>(count));
            }
        }).detach();
        return true;
    }

    /// Everything written so far: a marker line of its own written now comes through after it.
    std::string drain() {
        const std::string marker = "graphics-app-test: capture " + std::to_string(++drains) + "\n";
        (void)!::write(2, marker.data(), marker.size());
        for (int round = 0; round < 500; ++round) {
            {
                const std::lock_guard hold(lock);
                if (const auto at = text.find(marker); at != std::string::npos) return text.substr(0, at);
            }
            std::this_thread::sleep_for(10ms);
        }
        const std::lock_guard hold(lock);
        return text;
    }
};

/// The App's frame clock for the live variants: a tick per period of the rate the window prefers (120 without a
/// preference, the session's refresh_rate), as DisplayLinkClock follows setPreferredRate; paused unless the
/// window's gate is open, as GameSession runs it.
class Vsync {
public:
    std::atomic<bool> paused{true};
    std::atomic<int> ticks{0};

    void start() {
        thread_ = std::thread([this] { run(); });
    }

    void stop() {
        running_ = false;
        thread_.join();
    }

private:
    std::atomic<bool> running_{true};
    std::thread thread_;

    static std::int64_t now() {
        return static_cast<std::int64_t>(clock_gettime_nsec_np(CLOCK_UPTIME_RAW));
    }

    void run() {
        std::int64_t tick = now();
        while (running_) {
            const float rate = std::bit_cast<float>(preferred_rate.load());
            const std::int64_t period = std::llround(1e9 / (rate > 0 && rate < 120 ? static_cast<double>(rate) : 120.0));
            tick += period;
            if (const std::int64_t left = tick - now(); left > 0) {
                std::this_thread::sleep_for(std::chrono::nanoseconds(left));
            } else {
                tick = now();
            }
            if (!paused) {
                apkrt_vsync_post(tick, tick + period);
                ++ticks;
            }
        }
    }
};

/// The lines of `text` that begin with `prefix`.
std::string lines_with(const std::string &text, const std::string &prefix) {
    std::istringstream input(text);
    std::string result;
    for (std::string line; std::getline(input, line);) {
        if (line.starts_with(prefix)) result += line + "\n";
    }
    return result;
}

std::vector<std::byte> read_file(const fs::path &path) {
    std::ifstream file(path, std::ios::binary);
    std::vector<std::byte> bytes(fs::file_size(path));
    file.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return bytes;
}

std::string read_text(const fs::path &path) {
    std::ifstream file(path);
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

/// The ELF file `elf` as <name>.dylib in `directory`, a container signed ad hoc, as apk-convert writes it.
void convert(const fs::path &elf, const fs::path &directory, const std::string &name) {
    auto container = apk::container::convert(read_file(elf), {.install_name = "@rpath/" + name + ".dylib"}).bytes;
    apk::codesign::sign(container, {.identifier = name});
    fs::create_directories(directory);
    std::ofstream file(directory / (name + ".dylib"), std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char *>(container.data()), static_cast<std::streamsize>(container.size()));
}

/// The package of the app's manifest (package="...").
std::string package_of(const std::string &manifest) {
    const auto start = manifest.find("package=\"");
    if (start == std::string::npos) return "";
    const auto end = manifest.find('"', start + 9);
    return manifest.substr(start + 9, end - start - 9);
}

/// The value of the manifest's <meta-data android:name="`name`" android:value="..."/>, empty without one.
std::string meta_data_of(const std::string &manifest, const std::string &name) {
    const auto at = manifest.find("android:name=\"" + name + "\"");
    if (at == std::string::npos) return "";
    const auto value = manifest.find("android:value=\"", at);
    if (value == std::string::npos || value > manifest.find('>', at)) return "";
    const auto start = value + 15;
    return manifest.substr(start, manifest.find('"', start) - start);
}

/// Posts one event; its serial number, 0 when the post failed.
std::uint32_t post(apkrt_event_type type, std::uint64_t a0 = 0, std::uint64_t a1 = 0, std::uint64_t a2 = 0) {
    const std::uint32_t serial = apkrt_session_post(type, a0, a1, a2);
    if (serial == 0) fail("the post of event " + std::to_string(type) + " failed");
    return serial;
}

/// Milliseconds until the answer to `serial`, -1 without one within `seconds`.
double answered(std::uint32_t serial, double seconds) {
    const auto start = std::chrono::steady_clock::now();
    if (serial == 0 || apkrt_session_wait(serial, seconds) != 1) return -1;
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

void check_answer(double milliseconds, const std::string &what) {
    if (milliseconds < 0 || milliseconds >= 500) fail(what + " was not answered within 500 ms");
}

std::uint64_t pair(std::int32_t low, std::int32_t high) {
    return static_cast<std::uint32_t>(low) | static_cast<std::uint64_t>(static_cast<std::uint32_t>(high)) << 32;
}

/// The App's window: the view's layer, graphics/host's in every build, its bounds in points, its size in pixels.
/// Opaque unless the runtime reported otherwise, as the App's view makes it by Android's rule (V5's late frames).
std::uint64_t add_window() {
    @autoreleasepool {
        MinatoMetalLayer *layer = [MinatoMetalLayer layer];
        layer.opaque = window_opaque;
        layer.contentsScale = scale;
        layer.bounds = CGRectMake(0, 0, width / scale, height / scale);
        last_layer = layer;
        return apkrt_window_add((__bridge void *)layer, width, height);
    }
}

/// onNativeWindowCreated and what SessionBridge posts after it: the resize to the same size, the content's
/// rectangle when it changed (with the first window), redraw needed.
void window_created(std::uint64_t window, bool first) {
    post(APKRT_EVENT_NATIVE_WINDOW_CREATED, window, width, height);
    post(APKRT_EVENT_NATIVE_WINDOW_RESIZED, window, width, height);
    if (first) post(APKRT_EVENT_CONTENT_RECT_CHANGED, pair(0, 0), pair(width, height));
    post(APKRT_EVENT_NATIVE_WINDOW_REDRAW_NEEDED, window);
}

/// The focus gained, as SessionBridge gives it: the post, then the gate opened, and with it the frame clock
/// (`clock`, the live variants'). Milliseconds until activity-host answered, -1 without an answer in `seconds`.
double focus_gained(std::uint64_t window, Vsync *clock, double seconds) {
    const std::uint32_t serial = post(APKRT_EVENT_WINDOW_FOCUS_CHANGED, 1);
    apkrt_window_set_gate(window, 1);
    if (clock != nullptr) clock->paused = false;
    return answered(serial, seconds);
}

/// The focus lost: the post, then the gate closed, and with it the frame clock.
void focus_lost(std::uint64_t window, Vsync *clock) {
    post(APKRT_EVENT_WINDOW_FOCUS_CHANGED, 0);
    apkrt_window_set_gate(window, 0);
    if (clock != nullptr) clock->paused = true;
}

/// The start, as SessionBridge posts it: the window added first, the focus last. False when activity-host did
/// not answer the focus within 10 s.
bool start(std::uint64_t window, Vsync *clock) {
    post(APKRT_EVENT_CREATE);
    post(APKRT_EVENT_START);
    post(APKRT_EVENT_RESUME);
    post(APKRT_EVENT_INPUT_QUEUE_CREATED, 0);
    window_created(window, true);
    if (focus_gained(window, clock, 10) < 0) {
        fail("no answer for the start; the requests: " + requests_text());
        return false;
    }
    return true;
}

/// Whether the manifest's activity handles `change` itself (android:configChanges, its values joined by '|').
bool handles(const std::string &manifest, const std::string &change) {
    const auto at = manifest.find("android:configChanges=\"");
    if (at == std::string::npos) return false;
    const auto start = at + 23;
    std::istringstream values(manifest.substr(start, manifest.find('"', start) - start));
    for (std::string value; std::getline(values, value, '|');) {
        if (value == change) return true;
    }
    return false;
}

/// A turn of `window` on `layer` to `new_width` x `new_height` pixels as the App plays it (SessionBridge, ADR-0038
/// c): the layer laid out in its new bounds, onConfigurationChanged, the window's new size, then the window's events
/// with the whole window as the content's rectangle. Milliseconds until activity-host answered the redraw, -1
/// without an answer in 2 s.
double turn(std::uint64_t window, MinatoMetalLayer *layer, std::int32_t new_width, std::int32_t new_height) {
    layer.bounds = CGRectMake(0, 0, new_width / scale, new_height / scale);
    post(APKRT_EVENT_CONFIGURATION_CHANGED);
    if (apkrt_window_set_size(window, new_width, new_height) != 0) fail("apkrt_window_set_size refused a turn");
    post(APKRT_EVENT_NATIVE_WINDOW_RESIZED, window, static_cast<std::uint64_t>(new_width),
         static_cast<std::uint64_t>(new_height));
    post(APKRT_EVENT_CONTENT_RECT_CHANGED, pair(0, 0), pair(new_width, new_height));
    return answered(post(APKRT_EVENT_NATIVE_WINDOW_REDRAW_NEEDED, window), 2);
}

/// What the two turns, to landscape and back, gave the live variant in its device lines: vulkan-live a present
/// that answered VK_SUBOPTIMAL_KHR and then a swapchain at the new size, once per turn; gles-live its surface at
/// the new size within 2 frames of APP_CMD_WINDOW_RESIZED with that size.
void check_turns(const std::string &app, const std::string &device_lines) {
    const std::string sizes[2] = {std::to_string(height) + " x " + std::to_string(width),
                                  std::to_string(width) + " x " + std::to_string(height)};
    const std::regex resized(R"(frame ([0-9]+): window resized to ([0-9]+ x [0-9]+)$)");
    const std::regex surface(R"(frame ([0-9]+): surface ([0-9]+ x [0-9]+)$)");
    std::istringstream lines(device_lines);
    int turns = 0, suboptimal = 0, surfaces = 0;
    long resized_at[2] = {-1, -1}, surface_at[2] = {-1, -1};
    bool waiting = false;  // vulkan-live: a VK_SUBOPTIMAL_KHR whose new swapchain has not come yet
    for (std::string line; std::getline(lines, line);) {
        std::smatch found;
        if (line.find("vkQueuePresentKHR answered VK_SUBOPTIMAL_KHR") != std::string::npos) {
            ++suboptimal;
            waiting = true;
        } else if (waiting && turns < 2 && line.find("window 1: swapchain " + sizes[turns] + ", ") != std::string::npos) {
            ++turns;
            waiting = false;
        } else if (std::regex_search(line, found, resized) && found[2] == sizes[0] && resized_at[0] < 0) {
            resized_at[0] = std::stol(found[1]);
        } else if (std::regex_search(line, found, resized) && found[2] == sizes[1] && resized_at[0] >= 0 &&
                   resized_at[1] < 0) {
            resized_at[1] = std::stol(found[1]);
        } else if (std::regex_search(line, found, surface)) {
            if (surfaces < 2 && found[2] == sizes[surfaces]) surface_at[surfaces] = std::stol(found[1]);
            ++surfaces;
        }
    }
    const char *names[2] = {"the turn to landscape", "the turn back"};
    if (app == "vulkan-live") {
        if (suboptimal != 2 || turns != 2) {
            fail("the turns gave " + std::to_string(suboptimal) + " presents with VK_SUBOPTIMAL_KHR and " +
                 std::to_string(turns) + " new swapchains at the new size, not 2 and 2");
        }
        return;
    }
    if (surfaces != 2) fail("the surface changed its size " + std::to_string(surfaces) + " times, not 2");
    for (int index = 0; index < 2; ++index) {
        if (resized_at[index] < 0 || surface_at[index] < 0 || surface_at[index] > resized_at[index] + 2) {
            fail(std::string(names[index]) + ": APP_CMD_WINDOW_RESIZED at frame " + std::to_string(resized_at[index]) +
                 ", the surface at " + sizes[index] + " at frame " + std::to_string(surface_at[index]));
        }
    }
}

/// gles-test and vulkan-test: their scenes and frames, then their own ANativeActivity_finish and the App's quit.
void play_scenes(const std::string &app, const fs::path &expected, std::uint64_t presents, bool translucent,
                 Capture &capture) {
    const std::uint64_t window = add_window();
    if (!start(window, nullptr)) return;
    // The app's own ANativeActivity_finish ends its scenes; a translucent start reports WINDOW_OPAQUE before it.
    // wait_for holds seen.lock while it asks, so the predicate looks without requested(), which would take it again.
    const auto finished = [] {
        return std::find(seen.requests.begin(), seen.requests.end(), "FINISH") != seen.requests.end();
    };
    if (!wait_for(finished, 120s)) fail("no FINISH within 120 s: " + requests_text());
    const std::uint64_t counted = apkrt_window_presents(window);
    if (vulkan) {
        // ANGLE's Vulkan surface draws into a CAMetalLayer of its own on the window's layer (WindowSurfaceVkMac), whose
        // drawables keep the size of the apps' buffers, 256 x 256, as patch 0008 keeps the swapchain's extent.
        CAMetalLayer *drawn = nil;
        for (CALayer *sublayer in last_layer.sublayers) {
            if ([sublayer isKindOfClass:[CAMetalLayer class]]) drawn = (CAMetalLayer *)sublayer;
        }
        const CGSize size = drawn != nil ? drawn.drawableSize : CGSizeZero;
        ::dprintf(report, "host: ANGLE's Vulkan layer: drawables of %.0f x %.0f\n", size.width, size.height);
        if (size.width != 256 || size.height != 256) {
            fail("ANGLE's Vulkan layer has drawables of " + std::to_string(static_cast<int>(size.width)) + " x " +
                 std::to_string(static_cast<int>(size.height)) + ", not the app's 256 x 256");
        }
        // ANGLE's instance runs MoltenVK without Metal argument buffers (angle_vulkan.m), so MoltenVK encodes none of
        // the barrier fences that hold each render encoder's stages behind the encoders before it: those came at 8 a
        // render encoder, several a present. What waits then is the app's own sync (a fence or a query, gles-test's
        // scene 12), fewer than one a present.
        const int fence_waits = minato_app_test_fence_waits();
        ::dprintf(report, "host: render encoder fence waits encoded: %d\n", fence_waits);
        if (static_cast<std::uint64_t>(fence_waits) >= counted) {
            fail("MoltenVK encoded " + std::to_string(fence_waits) + " render encoder fence waits for " +
                 std::to_string(counted) + " presents on ANGLE's way");
        }
    }
    focus_lost(window, nullptr);
    apkrt_window_remove(window);
    post(APKRT_EVENT_PAUSE);
    const double destroyed = answered(post(APKRT_EVENT_NATIVE_WINDOW_DESTROYED, window), 2);
    post(APKRT_EVENT_STOP);
    const double destroy = answered(post(APKRT_EVENT_DESTROY), 2);
    wait_for([] { return seen.ended; }, 10s);

    const std::string log = capture.drain();
    ::dprintf(report, "host: presents counted: %llu\n", static_cast<unsigned long long>(counted));
    ::dprintf(report, "host: onNativeWindowDestroyed answered in %.1f ms, onDestroy in %.1f ms\n", destroyed, destroy);
    if (counted != presents) {
        fail("the window counted " + std::to_string(counted) + " presents, not " + std::to_string(presents));
    }
    check_answer(destroyed, "onNativeWindowDestroyed");
    check_answer(destroy, "onDestroy");
    const std::string wanted = translucent ? "[WINDOW_OPAQUE 0][FINISH]" : "[FINISH]";
    if (const std::string requests = requests_text(); requests != wanted) fail("the requests were " + requests);
    // The report reaches the layer through the main queue, which the waits above served.
    ::dprintf(report, "host: the window's layer at the end: %s\n", last_layer.opaque ? "opaque" : "not opaque");
    if (last_layer.opaque == translucent) {
        fail(std::string("the window's layer is ") + (translucent ? "still opaque" : "not opaque"));
    }
    const std::string app_lines = lines_with(log, "I/" + app + ": ");
    if (app_lines != read_text(expected / (app + ".logcat"))) {
        fail("the app's lines differ from Android 15's:\n" + app_lines);
    }
    ::dprintf(report, "%s", lines_with(log, "I/" + app + "-device: ").c_str());
}

/// A thread of the host that sleeps 1 ms at a time and notes each wakeup later than 30 ms after the one before, on
/// CLOCK_UPTIME_RAW (the guest's CLOCK_MONOTONIC): a gap it saw as well is the machine's, which gave its CPU to other
/// processes, not the app's; a wait inside the app leaves it on time.
struct Canary {
    std::atomic<bool> running{false};
    std::thread thread;
    std::mutex mutex;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> gaps;  // [start, end), ns

    void start() {
        running = true;
        thread = std::thread([this] {
            std::uint64_t last = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
            while (running) {
                ::usleep(1000);
                const std::uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
                if (now - last > 30'000'000) {
                    const std::lock_guard<std::mutex> lock(mutex);
                    gaps.emplace_back(last, now);
                }
                last = now;
            }
        });
    }

    /// The longest of its gaps that overlaps [start, end), in ms; 0 for none.
    double longest(std::uint64_t start, std::uint64_t end) {
        const std::lock_guard<std::mutex> lock(mutex);
        std::uint64_t longest = 0;
        for (const auto &gap : gaps) {
            if (gap.first < end && gap.second > start) longest = std::max(longest, gap.second - gap.first);
        }
        return static_cast<double>(longest) / 1e6;
    }

    ~Canary() {
        running = false;
        if (thread.joinable()) thread.join();
    }
};

/// gles-live and vulkan-live: the start, two turns, HOME, the return with a new window and Quit.
void play_live(const std::string &app, Capture &capture) {
    Vsync vsync;
    vsync.start();
    Canary canary;
    canary.start();
    const std::uint64_t first = add_window();
    MinatoMetalLayer *first_layer = last_layer;
    if (!start(first, &vsync)) {
        vsync.stop();
        return;
    }
    std::this_thread::sleep_for(2s);
    const double turned = turn(first, first_layer, height, width);
    std::this_thread::sleep_for(2s);
    const double turned_back = turn(first, first_layer, width, height);
    std::this_thread::sleep_for(2s);

    // HOME: the scene inactive, and after the home gesture's animation in the background, a few hundred ms later
    // on iOS [guess]. Inside didEnterBackground the window goes and the teardown is answered while the gates are
    // still open, so the frame gles-live was still drawing as the focus went commits in time; only then the
    // background flag and "scheduled" (ADR-0032 decision 6 and ADR-0029 decision 15, 2026-10-27). With the flag
    // first, that frame's commit waited at the gates until the foreground, and onNativeWindowDestroyed with it.
    focus_lost(first, &vsync);
    std::this_thread::sleep_for(300ms);
    const std::uint64_t first_presents = apkrt_window_presents(first);
    apkrt_window_remove(first);
    preferred_rate = 0;  // the App drops a removed window's preference
    post(APKRT_EVENT_PAUSE);
    const double first_destroyed = answered(post(APKRT_EVENT_NATIVE_WINDOW_DESTROYED, first), 2);
    post(APKRT_EVENT_STOP);
    post(APKRT_EVENT_SAVE_INSTANCE_STATE);
    apkrt_set_background(1);
    const int scheduled = apkrt_wait_scheduled(1.0);
    minato_app_test_count_commits();
    std::this_thread::sleep_for(500ms);

    // The return: the foreground with a new window, then the scene active.
    const int background_commits = minato_app_test_commits();
    apkrt_set_background(0);
    const std::uint64_t second = add_window();
    post(APKRT_EVENT_START);
    post(APKRT_EVENT_RESUME);
    window_created(second, false);
    if (focus_gained(second, &vsync, 2) < 0) fail("no answer for the focus after the return");
    std::this_thread::sleep_for(2s);

    // Quit.
    focus_lost(second, &vsync);
    const std::uint64_t second_presents = apkrt_window_presents(second);
    apkrt_window_remove(second);
    preferred_rate = 0;
    post(APKRT_EVENT_PAUSE);
    const double second_destroyed = answered(post(APKRT_EVENT_NATIVE_WINDOW_DESTROYED, second), 2);
    post(APKRT_EVENT_STOP);
    const double destroy = answered(post(APKRT_EVENT_DESTROY), 2);
    wait_for([] { return seen.ended; }, 10s);
    vsync.stop();

    const std::string log = capture.drain();
    ::dprintf(report, "host: presents counted: %llu on the first window, %llu on the second; %d ticks\n",
              static_cast<unsigned long long>(first_presents), static_cast<unsigned long long>(second_presents),
              vsync.ticks.load());
    ::dprintf(report, "host: \"scheduled\": %d; guest commits in the background: %d\n", scheduled, background_commits);
    ::dprintf(report, "host: onNativeWindowDestroyed answered in %.1f ms and %.1f ms, onDestroy in %.1f ms\n",
              first_destroyed, second_destroyed, destroy);
    ::dprintf(report, "host: the turns' onNativeWindowRedrawNeeded answered in %.1f ms and %.1f ms\n", turned,
              turned_back);
    const std::string requests = requests_text();
    ::dprintf(report, "host: requests: %s\n", requests.c_str());
    if (first_presents == 0) fail("the first window counted no presents");
    if (second_presents == 0) fail("the second window counted no presents");
    if (scheduled != 1) fail("\"scheduled\" said no");
    if (background_commits != 0) fail("guest threads committed command buffers in the background");
    check_answer(first_destroyed, "the first onNativeWindowDestroyed");
    check_answer(second_destroyed, "the second onNativeWindowDestroyed");
    check_answer(destroy, "onDestroy");
    check_answer(turned, "the turn's onNativeWindowRedrawNeeded");
    check_answer(turned_back, "the turn back's onNativeWindowRedrawNeeded");
    if (!std::regex_match(requests, std::regex(R"((\[FRAME_RATE [0-9.]+\])+)")) ||
        requests.find("[FRAME_RATE 120]") == std::string::npos ||
        requests.find("[FRAME_RATE 60]") == std::string::npos) {
        fail("the requests were not FRAME_RATE alone, with 120 and 60");
    }
    const std::string app_lines = lines_with(log, "I/" + app + ": ");
    const std::regex destroyed_line("I/" + app + R"(: onDestroy: frames [1-9][0-9]*, windows 2, failures 0, lost [0-9]+\n)");
    if (!std::regex_search(app_lines, destroyed_line)) fail("no onDestroy line with windows 2 and failures 0");
    // Each rate's frame intervals leave the breaks out (HOME, the window's change): none reaches 100 ms, but for one
    // in which the canary saw a gap of 50 ms or more too: the machine's, not the app's.
    const std::string device_lines = lines_with(log, "I/" + app + "-device: ");
    const std::regex rate_line(R"(rate ([0-9]+): [0-9]+ frame intervals, median [0-9.]+ ms, p99 [0-9.]+ ms, )"
                               R"(max ([0-9.]+) ms(, ending at ([0-9]+) ns)?)");
    int rates = 0;
    for (std::sregex_iterator line(device_lines.begin(), device_lines.end(), rate_line), end; line != end; ++line) {
        ++rates;
        const double longest = std::stod((*line)[2].str());
        const std::uint64_t at = (*line)[4].matched ? std::stoull((*line)[4].str()) : 0;
        const auto span = static_cast<std::uint64_t>(longest * 1e6);
        const double gap = at > span ? canary.longest(at - span, at) : 0;
        ::dprintf(report, "host: rate %s: max %.2f ms, the canary's longest gap in it %.1f ms%s\n",
                  (*line)[1].str().c_str(), longest, gap,
                  longest >= 100 && gap >= 50 ? ": the machine's, not counted" : "");
        if (longest >= 100 && gap < 50) {
            fail("a frame interval spans a break: " + line->str() + ", the canary's longest gap in it " +
                 std::to_string(static_cast<int>(gap)) + " ms");
        }
    }
    if (rates < 2) fail("the frame intervals of fewer than two rates");
    check_turns(app, device_lines);
    ::dprintf(report, "%s", device_lines.c_str());
}

/// present.csv of present-gles or present-vulkan: a row per frame in order, 600 at 60 and then 600 at 120 frames a
/// second, each rate in blocks of 200 at the intervals 1, 2 and 3, the frame times rising, each time asked for its
/// interval times the rate's period after its frame time, the period within 1 % of the clock's, and each frame's
/// timeline 0 (ADR-0041 decision 11) a whole number of periods after its frame time, within 1 % of a period. A past
/// timing that came gives the frame the time it asked for back as its desiredPresentTime, as Android's do, though
/// graphics/host handed MoltenVK another (decision 11).
void check_present_csv(const fs::path &path) {
    std::ifstream file(path);
    std::string line;
    if (!std::getline(file, line) ||
        line != "api,rate,interval,frame,frame_time_ns,asked_ns,timeline0_ns,present_id,desired_ns,actual_ns,"
                "earliest_ns,margin_ns") {
        fail("present.csv lacks its header");
        return;
    }
    int rows = 0;
    long long previous = 0, period = 0;
    for (; std::getline(file, line); ++rows) {
        std::vector<std::string> fields;
        std::istringstream values(line);
        for (std::string value; std::getline(values, value, ',');) fields.push_back(value);
        const int rate = rows < 600 ? 60 : 120, interval = 1 + rows % 600 / 200;
        if (fields.size() != 12 || fields[1] != std::to_string(rate) || fields[2] != std::to_string(interval) ||
            fields[3] != std::to_string(rows)) {
            fail("present.csv row " + std::to_string(rows) + " is " + line);
            return;
        }
        const long long frame_time = std::stoll(fields[4]), asked = std::stoll(fields[5]),
                        timeline0 = std::stoll(fields[6]);
        if (rows % 600 == 0) period = (asked - frame_time) / interval;
        const double periods = static_cast<double>(timeline0 - frame_time) / static_cast<double>(period);
        if (frame_time <= previous || asked - frame_time != interval * period ||
            std::abs(static_cast<double>(period) * rate / 1e9 - 1) > 0.01 || periods < 0.99 ||
            std::abs(periods - std::round(periods)) > 0.01) {
            fail("present.csv row " + std::to_string(rows) + " is " + line + " with a period of " +
                 std::to_string(period) + " ns");
            return;
        }
        if (fields[7] != "0" && std::stoll(fields[8]) != asked) {
            fail("present.csv row " + std::to_string(rows) + " is " + line + ": its past timing's desired time is not "
                 "the one it asked for");
            return;
        }
        previous = frame_time;
    }
    if (rows != 1200) fail("present.csv has " + std::to_string(rows) + " rows, not 1200");
}

/// present-gles and present-vulkan (ADR-0041 test 3): the start with the frame clock, the app's frames at 60 and then
/// 120 frames a second until its own ANativeActivity_finish, then the App's quit.
void play_present(const std::string &app, const fs::path &expected, const fs::path &data, Capture &capture) {
    Vsync vsync;
    vsync.start();
    const std::uint64_t window = add_window();
    if (!start(window, &vsync)) {
        vsync.stop();
        return;
    }
    if (!wait_for([] { return std::find(seen.requests.begin(), seen.requests.end(), "FINISH") != seen.requests.end(); },
                  90s)) {
        fail("no FINISH within 90 s");
    }
    const std::uint64_t counted = apkrt_window_presents(window);
    focus_lost(window, &vsync);
    apkrt_window_remove(window);
    preferred_rate = 0;
    post(APKRT_EVENT_PAUSE);
    const double destroyed = answered(post(APKRT_EVENT_NATIVE_WINDOW_DESTROYED, window), 2);
    post(APKRT_EVENT_STOP);
    const double destroy = answered(post(APKRT_EVENT_DESTROY), 2);
    wait_for([] { return seen.ended; }, 10s);
    vsync.stop();

    const std::string log = capture.drain();
    const std::string requests = requests_text();
    ::dprintf(report, "host: presents counted: %llu; %d ticks; requests: %s\n",
              static_cast<unsigned long long>(counted), vsync.ticks.load(), requests.c_str());
    ::dprintf(report, "host: onNativeWindowDestroyed answered in %.1f ms, onDestroy in %.1f ms\n", destroyed, destroy);
    if (counted != 1200) fail("the window counted " + std::to_string(counted) + " presents, not 1200");
    check_answer(destroyed, "onNativeWindowDestroyed");
    check_answer(destroy, "onDestroy");
    if (requests != "[FRAME_RATE 60][FRAME_RATE 120][FINISH]") fail("the requests were " + requests);
    const std::string app_lines = lines_with(log, "I/" + app + ": ");
    if (app_lines != read_text(expected / (app + ".logcat"))) {
        fail("the app's lines differ from Android 15's:\n" + app_lines);
    }
    check_present_csv(data / "files" / "present.csv");
    ::dprintf(report, "%s", lines_with(log, "I/" + app + "-device: ").c_str());
}

/// present-limit-gles and present-limit-vulkan: the start without a frame clock, the app's unpaced presents until it
/// finishes, the quit; its presents a second between `low` and `high`.
void play_limit(const std::string &app, Capture &capture, long low, long high) {
    const std::uint64_t window = add_window();
    if (!start(window, nullptr)) return;
    const auto finished = [] {
        return std::find(seen.requests.begin(), seen.requests.end(), "FINISH") != seen.requests.end();
    };
    if (!wait_for(finished, 60s)) fail("no FINISH within 60 s: " + requests_text());
    const std::uint64_t counted = apkrt_window_presents(window);
    focus_lost(window, nullptr);
    apkrt_window_remove(window);
    post(APKRT_EVENT_PAUSE);
    const double destroyed = answered(post(APKRT_EVENT_NATIVE_WINDOW_DESTROYED, window), 2);
    post(APKRT_EVENT_STOP);
    const double destroy = answered(post(APKRT_EVENT_DESTROY), 2);
    wait_for([] { return seen.ended; }, 10s);

    const std::string log = capture.drain();
    ::dprintf(report, "host: presents counted: %llu\n", static_cast<unsigned long long>(counted));
    ::dprintf(report, "host: onNativeWindowDestroyed answered in %.1f ms, onDestroy in %.1f ms\n", destroyed, destroy);
    check_answer(destroyed, "onNativeWindowDestroyed");
    check_answer(destroy, "onDestroy");
    if (const std::string requests = requests_text(); requests != "[FINISH]") fail("the requests were " + requests);
    ::dprintf(report, "%s", lines_with(log, "I/" + app + ": ").c_str());
    const std::string device = lines_with(log, "I/" + app + "-device: ");
    ::dprintf(report, "%s", device.c_str());
    const std::string key = "presents a second: ";
    const auto at = device.find(key);
    if (at == std::string::npos) {
        fail("no presents a second in the app's device line");
        return;
    }
    const long rate = std::strtol(device.c_str() + at + key.size(), nullptr, 10);
    if (rate < low || rate > high) {
        fail(std::to_string(rate) + " presents a second, not " + std::to_string(low) + " to " + std::to_string(high));
    }
}

/// Waits until the window counted `count` presents or the app asked to finish: true for the presents. `limit`
/// counts this thread's rounds (patience.h).
bool presented(std::uint64_t window, std::uint64_t count, std::chrono::milliseconds limit) {
    Patience patience(limit);
    do {
        if (apkrt_window_presents(window) >= count) return true;
        if (requested("FINISH")) return false;
        std::this_thread::sleep_for(100us);
    } while (patience.more(100us));
    return apkrt_window_presents(window) >= count;
}

/// bench-gles and bench-vulkan: the start, the frames in lockstep with the ticks until the app finishes, the quit.
/// With `record`, the layer records the run.
void play_bench(const std::string &app, Capture &capture, bool record, bool capped, std::uint64_t every) {
    const std::uint64_t frames = record ? 120 : 1080 / every;  // 1 s or 9 s at 120 Hz
    // Without the recording or a cap, bench.conf's check: frame 1000 drawn and presented once before the first frame
    // too.
    const std::uint64_t extra = record || capped ? 0 : 1;
    constexpr double period = 1e9 / 120;
    const std::uint64_t window = add_window();
    MinatoMetalLayer *layer = last_layer;
    if (record) {
        // A drawable before the recording, which it must not hold; room for twice the frames, so that extra
        // drawables show in the count.
        @autoreleasepool {
            layer.device = MTLCreateSystemDefaultDevice();
            (void)[layer nextDrawable];
        }
        if (![layer startRecording:2 * frames]) fail("the layer did not start recording");
    }
    std::vector<std::int64_t> posts;
    if (!start(window, nullptr)) return;
    // The scene asks for its rate once it can draw, and posts its first callback right after.
    if (!wait_for([] { return std::find(seen.requests.begin(), seen.requests.end(), "FRAME_RATE 120") !=
                              seen.requests.end(); },
                  30s)) {
        fail("no FRAME_RATE 120 within 30 s; the requests: " + requests_text());
    }
    std::this_thread::sleep_for(50ms);
    std::int64_t first = 0;
    for (int attempt = 0; attempt < 3 && apkrt_window_presents(window) == 0 && !requested("FINISH"); ++attempt) {
        first = static_cast<std::int64_t>(clock_gettime_nsec_np(CLOCK_UPTIME_RAW));
        if (posts.empty()) posts.push_back(first);
        apkrt_vsync_post(first, first + std::llround(period));
        presented(window, 1 + extra, 10000ms);
    }
    std::uint64_t ticks = 1;
    if (apkrt_window_presents(window) == 0) {
        fail("no present for the first tick");
    } else {
        for (;; ++ticks) {
            const std::int64_t late = ticks % 2 == 0 ? 1000000 : 500000;
            const std::int64_t tick = first + std::llround(static_cast<double>(ticks) * period) + late;
            posts.push_back(static_cast<std::int64_t>(clock_gettime_nsec_np(CLOCK_UPTIME_RAW)));
            apkrt_vsync_post(tick, tick + std::llround(period));
            if (ticks % every != 0) {
                // A tick the capped engine passes over: no present to wait for. The app may not have asked for the
                // next tick yet, so this one can be lost, which the limiter does not mind.
                std::this_thread::sleep_for(2ms);
                if (requested("FINISH")) break;
                continue;
            }
            if (!capped) {
                if (!presented(window, ticks / every + 1 + extra, 5000ms)) break;
                continue;
            }
            // A tick only reaches an app that asked for one (apkrt_vsync_post): the tick it draws on comes again
            // every 20 ms until its present. The limiter passes over a second one at the same time while the
            // engine's interval exceeds half a period, a held rate below 240 on 120 Hz, as every tested cap.
            bool shown = false;
            for (int attempt = 0; attempt < 250 && !shown && !requested("FINISH"); ++attempt) {
                if (attempt > 0) apkrt_vsync_post(tick, tick + std::llround(period));
                shown = presented(window, ticks / every + 1, 20ms);
            }
            if (!shown) break;
        }
    }
    if (!wait_for([] { return std::find(seen.requests.begin(), seen.requests.end(), "FINISH") != seen.requests.end(); },
                  10s)) {
        fail("the app did not finish by itself");
    }
    MinatoLayerRecording *recording = record ? [layer stopRecording:2] : nil;

    // The App's quit.
    focus_lost(window, nullptr);
    const std::uint64_t counted = apkrt_window_presents(window);
    apkrt_window_remove(window);
    post(APKRT_EVENT_PAUSE);
    const double destroyed = answered(post(APKRT_EVENT_NATIVE_WINDOW_DESTROYED, window), 2);
    post(APKRT_EVENT_STOP);
    const double destroy = answered(post(APKRT_EVENT_DESTROY), 2);
    wait_for([] { return seen.ended; }, 10s);
    if (record) {
        @autoreleasepool {
            (void)[layer nextDrawable];  // after the recording: not in it
        }
    }

    capture.drain();
    ::dprintf(report, "host: %llu ticks after the first, %llu presents counted\n",
              static_cast<unsigned long long>(ticks), static_cast<unsigned long long>(counted));
    ::dprintf(report, "host: onNativeWindowDestroyed answered in %.1f ms, onDestroy in %.1f ms\n", destroyed, destroy);
    if (counted != frames + extra) {
        fail("the window counted " + std::to_string(counted) + " presents, not " + std::to_string(frames + extra));
    }
    check_answer(destroyed, "onNativeWindowDestroyed");
    check_answer(destroy, "onDestroy");
    if (const std::string requests = requests_text(); requests != "[KEEP_SCREEN_ON 1][FRAME_RATE 120][FINISH]") {
        fail("the requests were " + requests);
    }
    if (record) {
        ::dprintf(report, "recorder: %s, APK: %llu drawables; %s\n", app.substr(6).c_str(),
                  static_cast<unsigned long long>(recording.count), recording_notes(recording).c_str());
        for (const auto &problem : recording_problems(recording, posts, frames)) fail("recorder: " + problem);
    }
}

}  // namespace

int main(int argc, char **argv) {
    if (argc != 11) {
        std::fprintf(stderr, "usage: graphics-app-test <scratch> <app> <fixtures build> <bionic lib> <guest system> "
                             "<front libs> <expected> <libGLESv2.dylib> <libMoltenVK.dylib> "
                             "<presents | translucent:<presents> | live | present | bench:<load> | record:<load> | "
                             "cap:<load>:<cap>[:<unlock>] | limit:<rate>:<low>:<high>>\n");
        return 2;
    }
    const std::string app = argv[2];
    const fs::path scratch = fs::path(argv[1]) / app, fixtures = argv[3], bionic = argv[4], system = argv[5],
                   front = argv[6], expected = argv[7], angle = argv[8], moltenvk = argv[9];
    const bool record = std::strncmp(argv[10], "record:", 7) == 0;
    const bool capped = std::strncmp(argv[10], "cap:", 4) == 0;
    const bool live = std::strcmp(argv[10], "live") == 0,
               bench = record || capped || std::strncmp(argv[10], "bench:", 6) == 0;
    // cap:<load>:<cap>[:<unlock>]: the engine draws every `every`th tick, the fewest whole periods of 120 Hz, within
    // half a period, that the rate it holds takes.
    std::string load_name = bench ? std::string(std::strchr(argv[10], ':') + 1) : "";
    int cap = 0, unlock = 0;
    if (capped) {
        const auto colon = load_name.find(':');
        const std::string rest = colon == std::string::npos ? "" : load_name.substr(colon + 1);
        load_name = load_name.substr(0, colon);
        cap = std::atoi(rest.c_str());
        if (const auto second = rest.find(':'); second != std::string::npos) {
            unlock = std::atoi(rest.c_str() + second + 1);
        }
    }
    const int held = unlock > 0 && (cap <= 0 || cap < unlock) ? unlock : cap;
    std::uint64_t every = 1;
    constexpr double tick_period = 1e9 / 120;
    const double rate = held <= 0 ? 30 : held;
    while (capped && static_cast<double>(every) * tick_period + tick_period / 2 < 1e9 / rate) ++every;
    const bool present = std::strcmp(argv[10], "present") == 0;
    const bool translucent = std::strncmp(argv[10], "translucent:", 12) == 0;
    // limit:<rate>:<low>:<high>: the frame rate limit and the range of the app's presents a second.
    const bool limited = std::strncmp(argv[10], "limit:", 6) == 0;
    long limit_rate = 0, limit_low = 0, limit_high = 0;
    if (limited && std::sscanf(argv[10], "limit:%ld:%ld:%ld", &limit_rate, &limit_low, &limit_high) != 3) {
        std::fprintf(stderr, "limit:<rate>:<low>:<high>, not %s\n", argv[10]);
        return 2;
    }
    const std::uint64_t presents = live || bench || present || limited ? 0
                                   : translucent         ? std::strtoull(argv[10] + 12, nullptr, 10)
                                                         : std::strtoull(argv[10], nullptr, 10);
    // tools/build-bench.sh puts the APKs at the top of its build, tools/build-fixtures.sh into apks/; the bench's
    // manifests are in <expected>/<app>/, the test apps' in fixtures/apps/<app>/.
    const fs::path apk = bench ? fixtures / (app + ".apk") : fixtures / "apks" / (app + ".apk");
    const fs::path library = fixtures / "bin" / ("lib" + app + ".so");
    const fs::path host = bionic.parent_path() / "bin" / "activity-host";
    const fs::path manifest_path = bench ? expected / app / "AndroidManifest.xml"
                                         : fs::path(expected).parent_path() / "apps" / app / "AndroidManifest.xml";
    std::vector<fs::path> needed = {apk, library, host, bionic / "libandroid.so", front / "libEGL.so",
                                    front / "libvulkan.so", angle, moltenvk, manifest_path};
    if (!live && !bench && !limited) needed.push_back(expected / (app + ".logcat"));
    for (const auto &path : needed) {
        if (!fs::exists(path)) {
            std::printf("skipped: %s is missing (tools/build-fixtures.sh, tools/build-bench.sh, tools/build-bionic.sh, "
                        "tools/build-front-libs.sh, tools/build-angle.sh, tools/fetch-moltenvk.sh, "
                        "tools/android-reference.sh --app %s)\n",
                        path.c_str(), app.c_str());
            return 0;
        }
    }
    const std::string manifest = read_text(manifest_path), package = package_of(manifest);
    if (package.empty()) {
        std::fprintf(stderr, "no package in %s\n", manifest_path.c_str());
        return 2;
    }
    std::string function = meta_data_of(manifest, "android.app.func_name");
    if (function.empty()) function = "ANativeActivity_onCreate";
    if (translucent) function = "ANativeActivity_onCreateTranslucent";
    if (live && (!handles(manifest, "orientation") || !handles(manifest, "screenSize"))) {
        fail("the live variant's activity does not handle orientation and screenSize itself (configChanges), so the "
             "App would not turn it");
    }

    // The containers, and the host folders of the app's data (fresh for each run).
    fs::remove_all(scratch);
    const fs::path system_libraries = scratch / "system-lib64", app_libraries = scratch / "app-lib";
    const fs::path data = scratch / "data", external = scratch / "external", obb = scratch / "obb";
    convert(host, scratch / "system-bin", "activity-host");
    for (const char *name : {"libc.so", "libm.so", "libdl.so", "ld-android.so", "liblog.so", "libnetd_client.so",
                             "libandroid.so", "libnativewindow.so", "libz.so"}) {
        convert(bionic / name, system_libraries, name);
    }
    for (const char *name : {"libEGL.so", "libGLESv2.so", "libGLESv3.so", "libvulkan.so"}) {
        convert(front / name, system_libraries, name);
    }
    convert(library, app_libraries, "lib" + app + ".so");
    if (capped) convert(fixtures / "bin" / "libbench-scripting.so", app_libraries, "libbench-scripting.so");
    if (unlock != 0) convert(bionic / "libminato-frame-rate.so", system_libraries, "libminato-frame-rate.so");
    for (const auto &folder : {data, external, obb}) fs::create_directories(folder);
    if (bench) {
        // bench.conf in the app's internalDataPath, /data/user/0/<package>/files.
        const std::string &load = load_name;
        fs::create_directories(data / "files");
        std::ofstream(data / "files" / "bench.conf")
            << "run=" << app.substr(6) << "-120-" << load << "-apk-00\nrate=120\nload=" << load
            << (record ? "\nseconds=1\n" : capped ? "\nseconds=9\n" : "\nseconds=9\ncheck=1000\n")
            << (capped ? "cap=" + std::to_string(cap) + "\n" : "")
            << (unlock != 0 ? "unlock=" + std::to_string(unlock) + "\n" : "");
    }

    // The guest paths the App mounts (ADR-0028, decision 14).
    const std::string installed = "/data/app/" + package;
    char error[1024] = "";
    const auto mount = [&](const std::string &guest, const fs::path &host_path) {
        if (apkrt_mount(guest.c_str(), host_path.c_str(), error, sizeof error) != 0) fail(error);
    };
    mount(installed + "/base.apk", apk);
    mount("/data/user/0/" + package, data);
    mount("/data/data/" + package, data);
    mount("/storage/emulated/0/Android/data/" + package, external);
    mount("/sdcard/Android/data/" + package, external);
    mount("/storage/emulated/0/Android/obb/" + package, obb);
    mount("/sdcard/Android/obb/" + package, obb);
    if (fs::is_directory(system)) mount("/system", system);
    if (apkrt_add_library_directory("/system/lib64", system_libraries.c_str(), error, sizeof error) != 0 ||
        apkrt_add_library_directory((installed + "/lib/arm64").c_str(), app_libraries.c_str(), error, sizeof error) !=
            0) {
        fail(error);
    }
    if (minato_graphics_set_providers(moltenvk.c_str(), angle.c_str()) != 0) fail("the runtime refused the providers");
    // ADR-0044 c): MINATO_ANGLE_BACKEND=vulkan puts the app on the ES 3.1 path, ANGLE's Vulkan backend on MoltenVK,
    // after the run-time check, as the session does for an app whose manifest requires ES 3.1.
    const char *backend = std::getenv("MINATO_ANGLE_BACKEND");
    vulkan = backend != nullptr && std::strcmp(backend, "vulkan") == 0;
    if (vulkan) {
        char reason[256] = "";
        if (minato_graphics_angle_vulkan_check(0, reason, sizeof reason) != 1) {
            fail(std::string("the run-time check refuses the ES 3.1 path: ") + reason);
        }
        if (minato_graphics_set_angle_backend(1) != 0) fail("graphics/host took no Vulkan backend for ANGLE");
    }

    Capture capture;
    if (!capture.start()) {
        fail("cannot capture stderr");
        return 1;
    }

    apkrt_session session{};
    session.end = on_end;
    session.request = on_request;
    session.configuration = {.scale = scale, .width = width, .height = height, .orientation = 1, .night = 1,
                             .refresh_rate = 120, .language = "en", .country = "US"};
    if (apkrt_session_begin(&session) != 0) fail("apkrt_session_begin");

    const std::string apk_argument = installed + "/base.apk", library_path = "LD_LIBRARY_PATH=" + installed + "/lib/arm64";
    const char *const arguments[] = {"/system/bin/activity-host", "--package", package.c_str(), "--apk",
                                     apk_argument.c_str(), "--library", app.c_str(), "--function", function.c_str(),
                                     nullptr};
    const char *const environment[] = {library_path.c_str(), nullptr};
    const fs::path container = scratch / "system-bin" / "activity-host.dylib";
    // ADR-0066 step 4: the FPS unlock before the program, as the App's session sets it.
    if (unlock != 0 && apkrt_set_frame_rate_unlock(unlock) != 0) fail("the runtime refused the unlock");
    // ADR-0078 decision 4: the frame rate limit before the program, as the App's session sets it.
    if (limited && minato_graphics_set_frame_rate_limit(static_cast<int>(limit_rate)) != 0) {
        fail("graphics/host refused the limit " + std::to_string(limit_rate));
    }
    if (apkrt_start_program(container.c_str(), arguments, environment, error, sizeof error) != 0) {
        fail(std::string("apkrt_start_program: ") + error);
        _exit(1);
    }

    if (live) {
        play_live(app, capture);
    } else if (present) {
        play_present(app, expected, data, capture);
    } else if (bench) {
        play_bench(app, capture, record, capped, every);
    } else if (limited) {
        play_limit(app, capture, limit_low, limit_high);
    } else {
        play_scenes(app, expected, presents, translucent, capture);
    }
    const bool ended = wait_for([] { return seen.ended; }, 10s);
    {
        const std::lock_guard hold(seen.lock);
        if (!ended) {
            fail("activity-host did not end");
        } else if (seen.status != 0) {
            fail("activity-host ended with status " + std::to_string(seen.status));
        }
    }
    if (vulkan) {
        const unsigned swapchains = minato_graphics_angle_vulkan_swapchains();
        ::dprintf(report, "ANGLE's Vulkan swapchains: %u\n", swapchains);
        // A window that keeps its size keeps its swapchain (patch 0008, and SUBOPTIMAL answered as SUCCESS).
        if (!live && !bench && !present && swapchains != 1) {
            fail("ANGLE made " + std::to_string(swapchains) + " swapchains for one window of one size, not 1");
        }
    }
    if (failures == 0) {
        ::dprintf(report, "PASS: %s\n", app.c_str());
    } else {
        ::dprintf(report, "%d failures\n", failures);
    }
    _exit(failures == 0 ? 0 : 1);
}
