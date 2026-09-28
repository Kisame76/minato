// ADR-0029 B2 and ADR-0030 V5 on the Mac: a scripted host that plays the App's lifecycle for one of the test
// apps of fixtures/apps (gles-test, vulkan-test) through activity-host, with graphics/host's providers of
// ANGLE and MoltenVK and a window on a CAMetalLayer the size of an iPhone 15 Pro Max's screen (430 x 932
// points at scale 3). It converts activity-host, the system's libraries, the front libs and the app's library
// into containers signed ad hoc, mounts the guest paths, begins the session, starts the app, and posts the
// events in the order the App's SessionBridge gives them: the start with its window and the focus, then the
// gate opened; after the app's ANativeActivity_finish the focus lost, the gate closed, the window removed,
// onPause, onNativeWindowDestroyed, onStop and onDestroy. It passes when the app's lines of the guest's log
// (stderr in logcat's tag format) are fixtures/expected/<app>.logcat, Android 15's, the only request was
// FINISH, the window counted <presents> presents, onNativeWindowDestroyed and onDestroy were answered within
// 500 ms, and activity-host ended with status 0. The app's device lines are printed, not compared.
//   graphics-app-test <scratch> <app> <fixtures build> <bionic lib> <guest system> <front libs> <expected>
//                     <ANGLE's libGLESv2.dylib> <libMoltenVK.dylib> <presents>
#import <Foundation/Foundation.h>
#import <QuartzCore/QuartzCore.h>

#include "APKRuntime/apkrt.h"
#include "apkconv/codesign.h"
#include "apkconv/container.h"
#include "minato_graphics.h"

#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

// An iPhone 15 Pro Max in portrait: 430 x 932 points at scale 3.
constexpr double scale = 3;
constexpr std::int32_t width = 1290, height = 2796;

int failures = 0;
int report = 2;  // the real stderr, also once descriptor 2 is the capture's pipe

void fail(const std::string &what) {
    ::dprintf(report, "FAIL: %s\n", what.c_str());
    ++failures;
}

/// What the session's handlers saw, written on guest threads.
struct Seen {
    std::mutex lock;
    std::condition_variable changed;
    std::vector<std::string> requests;
    bool ended = false;
    int status = -1;
};
Seen seen;

