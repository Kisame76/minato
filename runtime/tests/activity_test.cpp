// ADR-0028's test 3 on the Mac: a scripted host that plays ADR-0032's lifecycle machine for activity-test
// (fixtures/apps/activity-test), as the App does in game mode. It converts activity-host, the system's
// libraries and the app's library into containers signed ad hoc, mounts the guest paths of decision 14,
// begins the session with a fixed configuration and its handlers, starts /system/bin/activity-host, and
// posts the events of the start, of HOME, of the return and of the quit that follows the app's
// ANativeActivity_finish, in the order Android 15 calls the callbacks (tools/android-reference.sh --app
// activity-test), with the window's steps of decision 6 around them. It passes when the app's lines of
// the guest's log (stderr, logcat's tag format) are fixtures/expected/activity-test.logcat, its device
// lines are those of the configuration given here, the requests were KEEP_SCREEN_ON and FINISH, the three
// folders exist, and activity-host ended with status 0 once onDestroy was answered. A sixth argument names
// a variant: polling starts the app at activity_test_polling_onCreate, whose onStart polls the main
// thread's looper first, and must give the same log; leftovers leaves NATIVE_WINDOW_DESTROYED out, so
// activity-host destroys the window before the next one and before onDestroy, as NativeActivity does;
// missing-library, missing-function and missing-apk start activity-host with what is not there and pass
// when it reports the reason as START_FAILED and ends with status 1.
//   activity_test <scratch directory> <fixtures bin> <bionic lib> <guest /system> <fixtures/expected> [<case>]
#include "APKRuntime/apkrt.h"
#include "apkconv/codesign.h"
#include "apkconv/container.h"

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

constexpr const char *package = "io.github.kisame76.minato.test.activity";
constexpr std::int32_t width = 1290, height = 2796;  // an iPhone 15 Pro Max in portrait, at scale 3

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

    int drains = 0;

    /// Everything written so far: a marker line of its own written now comes through after it.
    std::string drain() {
        const std::string marker = "activity_test: capture " + std::to_string(++drains) + "\n";
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

std::vector<std::byte> read_file(const fs::path &path) {
    std::ifstream file(path, std::ios::binary);
    std::vector<std::byte> bytes(fs::file_size(path));
    file.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return bytes;
}

/// The ELF file `elf` as <file name>.dylib in `directory`, a container signed ad hoc, as apk-convert writes it.
fs::path convert(const fs::path &elf, const fs::path &directory, const std::string &name) {
    auto container = apk::container::convert(read_file(elf), {.install_name = "@rpath/" + name + ".dylib"}).bytes;
    apk::codesign::sign(container, {.identifier = name});
    fs::create_directories(directory);
    const auto path = directory / (name + ".dylib");
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char *>(container.data()), static_cast<std::streamsize>(container.size()));
    return path;
}

