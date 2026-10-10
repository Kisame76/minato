// apk-run: runs the program in a container dylib on the Mac, one program per process (ADR-0019).
// The tests start it once per fixture; the exit status is the program's own.
//   apk-run [--mount <guest path>=<host path>]... [--libraries <guest directory>=<host directory>]...
//           [--cwd <guest path>] [--app-environment] [--env <name>=<value>]... [--argv0 <text>]
//           [--configuration <width>x<height>@<scale>[,<language>[-<country>]]] [--lifecycle <step>[,<step>]...]
//           [--measures <file>] <container.dylib> [argument...]
// --mount puts a host directory under an Android path (ARCHITECTURE.md 4.5), --libraries makes the
// library containers of a host directory the libraries in a guest directory, e.g. /system/lib64. Both split at
// their last '=': a guest path may hold one, as Android 15's code paths do (/data/app/~~<base64>==/...), a host
// path may not. --env splits at its first, as a value may hold one (--env NAME=a=b gives NAME the value a=b).
// --cwd sets the working directory the program starts in ("/" by default). The program starts with init's
// environment (ADR-0039 decision 2) and on top of it what an Android shell sets (PATH, ANDROID_DATA,
// ANDROID_ROOT, HOME); --app-environment leaves the shell's out, as an app starts, and --env adds entries on
// top of either. --argv0 is how the program was started (/data/local/tmp/<container name> by default).
// --configuration begins a session before the program starts (ADR-0039, config-test), whose configuration the
// guest reads through session_configuration: a screen of <width> x <height> pixels at <scale>, portrait when it is
// at least as high as wide and landscape else, without night mode, at 120 Hz, in en-US unless a locale is given.
// The session's frame clock then posts a vsync tick every period of that rate (ADR-0041 a)), as the app's does
// (ADR-0036). The program's end ends the process with its status, as without a session; requests go nowhere, but for
// FINISH with --lifecycle.
// --lifecycle, with --configuration, plays the host's lifecycle as the App does in game mode (ADR-0028 decision 6,
// ADR-0032; activity-host's header lists the events), for a guest that reads the session (ADR-0043 step c1). Its
// steps follow one another, each once the one before is answered and a second has passed, as Android's reference
// scripts wait before HOME and before the return:
//   start       a window of the configuration's size (no layer), CREATE, START, RESUME, INPUT_QUEUE_CREATED without
//               a channel, the window created, resized with its size, the content's rectangle (the whole window),
//               redraw needed and the focus; the window's gate opens once these are posted, before their answers, as
//               the App's SessionBridge opens it with the focus: a Java app draws on the thread that answers them;
//               once they are answered, "apk-run: the start was answered" on stderr;
//   background  the background, "scheduled", the window, the surfaces and the requested windows removed, PAUSE,
//               the focus lost, each surface destroyed, each requested window destroyed, the window destroyed, STOP
//               and SAVE_INSTANCE_STATE;
//   foreground  the background off, a new window, START, RESUME and the window's events as at the start but the
//               rectangle, the gate opening as at the start; once they are answered, "apk-run: the return was
//               answered" on stderr.
//   tap:<x>:<y> (ADR-0045 step 7b2) a finger's tap at <x>, <y> in the window's pixels, on the session's input
//               channel: DOWN and UP at the same time, with the pressures 1 and 0, as Android's `input tap`;
//   swipe:<x1>:<y1>:<x2>:<y2>:<ms>  a finger's swipe from the first place to the second: DOWN, then a MOVE every
//               period of the session's refresh rate with two samples, the period's middle and its end, on the
//               line between the places, as UIKit hands the iPhone's coalesced touches, and UP at the second
//               place <ms> after the DOWN, as `input swipe`'s. With a touch in the list, the start's
//               INPUT_QUEUE_CREATED names an input channel (apkrt_input_add), else none, as before.
// The guest's surfaces (ADR-0043 c4) get windows of their own without a layer (headless_host.h): SURFACE_CREATED for a
// make, the gate opening outside the background; SURFACE_RESIZED for a place with another size; a remove takes the
// window away. Under graphics-run the window and each surface have layers of its window host (apk_run_layers.h),
// which follow the places and the window's opacity, as the App's do (ADR-0043 c4 (c)).
// The guest's requested windows (ADR-0041, gap 4: dialogs, popups, toasts, translucent activities, sub-windows) are
// answered in the same way: WINDOW_CREATED, WINDOW_RESIZED, and at the background and the end WINDOW_DESTROYED, a
// sub-window's before its parent's; under graphics-run each has a layer in Android 15's order of windows, which
// follows its places, alpha and dim (ADR-0041 gap 4 (c)).
// The windows' buffers for drawing by the CPU are graphics-run's under graphics-run, and headless ones under apk-run
// alone (ADR-0043 c3), one per window, as a host without a screen has: a lock gives a buffer kept for the window's
// geometry, cleared when that is new, and the post drops what was drawn, saying so once per window on stderr
// ("apk-run: the first post of window <id>"), which java_app_run.py --drawn looks for. Before a touch they mark
// each window's last frame, and before the next step, or a second after a last touch, each window there at the touch
// or made since gets a line for the corpus harness's "frame" level: "apk-run: window <id> after <step>: <n> posts
// since, <m> in all, the frame changed|the frame stayed|a new window[, removed]", where a frame changed when one
// posted after the touch differs from the last one before it.
// The guest's FINISH request then gets the quit: the window, the surfaces and the requested windows removed, PAUSE,
// the focus lost, each surface and requested window destroyed, the window destroyed, STOP and DESTROY, or only
// DESTROY after a last background, as Android
// destroys a stopped activity. The first step is
// start, and background and foreground alternate, with touches in the foreground. A step waits 120 s for its answer,
// as an app process on the interpreter (ADR-0039) may take its time to start; without one, apk-run says so and plays
// no more steps. A touch has no answer: the next step comes a second after its last event.
// --measures has the runtime write what the run measured to <file> at the program's end (apkrt_report_exit_measures).
// The guest's audio streams play on an output that plays nowhere (headless_audio.h), at 48 kHz in 10 ms bursts.
#include "APKRuntime/apkrt.h"
#include "apk_run_layers.h"
#include "frame_clock.h"
#include "headless_audio.h"
#include "headless_host.h"

