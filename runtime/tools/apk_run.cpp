// apk-run: runs the program in a container dylib on the Mac, one program per process (ADR-0019).
// The tests start it once per fixture; the exit status is the program's own.
//   apk-run [--mount <guest path>=<host path>]... [--libraries <guest directory>=<host directory>]...
//           [--cwd <guest path>] [--app-environment] [--env <name>=<value>]... [--argv0 <text>]
//           [--configuration <width>x<height>@<scale>[,<language>[-<country>]]]
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
// (ADR-0036). The program's end ends the process with its status, as without a session; requests go nowhere.
// --measures has the runtime write what the run measured to <file> at the program's end (apkrt_report_exit_measures).
#include "APKRuntime/apkrt.h"

#include <fcntl.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
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
                 "               [--measures <file>] <container.dylib> [argument...]\n"
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
    if (index >= argc) return usage();

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
        if (apkrt_session_begin(&session) != 0) {
            std::fprintf(stderr, "apk-run: the session did not begin\n");
            return 2;
        }
        std::thread(run_frame_clock, session.configuration.refresh_rate).detach();
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
