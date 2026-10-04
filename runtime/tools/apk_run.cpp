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
//   start       a window of the configuration's size (no layer; graphics-run's buffers under graphics-run), CREATE,
//               START, RESUME, INPUT_QUEUE_CREATED without a channel, the window created, resized with its size, the
//               content's rectangle (the whole window), redraw needed and the focus; then the window's gate opens;
//   background  the background, "scheduled", the window removed, PAUSE, the focus lost, the window destroyed, STOP
//               and SAVE_INSTANCE_STATE;
//   foreground  the background off, a new window, START, RESUME and the window's events as at the start but the
//               rectangle; then the gate opens.
// The guest's FINISH request then gets the quit: the window removed, PAUSE, the focus lost, the window destroyed,
// STOP and DESTROY, or only DESTROY after a last background, as Android destroys a stopped activity. The first step is
// start, and background and foreground alternate. A step waits 120 s for its answer, as an app process on the
// interpreter (ADR-0039) may take its time to start; without one, apk-run says so and plays no more steps.
// --measures has the runtime write what the run measured to <file> at the program's end (apkrt_report_exit_measures).
#include "APKRuntime/apkrt.h"

#include <fcntl.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
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
/// guest's CLOCK_MONOTONIC, every period of `rate`, each one's target the next; a clock that falls behind skips
/// refreshes, as a display link does.
[[noreturn]] void run_frame_clock(std::int32_t rate) {
    const std::int64_t period = 1'000'000'000 / rate;
    auto tick = static_cast<std::int64_t>(clock_gettime_nsec_np(CLOCK_UPTIME_RAW));
    for (;;) {
        apkrt_vsync_post(tick, tick + period);
        const auto now = static_cast<std::int64_t>(clock_gettime_nsec_np(CLOCK_UPTIME_RAW));
        tick += ((now - tick) / period + 1) * period;
        const std::int64_t wait = tick - now;
        const timespec delay{static_cast<time_t>(wait / 1'000'000'000), static_cast<long>(wait % 1'000'000'000)};
        nanosleep(&delay, nullptr);
    }
}

/// --lifecycle's steps: start first, then background and foreground in turn; false for any other list.
bool parse_lifecycle(const std::string &value, std::vector<std::string> &steps) {
    std::istringstream list(value);
    for (std::string step; std::getline(list, step, ',');) {
        const char *const wanted = steps.empty() ? "start" : steps.size() % 2 == 1 ? "background" : "foreground";
        if (step != wanted) return false;
        steps.push_back(step);
    }
    return !steps.empty();
}

/// The quit's trigger: the guest's FINISH request, which the request handler hands over without blocking.
struct {
    std::mutex lock;
    std::condition_variable asked;
    bool finish = false;
} finishing;

void on_request(void *, int type, std::uint64_t, const char *) {
    if (type != APKRT_REQUEST_FINISH) return;
    std::lock_guard<std::mutex> hold(finishing.lock);
    finishing.finish = true;
    finishing.asked.notify_all();
}

/// Posts one event; its serial number, 0 when the post failed, which apk-run reports.
std::uint32_t post(std::uint32_t type, std::uint64_t a0 = 0, std::uint64_t a1 = 0, std::uint64_t a2 = 0) {
    const std::uint32_t serial = apkrt_session_post(type, a0, a1, a2);
    if (serial == 0) std::fprintf(stderr, "apk-run: the post of event %u failed\n", type);
    return serial;
}

/// Waits up to 120 s for the answer to `serial`, which answers the events before it too; false, after a line on
/// stderr, when it did not come.
bool answered(std::uint32_t serial, const char *what) {
    if (serial != 0 && apkrt_session_wait(serial, 120) == 1) return true;
    std::fprintf(stderr, "apk-run: no answer for %s; no more lifecycle steps\n", what);
    return false;
}