#include <fcntl.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

/// Whether the build sets the windows' buffers itself: graphics-run does, graphics/host's or its window host's fake
/// ones, and defines this to say so (graphics/tests/graphics_run.c); apk-run alone has none.
extern "C" __attribute__((weak)) int apk_run_sets_window_buffers(void) { return 0; }

/// The window host's layers, which graphics-run defines (graphics/tests/window_host.m); apk-run alone has none.
extern "C" __attribute__((weak)) const apk_run_layer_hooks *apk_run_layers(void) { return nullptr; }

namespace {

int usage() {
    std::fprintf(stderr,
                 "usage: apk-run [--mount <guest path>=<host path>]... [--libraries <guest directory>=<host directory>]...\n"
                 "               [--cwd <guest path>] [--app-environment] [--env <name>=<value>]... [--argv0 <text>]\n"
                 "               [--configuration <width>x<height>@<scale>[,<language>[-<country>]]]\n"
                 "               [--lifecycle <step>[,<step>]...] [--measures <file>] <container.dylib> [argument...]\n"
                 "--mount and --libraries split at the last '=', --env at the first.\n");
    return 2;
}

/// --configuration's value as a session's configuration; false when it does not parse.
bool parse_configuration(const std::string &value, apkrt_configuration &configuration) {
    int width = 0, height = 0, used = 0;
    double scale = 0;
    if (std::sscanf(value.c_str(), "%dx%d@%lf%n", &width, &height, &scale, &used) != 3 || width <= 0 || height <= 0 ||
        !(scale > 0 && scale < 64)) {
        return false;
    }
    std::string language = "en", country = "US";
    const std::string_view rest = std::string_view(value).substr(static_cast<std::size_t>(used));
    if (!rest.empty()) {
        if (rest[0] != ',') return false;
        const auto locale = rest.substr(1);
        const auto dash = locale.find('-');
        language = std::string(locale.substr(0, dash));
        country = dash == std::string_view::npos ? "" : std::string(locale.substr(dash + 1));
        if (language.size() < 2 || language.size() >= sizeof configuration.language ||
            country.size() >= sizeof configuration.country) {
            return false;
        }
    }
    configuration = {};
    configuration.scale = scale;
    configuration.width = width;
    configuration.height = height;
    configuration.orientation = height >= width ? 1 : 2;  // ACONFIGURATION_ORIENTATION_PORT, _LAND
    configuration.night = 1;                               // ACONFIGURATION_UI_MODE_NIGHT_NO
    configuration.refresh_rate = 120;
    std::memcpy(configuration.language, language.c_str(), language.size() + 1);
    std::memcpy(configuration.country, country.c_str(), country.size() + 1);
    return true;
}

/// The session's frame clock, as the app's CADisplayLink (ADR-0036, decision 1): a tick on the uptime clock, the
/// guest's CLOCK_MONOTONIC, every period of `rate` (frame_clock.h), for good.
void run_frame_clock(std::int32_t rate) {
    frame_clock::run(
        1'000'000'000 / rate, [] { return static_cast<std::int64_t>(clock_gettime_nsec_np(CLOCK_UPTIME_RAW)); },
        [](std::int64_t wait) {
            const timespec delay{static_cast<time_t>(wait / 1'000'000'000), static_cast<long>(wait % 1'000'000'000)};
            nanosleep(&delay, nullptr);
        },
        [](std::int64_t tick, std::int64_t target) {
            apkrt_vsync_post(tick, target);
            return true;
        });
}

/// A touch step (ADR-0045 step 7b2): a tap's place, or a swipe's two places and its duration.
struct Touch {
    bool swipe = false;
    float x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    int milliseconds = 0;
};

/// `step` as tap:<x>:<y> or swipe:<x1>:<y1>:<x2>:<y2>:<ms> with finite places and <ms> above 0; false for any
/// other step.
bool parse_touch(const std::string &step, Touch &touch) {
    int used = 0;
    if (std::sscanf(step.c_str(), "tap:%f:%f%n", &touch.x1, &touch.y1, &used) == 2 &&
        used == static_cast<int>(step.size()) && std::isfinite(touch.x1) && std::isfinite(touch.y1)) {
        touch.swipe = false;
        return true;
    }
    used = 0;
    if (std::sscanf(step.c_str(), "swipe:%f:%f:%f:%f:%d%n", &touch.x1, &touch.y1, &touch.x2, &touch.y2,
                    &touch.milliseconds, &used) == 5 &&
        used == static_cast<int>(step.size()) && touch.milliseconds > 0 && std::isfinite(touch.x1) &&
        std::isfinite(touch.y1) && std::isfinite(touch.x2) && std::isfinite(touch.y2)) {
        touch.swipe = true;
        return true;
    }
    return false;
}

/// --lifecycle's steps: start first, then background and foreground in turn, and touches after the start or a
/// foreground; false for any other list.
bool parse_lifecycle(const std::string &value, std::vector<std::string> &steps) {
    std::istringstream list(value);
    std::size_t turns = 0;  // the start, the backgrounds and the foregrounds so far
    for (std::string step; std::getline(list, step, ',');) {
        Touch touch;
        if (turns % 2 == 1 && parse_touch(step, touch)) {
            steps.push_back(step);
            continue;
        }
        const char *const wanted = turns == 0 ? "start" : turns % 2 == 1 ? "background" : "foreground";
        if (step != wanted) return false;
        steps.push_back(step);
        ++turns;
    }
    return !steps.empty();
}

/// The quit's trigger: the guest's FINISH request, which the request handler hands over without blocking.
struct {
    std::mutex lock;
    std::condition_variable asked;
    bool finish = false;
} finishing;

void on_request(void *, int type, std::uint64_t argument, const char *) {
    if (type == APKRT_REQUEST_WINDOW_OPAQUE) {
        if (const auto *layers = apk_run_layers()) layers->window_opaque(argument != 0);
        return;
    }
    if (type != APKRT_REQUEST_FINISH) return;
    std::lock_guard<std::mutex> hold(finishing.lock);
    finishing.finish = true;
    finishing.asked.notify_all();
}

/// --lifecycle's headless buffers, one per window (headless_host.h). A second lock of a window's buffer answers -EBUSY
/// with a line on stderr, and each window's first post a line, which java_app_run.py's --drawn looks for.
headless::Buffers buffers;

int headless_lock(void *, std::uint64_t id, apkrt_window_buffer *buffer) {
    const int answer = buffers.lock(id, *buffer);
    if (answer == -EBUSY) {
        std::fprintf(stderr, "apk-run: window %llu's lock while another one holds its buffer\n",
                     static_cast<unsigned long long>(id));
    }
    return answer;
}

int headless_post(void *, std::uint64_t id) {
    bool first = false;
    const int answer = buffers.post(id, first);
    if (first) std::fprintf(stderr, "apk-run: the first post of window %llu\n", static_cast<unsigned long long>(id));
    return answer;
}

void headless_removed(void *, std::uint64_t id) {
    buffers.removed(id);
}

/// Posts one event; its serial number, 0 when the post failed, which apk-run reports.
std::uint32_t post(std::uint32_t type, std::uint64_t a0 = 0, std::uint64_t a1 = 0, std::uint64_t a2 = 0) {
    const std::uint32_t serial = apkrt_session_post(type, a0, a1, a2);
    if (serial == 0) std::fprintf(stderr, "apk-run: the post of event %u failed\n", type);
    return serial;
}

/// --lifecycle's surfaces (headless_host.h): windows on graphics-run's layers or none, opened, sized and removed in
/// the registry, and the events posted, from the surface handler on the guest's threads and from the lifecycle's
/// thread. A removed window's layer goes to the window host after the removal.
headless::Surfaces surfaces({
    [](const apkrt_surface_request &request) {
        const auto *layers = apk_run_layers();
        return apkrt_window_add(layers != nullptr ? layers->surface_layer(&request) : nullptr, request.width,
                                request.height);
    },
    [](std::uint64_t window) { apkrt_window_set_gate(window, 1); },
    [](std::uint64_t window, std::int32_t width, std::int32_t height) {
        apkrt_window_set_size(window, width, height);
    },
    [](std::uint64_t window) {
        void *const layer = apkrt_window_layer(window);
        apkrt_window_remove(window);
        if (const auto *layers = apk_run_layers()) layers->surface_removed(layer);
    },
    [](std::uint32_t type, std::uint64_t a0, std::uint64_t a1, std::uint64_t a2) { post(type, a0, a1, a2); },
    [](std::uint64_t, const apkrt_surface_request &request) {
        if (const auto *layers = apk_run_layers()) layers->surface_placed(&request);
    },
});

void on_surface(void *, const apkrt_surface_request *request) {
    surfaces.request(*request);
}

/// --lifecycle's requested windows (headless_host.h, ADR-0041 gap 4): windows on graphics-run's layers or none,
/// opened, sized and removed in the registry, and the events posted, as for the surfaces. A removed window's layer
/// goes to the window host after the removal.
headless::Windows windows({
    [](const apkrt_window_request &request) {
        const auto *layers = apk_run_layers();
        return apkrt_window_add(layers != nullptr ? layers->requested_layer(&request) : nullptr, request.width,
                                request.height);
    },
    [](std::uint64_t window) { apkrt_window_set_gate(window, 1); },
    [](std::uint64_t window, std::int32_t width, std::int32_t height) {
        apkrt_window_set_size(window, width, height);
    },
    [](std::uint64_t window) {
        void *const layer = apkrt_window_layer(window);
        apkrt_window_remove(window);
        if (const auto *layers = apk_run_layers()) layers->requested_removed(layer);
    },
    [](std::uint32_t type, std::uint64_t a0, std::uint64_t a1, std::uint64_t a2) { post(type, a0, a1, a2); },
    [](std::uint64_t, const apkrt_window_request &request) {
        if (const auto *layers = apk_run_layers()) layers->requested_placed(&request);
    },
});

void on_window(void *, const apkrt_window_request *request) {
    windows.request(*request);
}

/// Waits up to 120 s for the answer to `serial`, which answers the events before it too; false, after a line on
/// stderr, when it did not come.
bool answered(std::uint32_t serial, const char *what) {
    if (serial != 0 && apkrt_session_wait(serial, 120) == 1) return true;
    std::fprintf(stderr, "apk-run: no answer for %s; no more lifecycle steps\n", what);
    return false;
}

// AMOTION_EVENT_ACTION_DOWN, _UP and _MOVE, as guest_interface.h's struct apkrt_motion_event names them.
constexpr std::int32_t kActionDown = 0, kActionUp = 1, kActionMove = 2;

/// The uptime clock, the guest's CLOCK_MONOTONIC, in nanoseconds, as the frame clock's.
std::int64_t uptime() {
    return static_cast<std::int64_t>(clock_gettime_nsec_np(CLOCK_UPTIME_RAW));
}

/// Sleeps until `time` on the uptime clock.
void sleep_until(std::int64_t time) {
    for (std::int64_t wait; (wait = time - uptime()) > 0;) {
        const timespec delay{static_cast<time_t>(wait / 1'000'000'000), static_cast<long>(wait % 1'000'000'000)};
        nanosleep(&delay, nullptr);
    }
}

/// Posts one event of a finger, pointer 0, with `count` samples at `times` and `places` to input channel `input`.
void post_touch(std::uint64_t input, std::int32_t action, std::int64_t down, std::uint32_t count,
                const std::int64_t *times, const apkrt_pointer_coords *places) {
    const std::int32_t id = 0;
    if (apkrt_input_post(input, action, down, 1, &id, count, times, places) != 1) {
        std::fprintf(stderr, "apk-run: the post of touch action %d failed\n", action);
    }
}

/// A touch step on input channel `input` (ADR-0045 step 7b2): a tap as Android's `input tap` injects one, or a swipe
/// as its `input swipe`, places on the line by its lerp, with a MOVE every period of `rate` holding two samples.
void play_touch(std::uint64_t input, const Touch &touch, std::int32_t rate) {
    const std::int64_t down = uptime();
    const apkrt_pointer_coords first{touch.x1, touch.y1, 1, 0};
    post_touch(input, kActionDown, down, 1, &down, &first);
    if (!touch.swipe) {
        const apkrt_pointer_coords last{touch.x1, touch.y1, 0, 0};
        post_touch(input, kActionUp, down, 1, &down, &last);
        return;
    }
    const std::int64_t period = 1'000'000'000 / rate;
    const std::int64_t end = down + static_cast<std::int64_t>(touch.milliseconds) * 1'000'000;
    const auto place = [&](std::int64_t time) {
        const float alpha = static_cast<float>(time - down) / static_cast<float>(end - down);
        return apkrt_pointer_coords{(touch.x2 - touch.x1) * alpha + touch.x1,
                                    (touch.y2 - touch.y1) * alpha + touch.y1, 1, 0};
    };
    for (std::int64_t time = down + period; time < end; time += period) {
        sleep_until(time);
        const std::int64_t times[2] = {time - period / 2, time};
        const apkrt_pointer_coords places[2] = {place(times[0]), place(times[1])};
        post_touch(input, kActionMove, down, 2, times, places);
    }
    sleep_until(end);
    const apkrt_pointer_coords last{touch.x2, touch.y2, 0, 0};
    post_touch(input, kActionUp, down, 1, &end, &last);
}

/// --lifecycle's steps, then the quit after FINISH, on a thread of its own: the waits for answers may run on no
/// guest thread.
void run_lifecycle(std::vector<std::string> steps, std::int32_t width, std::int32_t height, std::int32_t rate) {
    std::uint64_t window = 0;
    // graphics-run's layer for the window, the same at every start and return, or none.
    const auto *layers = apk_run_layers();
    const auto add_window = [&] {
        return apkrt_window_add(layers != nullptr ? layers->window_layer(width, height) : nullptr, width, height);
    };
    // The touch steps' input channel, none without one (ADR-0045 step 7b2).
    std::uint64_t input = 0;
    // The window's events as Android 15 calls a NativeActivity's: created, resized with the same size, the content's
    // rectangle at the start, then redraw needed and the focus.
    const auto show = [&](bool first) {
        const auto pixels_wide = static_cast<std::uint64_t>(width), pixels_high = static_cast<std::uint64_t>(height);
        post(APKRT_EVENT_NATIVE_WINDOW_CREATED, window, pixels_wide, pixels_high);
        post(APKRT_EVENT_NATIVE_WINDOW_RESIZED, window, pixels_wide, pixels_high);
        if (first) post(APKRT_EVENT_CONTENT_RECT_CHANGED, headless::pair(0, 0), headless::pair(width, height));
        post(APKRT_EVENT_NATIVE_WINDOW_REDRAW_NEEDED, window);
        return post(APKRT_EVENT_WINDOW_FOCUS_CHANGED, 1);
    };
    // The window, the surfaces and the requested windows go before PAUSE; the surfaces' and the windows' events come
    // after the focus, before the window's and STOP, as Android 15 calls surfaceDestroyed from the stopped window
    // before onStop (apkrt.h's surface and window handlers).
    const auto hide = [&] {
        apkrt_window_remove(window);
        surfaces.take_away();
        windows.take_away();
        post(APKRT_EVENT_PAUSE);
        post(APKRT_EVENT_WINDOW_FOCUS_CHANGED, 0);
        surfaces.post_destroyed();
        windows.post_destroyed();
        post(APKRT_EVENT_NATIVE_WINDOW_DESTROYED, window);
        post(APKRT_EVENT_STOP);
    };
    // The touch step whose lines are due before the next step, with the headless buffers only.
    std::string touched;
    const auto report = [&] {
        if (touched.empty()) return;
        for (const auto &window : buffers.since_mark()) {
            std::fprintf(stderr, "apk-run: window %llu after %s: %llu posts since, %llu in all, %s%s\n",
                         static_cast<unsigned long long>(window.id), touched.c_str(),
                         static_cast<unsigned long long>(window.since), static_cast<unsigned long long>(window.posts),
                         window.made ? "a new window" : window.changed ? "the frame changed" : "the frame stayed",
                         window.removed ? ", removed" : "");
        }
        touched.clear();
    };
    bool stopped = false;
    for (std::size_t index = 0; index < steps.size(); ++index) {
        if (index > 0) std::this_thread::sleep_for(std::chrono::seconds(1));
        report();
        if (steps[index] == "start") {
            window = add_window();
            post(APKRT_EVENT_CREATE);
            post(APKRT_EVENT_START);
            post(APKRT_EVENT_RESUME);
            Touch touch;
            const auto touches = [&](const std::string &step) { return parse_touch(step, touch); };
            if (std::any_of(steps.begin(), steps.end(), touches)) {
                input = apkrt_input_add();
                if (input == 0) std::fprintf(stderr, "apk-run: no input channel for the touches\n");
            }
            post(APKRT_EVENT_INPUT_QUEUE_CREATED, input);
            const std::uint32_t shown = show(true);
            apkrt_window_set_gate(window, 1);
            if (!answered(shown, "the start")) return;
            std::fprintf(stderr, "apk-run: the start was answered\n");
        } else if (Touch touch; parse_touch(steps[index], touch)) {
            if (apk_run_sets_window_buffers() == 0) {
                buffers.mark();
                touched = steps[index];
            }
            play_touch(input, touch, rate);
        } else if (steps[index] == "background") {
            apkrt_set_background(1);
            surfaces.set_background(true);
            windows.set_background(true);
            if (apkrt_wait_scheduled(1.0) != 1) std::fprintf(stderr, "apk-run: \"scheduled\" did not answer yes\n");
            hide();
            if (!answered(post(APKRT_EVENT_SAVE_INSTANCE_STATE), "the background")) return;
            stopped = true;
        } else {
            apkrt_set_background(0);
            surfaces.set_background(false);
            windows.set_background(false);
            window = add_window();
            post(APKRT_EVENT_START);
            post(APKRT_EVENT_RESUME);
            const std::uint32_t shown = show(false);
            apkrt_window_set_gate(window, 1);
            if (!answered(shown, "the return")) return;
            std::fprintf(stderr, "apk-run: the return was answered\n");
            stopped = false;
        }
    }
    if (!touched.empty()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        report();
    }
    std::unique_lock<std::mutex> hold(finishing.lock);
    finishing.asked.wait(hold, [] { return finishing.finish; });
    hold.unlock();
    if (!stopped) {
        hide();
    } else {
        surfaces.take_away();  // a surface or a window made in the background
        windows.take_away();
    }
    answered(post(APKRT_EVENT_DESTROY), "onDestroy");
}

/// Splits <left>=<right> at the first equals sign, or at the last with `last`; false without one.
bool split(const std::string &value, std::string &left, std::string &right, bool last = false) {
    const auto equals = last ? value.rfind('=') : value.find('=');
    if (equals == std::string::npos) return false;
    left = value.substr(0, equals);
    right = value.substr(equals + 1);
    return true;
}

}  // namespace