void on_request(void *, int type, std::uint64_t argument, const char *text) {
    std::string request = type == APKRT_REQUEST_FINISH           ? "FINISH"
                          : type == APKRT_REQUEST_KEEP_SCREEN_ON ? "KEEP_SCREEN_ON " + std::to_string(argument)
                          : type == APKRT_REQUEST_START_FAILED   ? std::string("START_FAILED ") + (text ? text : "")
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

template <typename Predicate>
bool wait_for(Predicate predicate, std::chrono::seconds limit) {
    std::unique_lock hold(seen.lock);
    return seen.changed.wait_for(hold, limit, predicate);
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
std::string package_of(const fs::path &manifest) {
    const std::string text = read_text(manifest);
    const auto start = text.find("package=\"");
    if (start == std::string::npos) return "";
    const auto end = text.find('"', start + 9);
    return text.substr(start + 9, end - start - 9);
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

std::uint64_t pair(std::int32_t low, std::int32_t high) {
    return static_cast<std::uint32_t>(low) | static_cast<std::uint64_t>(static_cast<std::uint32_t>(high)) << 32;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc != 11) {
        std::fprintf(stderr, "usage: graphics-app-test <scratch> <app> <fixtures build> <bionic lib> <guest system> "
                             "<front libs> <expected> <libGLESv2.dylib> <libMoltenVK.dylib> <presents>\n");
        return 2;
    }
    const std::string app = argv[2];
    const fs::path scratch = fs::path(argv[1]) / app, fixtures = argv[3], bionic = argv[4], system = argv[5],
                   front = argv[6], expected = argv[7], angle = argv[8], moltenvk = argv[9];
    const std::uint64_t presents = std::strtoull(argv[10], nullptr, 10);
    const fs::path apk = fixtures / "apks" / (app + ".apk"), library = fixtures / "bin" / ("lib" + app + ".so");
    const fs::path host = bionic.parent_path() / "bin" / "activity-host";
    const fs::path manifest = fs::path(expected).parent_path() / "apps" / app / "AndroidManifest.xml";
    for (const auto &needed : {apk, library, host, bionic / "libandroid.so", front / "libEGL.so", front / "libvulkan.so",
                               angle, moltenvk, expected / (app + ".logcat")}) {
        if (!fs::exists(needed)) {
            std::printf("skipped: %s is missing (tools/build-fixtures.sh, tools/build-bionic.sh, "
                        "tools/build-front-libs.sh, tools/build-angle.sh, tools/fetch-moltenvk.sh, "
                        "tools/android-reference.sh --app %s)\n",
                        needed.c_str(), app.c_str());
            return 0;
        }
    }
    const std::string package = package_of(manifest);
    if (package.empty()) {
        std::fprintf(stderr, "no package in %s\n", manifest.c_str());
        return 2;
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
    for (const auto &folder : {data, external, obb}) fs::create_directories(folder);

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

    // The App's window: the view's layer, its bounds in points, its size in pixels.
    std::uint64_t window = 0;
    @autoreleasepool {
        CAMetalLayer *layer = [CAMetalLayer layer];
        layer.contentsScale = scale;
        layer.bounds = CGRectMake(0, 0, width / scale, height / scale);
        window = apkrt_window_add((__bridge void *)layer, width, height);
    }

    const std::string apk_argument = installed + "/base.apk", library_path = "LD_LIBRARY_PATH=" + installed + "/lib/arm64";
    const char *const arguments[] = {"/system/bin/activity-host", "--package", package.c_str(), "--apk",
                                     apk_argument.c_str(), "--library", app.c_str(), "--function",
                                     "ANativeActivity_onCreate", nullptr};
    const char *const environment[] = {library_path.c_str(), nullptr};
    const fs::path container = scratch / "system-bin" / "activity-host.dylib";
    if (apkrt_start_program(container.c_str(), arguments, environment, error, sizeof error) != 0) {
        fail(std::string("apkrt_start_program: ") + error);
        _exit(1);
    }

    // The start, as SessionBridge posts it: the window added first, the focus last, then the gate opened.
    post(APKRT_EVENT_CREATE);
    post(APKRT_EVENT_START);
    post(APKRT_EVENT_RESUME);
    post(APKRT_EVENT_INPUT_QUEUE_CREATED, 0);
    post(APKRT_EVENT_NATIVE_WINDOW_CREATED, window, width, height);
    post(APKRT_EVENT_NATIVE_WINDOW_RESIZED, window, width, height);
    post(APKRT_EVENT_CONTENT_RECT_CHANGED, pair(0, 0), pair(width, height));
    post(APKRT_EVENT_NATIVE_WINDOW_REDRAW_NEEDED, window);
    if (answered(post(APKRT_EVENT_WINDOW_FOCUS_CHANGED, 1), 10) < 0) fail("no answer for the start");
    apkrt_window_set_gate(window, 1);

    // The app draws its frames, then finishes itself; the App's quit sequence follows.
    if (!wait_for([] { return !seen.requests.empty(); }, 120s)) fail("no request within 120 s");
    const std::uint64_t counted = apkrt_window_presents(window);
    post(APKRT_EVENT_WINDOW_FOCUS_CHANGED, 0);
    apkrt_window_set_gate(window, 0);
    apkrt_window_remove(window);
    post(APKRT_EVENT_PAUSE);
    const double destroyed = answered(post(APKRT_EVENT_NATIVE_WINDOW_DESTROYED, window), 2);
    post(APKRT_EVENT_STOP);
    const double destroy = answered(post(APKRT_EVENT_DESTROY), 2);
    if (!wait_for([] { return seen.ended; }, 10s)) fail("the program did not end after onDestroy");

    const std::string log = capture.drain();
    ::dprintf(report, "host: presents counted: %llu\n", static_cast<unsigned long long>(counted));
    ::dprintf(report, "host: onNativeWindowDestroyed answered in %.1f ms, onDestroy in %.1f ms\n", destroyed, destroy);
    if (counted != presents) fail("the window counted " + std::to_string(counted) + " presents, not " + argv[10]);
    if (destroyed < 0 || destroyed >= 500) fail("onNativeWindowDestroyed was not answered within 500 ms");
    if (destroy < 0 || destroy >= 500) fail("onDestroy was not answered within 500 ms");
    std::lock_guard hold(seen.lock);
    if (seen.status != 0) fail("activity-host ended with status " + std::to_string(seen.status));
    std::string requests;
    for (const auto &request : seen.requests) requests += "[" + request + "]";
    if (requests != "[FINISH]") fail("the requests were " + requests);
    const std::string app_lines = lines_with(log, "I/" + app + ": ");
    if (app_lines != read_text(expected / (app + ".logcat"))) {
        fail("the app's lines differ from Android 15's:\n" + app_lines);
    }
    ::dprintf(report, "%s", lines_with(log, "I/" + app + "-device: ").c_str());
    if (failures == 0) {
        ::dprintf(report, "PASS: %s\n", app.c_str());
    } else {
        ::dprintf(report, "%d failures\n", failures);
    }
    _exit(failures == 0 ? 0 : 1);
}