/// Two 32-bit values in one argument, `low` in the low half.
std::uint64_t pair(std::int32_t low, std::int32_t high) {
    return static_cast<std::uint32_t>(low) | static_cast<std::uint64_t>(static_cast<std::uint32_t>(high)) << 32;
}

/// --lifecycle's steps, then the quit after FINISH, on a thread of its own: the waits for answers may run on no
/// guest thread.
void run_lifecycle(std::vector<std::string> steps, std::int32_t width, std::int32_t height) {
    std::uint64_t window = 0;
    // The window's events as Android 15 calls a NativeActivity's: created, resized with the same size, the content's
    // rectangle at the start, then redraw needed and the focus.
    const auto show = [&](bool first) {
        const auto pixels_wide = static_cast<std::uint64_t>(width), pixels_high = static_cast<std::uint64_t>(height);
        post(APKRT_EVENT_NATIVE_WINDOW_CREATED, window, pixels_wide, pixels_high);
        post(APKRT_EVENT_NATIVE_WINDOW_RESIZED, window, pixels_wide, pixels_high);
        if (first) post(APKRT_EVENT_CONTENT_RECT_CHANGED, pair(0, 0), pair(width, height));
        post(APKRT_EVENT_NATIVE_WINDOW_REDRAW_NEEDED, window);
        return post(APKRT_EVENT_WINDOW_FOCUS_CHANGED, 1);
    };
    bool stopped = false;
    for (std::size_t index = 0; index < steps.size(); ++index) {
        if (index > 0) std::this_thread::sleep_for(std::chrono::seconds(1));
        if (steps[index] == "start") {
            window = apkrt_window_add(nullptr, width, height);
            post(APKRT_EVENT_CREATE);
            post(APKRT_EVENT_START);
            post(APKRT_EVENT_RESUME);
            post(APKRT_EVENT_INPUT_QUEUE_CREATED, 0);
            if (!answered(show(true), "the start")) return;
            apkrt_window_set_gate(window, 1);
        } else if (steps[index] == "background") {
            apkrt_set_background(1);
            if (apkrt_wait_scheduled(1.0) != 1) std::fprintf(stderr, "apk-run: \"scheduled\" did not answer yes\n");
            apkrt_window_remove(window);
            post(APKRT_EVENT_PAUSE);
            post(APKRT_EVENT_WINDOW_FOCUS_CHANGED, 0);
            post(APKRT_EVENT_NATIVE_WINDOW_DESTROYED, window);
            post(APKRT_EVENT_STOP);
            if (!answered(post(APKRT_EVENT_SAVE_INSTANCE_STATE), "the background")) return;
            stopped = true;
        } else {
            apkrt_set_background(0);
            window = apkrt_window_add(nullptr, width, height);
            post(APKRT_EVENT_START);
            post(APKRT_EVENT_RESUME);
            if (!answered(show(false), "the return")) return;
            apkrt_window_set_gate(window, 1);
            stopped = false;
        }
    }
    std::unique_lock<std::mutex> hold(finishing.lock);
    finishing.asked.wait(hold, [] { return finishing.finish; });
    hold.unlock();
    if (!stopped) {
        apkrt_window_remove(window);
        post(APKRT_EVENT_PAUSE);
        post(APKRT_EVENT_WINDOW_FOCUS_CHANGED, 0);
        post(APKRT_EVENT_NATIVE_WINDOW_DESTROYED, window);
        post(APKRT_EVENT_STOP);
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
        if (!lifecycle.empty()) session.request = on_request;
        if (apkrt_session_begin(&session) != 0) {
            std::fprintf(stderr, "apk-run: the session did not begin\n");
            return 2;
        }
        std::thread(run_frame_clock, session.configuration.refresh_rate).detach();
        // Events wait in the pipe until the guest reads them, so the start may be posted before the program runs.
        if (!lifecycle.empty()) {
            std::thread(run_lifecycle, lifecycle, session.configuration.width, session.configuration.height).detach();
        }
    }
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