int main(int argc, char **argv) {
    char error[1024] = "";
    // What an Android shell sets for a program rather than the Mac's environment, and --env's entries.
    std::vector<std::string> shell{
        "PATH=/product/bin:/apex/com.android.runtime/bin:/system/bin:/system/xbin",
        "ANDROID_DATA=/data",
        "ANDROID_ROOT=/system",
        "HOME=/",
    };
    std::vector<std::string> given;
    std::string argv0;
    apkrt_session session{};
    bool configured = false;
    std::vector<std::string> lifecycle;
    int index = 1;
    for (; index < argc && std::string_view(argv[index]).starts_with("--"); ++index) {
        const std::string_view option = argv[index];
        if (option == "--app-environment") {
            shell.clear();
            continue;
        }
        if (index + 1 >= argc) return usage();
        const std::string value = argv[++index];
        std::string left, right;
        int result = 0;
        if (option == "--mount") {
            if (!split(value, left, right, true)) return usage();
            result = apkrt_mount(left.c_str(), right.c_str(), error, sizeof error);
        } else if (option == "--libraries") {
            if (!split(value, left, right, true)) return usage();
            result = apkrt_add_library_directory(left.c_str(), right.c_str(), error, sizeof error);
        } else if (option == "--cwd") {
            result = apkrt_set_working_directory(value.c_str(), error, sizeof error);
        } else if (option == "--env") {
            if (!split(value, left, right)) return usage();
            const auto named = [&](const std::string &entry) { return entry.starts_with(left + "="); };
            std::erase_if(shell, named);
            std::erase_if(given, named);
            given.push_back(value);
        } else if (option == "--argv0") {
            argv0 = value;
        } else if (option == "--configuration") {
            if (!parse_configuration(value, session.configuration)) return usage();
            configured = true;
        } else if (option == "--lifecycle") {
            if (!parse_lifecycle(value, lifecycle)) return usage();
        } else if (option == "--measures") {
            const int measures = ::open(value.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
            if (measures < 0) {
                std::fprintf(stderr, "apk-run: cannot open %s: %s\n", value.c_str(), std::strerror(errno));
                return 2;
            }
            apkrt_report_exit_measures(measures);
        } else {
            return usage();
        }
        if (result != 0) {
            std::fprintf(stderr, "apk-run: %s\n", error);
            return 2;
        }
    }
    if (index >= argc || (!lifecycle.empty() && !configured)) return usage();

    // Without --argv0 the program sees itself under an Android path, named after its container.
    const char *const container = argv[index];
    if (argv0.empty()) argv0 = "/data/local/tmp/" + std::filesystem::path(container).stem().string();
    std::vector<const char *> arguments{argv0.c_str()};
    for (++index; index < argc; ++index) arguments.push_back(argv[index]);
    arguments.push_back(nullptr);
    std::vector<const char *> variables;
    for (const auto &entry : shell) variables.push_back(entry.c_str());
    for (const auto &entry : given) variables.push_back(entry.c_str());
    variables.push_back(nullptr);

    if (configured) {
        // The end handler runs on the guest's thread instead of the process's end: it ends the process as that would.
        session.end = [](void *, int status) { ::_exit(status); };
        if (!lifecycle.empty()) {
            session.request = on_request;
            session.surface = on_surface;
            session.window = on_window;
        }
        if (apkrt_session_begin(&session) != 0) {
            std::fprintf(stderr, "apk-run: the session did not begin\n");
            return 2;
        }
        std::thread(run_frame_clock, session.configuration.refresh_rate).detach();
        if (!lifecycle.empty() && apk_run_sets_window_buffers() == 0) {
            static const apkrt_window_buffers headless_buffers = {headless_lock, headless_post, headless_removed,
                                                                  nullptr};
            apkrt_set_window_buffers(&headless_buffers);
        }
        // Events wait in the pipe until the guest reads them, so the start may be posted before the program runs.
        if (!lifecycle.empty()) {
            std::thread(run_lifecycle, lifecycle, session.configuration.width, session.configuration.height,
                        session.configuration.refresh_rate)
                    .detach();
        }
    }
    apkrt_set_audio_host(&headless_audio::host);
    apkrt_run_program(container, arguments.data(), variables.data(), error, sizeof error);
    std::fprintf(stderr, "apk-run: %s\n", error);
    if (configured) {
        // The frame clock's thread still posts ticks: no static destructor may run under it, as the session's end
        // handler also ends without them.
        std::fflush(stderr);
        ::_exit(127);
    }
    return 127;
}