std::string read_text(const fs::path &path) {
    std::ifstream file(path);
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

/// Posts one event; its serial number, 0 when the post failed.
std::uint32_t post(apkrt_event_type type, std::uint64_t a0 = 0, std::uint64_t a1 = 0, std::uint64_t a2 = 0) {
    const std::uint32_t serial = apkrt_session_post(type, a0, a1, a2);
    if (serial == 0) fail("the post of event " + std::to_string(type) + " failed");
    return serial;
}

/// Waits for the answer to `serial`, which answers every event before it too.
void answered(std::uint32_t serial, const char *what) {
    if (serial != 0 && apkrt_session_wait(serial, 10) != 1) fail(std::string("no answer for ") + what);
}

std::uint64_t pair(std::int32_t low, std::int32_t high) {
    return static_cast<std::uint32_t>(low) | static_cast<std::uint64_t>(static_cast<std::uint32_t>(high)) << 32;
}

/// The events of a window that the host created, as Android 15 calls them: created, then resized once
/// with the same size, then (at the first) the content's rectangle, then redraw needed and the focus.
std::uint32_t show_window(std::uint64_t window, bool first) {
    post(APKRT_EVENT_NATIVE_WINDOW_CREATED, window, width, height);
    post(APKRT_EVENT_NATIVE_WINDOW_RESIZED, window, width, height);
    // A rectangle whose four values differ, so that the device line shows their order.
    if (first) post(APKRT_EVENT_CONTENT_RECT_CHANGED, pair(6, 120), pair(width - 6, height - 36));
    post(APKRT_EVENT_NATIVE_WINDOW_REDRAW_NEEDED, window);
    return post(APKRT_EVENT_WINDOW_FOCUS_CHANGED, 1);
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
    const fs::path scratch = fs::path(argv[1]) / ("activity" + variant), fixtures = argv[2], bionic = argv[3],
                   system = argv[4], expected = argv[5];
    const fs::path apk = fixtures.parent_path() / "apks" / "activity-test.apk";
    const fs::path library = fixtures / "libactivity-test.so";
    const fs::path host = bionic.parent_path() / "bin" / "activity-host";
    for (const auto &needed : {apk, library, host, bionic / "libandroid.so"}) {
        if (!fs::exists(needed)) {
            std::printf("skipped: %s is not built (tools/build-fixtures.sh, tools/build-bionic.sh)\n", needed.c_str());
            return 0;
        }
    }

    // The containers, and the host folders of the app's data (fresh for each run).
    fs::remove_all(scratch);
    const fs::path system_libraries = scratch / "system-lib64", app_libraries = scratch / "app-lib";
    const fs::path data = scratch / "data", external = scratch / "external", obb = scratch / "obb";
    const fs::path container = convert(host, scratch / "system-bin", "activity-host");
    for (const char *name : {"libc.so", "libm.so", "libdl.so", "ld-android.so", "liblog.so", "libnetd_client.so",
                             "libandroid.so", "libnativewindow.so", "libz.so"}) {
        convert(bionic / name, system_libraries, name);
    }
    convert(library, app_libraries, "libactivity-test.so");
    for (const auto &folder : {data, external, obb}) fs::create_directories(folder);

    // The guest paths of decision 14.
    const std::string app = std::string("/data/app/") + package;
    char error[1024] = "";
    const auto mount = [&](const std::string &guest, const fs::path &host_path) {
        if (apkrt_mount(guest.c_str(), host_path.c_str(), error, sizeof error) != 0) fail(error);
    };
    mount(app + "/base.apk", apk);
    mount(std::string("/data/user/0/") + package, data);
    mount(std::string("/data/data/") + package, data);
    mount(std::string("/storage/emulated/0/Android/data/") + package, external);
    mount(std::string("/sdcard/Android/data/") + package, external);
    mount(std::string("/storage/emulated/0/Android/obb/") + package, obb);
    mount(std::string("/sdcard/Android/obb/") + package, obb);
    if (fs::is_directory(system)) mount("/system", system);
    if (apkrt_add_library_directory("/system/lib64", system_libraries.c_str(), error, sizeof error) != 0 ||
        apkrt_add_library_directory((app + "/lib/arm64").c_str(), app_libraries.c_str(), error, sizeof error) != 0) {
        fail(error);
    }

    Capture capture;
    if (!capture.start()) {
        fail("cannot capture stderr");
        return 1;
    }

    apkrt_session session{};
    session.end = on_end;
    session.request = on_request;
    session.configuration = {3.0, width, height, 1, 1, 120, "de", "DE"};
    if (apkrt_session_begin(&session) != 0) fail("apkrt_session_begin");

    const std::string apk_argument = app + (variant == "missing-apk" ? "/missing.apk" : "/base.apk");
    const std::string library_path = "LD_LIBRARY_PATH=" + app + "/lib/arm64";
    const char *const library_name = variant == "missing-library" ? "missing" : "activity-test";
    const char *const function = variant == "missing-function" ? "missing_onCreate"
                                 : variant == "polling"        ? "activity_test_polling_onCreate"
                                                               : "ANativeActivity_onCreate";
    const char *const arguments[] = {"/system/bin/activity-host", "--package", package, "--apk", apk_argument.c_str(),
                                     "--library", library_name, "--function", function, nullptr};
    const char *const environment[] = {library_path.c_str(), nullptr};
    if (apkrt_start_program(container.c_str(), arguments, environment, error, sizeof error) != 0) {
        fail(std::string("apkrt_start_program: ") + error);
        _exit(1);
    }

    // An app that cannot start: the reason reaches the host before the program ends with status 1.
    if (failing) {
        if (!wait_for([] { return seen.ended; }, 10s)) fail("activity-host did not end");
        const std::string reason = variant == "missing-library"
                                       ? "START_FAILED dlopen failed: library \"libmissing.so\" not found"
                                   : variant == "missing-function"
                                       ? "START_FAILED undefined symbol: missing_onCreate"
                                       : "START_FAILED cannot read the APK " + app + "/missing.apk";
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

    // The app finishes itself a second after its second window gained the focus; then the quit sequence,
    // whose window leaves before onPause. activity-host destroys the input queue before onDestroy.
    if (!wait_for([] { return !seen.requests.empty() && seen.requests.back() == "FINISH"; }, 10s)) {
        fail("no FINISH request");
    }
    apkrt_window_remove(second);
    post(APKRT_EVENT_PAUSE);
    post(APKRT_EVENT_WINDOW_FOCUS_CHANGED, 0);
    if (!leftovers) post(APKRT_EVENT_NATIVE_WINDOW_DESTROYED, second);
    post(APKRT_EVENT_STOP);
    answered(post(APKRT_EVENT_DESTROY), "onDestroy");
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
    const std::string reference = read_text(expected / "activity-test.logcat");
    if (app_lines != (leftovers ? without_window_destroyed(reference) : reference)) {
        fail("the app's lines differ from Android 15's:\n" + app_lines);
    }
    const std::string device = lines_with(log, "I/activity-test-device: ");
    if (device != device_lines) fail("the device lines differ:\n" + device);
    for (const auto &folder : {data / "files", external / "files", obb}) {
        if (!fs::is_directory(folder)) fail(folder.string() + " is not a folder");
    }
    ::dprintf(report, failures == 0 ? "PASS: activity-test\n" : "%d failures\n", failures);
    _exit(failures == 0 ? 0 : 1);
}
