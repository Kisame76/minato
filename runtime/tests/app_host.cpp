#include "app_host.h"

#include "apkconv/codesign.h"
#include "apkconv/container.h"

#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>

namespace app_host {

using namespace std::chrono_literals;

int failures = 0;
int report = 2;
Seen seen;
void (*on_request)(int type, std::uint64_t argument) = nullptr;

void fail(const std::string &what) {
    ::dprintf(report, "FAIL: %s\n", what.c_str());
    ++failures;
}

namespace {

/// A frame rate's bits as the rate.
std::string rate(std::uint64_t bits) {
    const auto low = static_cast<std::uint32_t>(bits);
    float value;
    std::memcpy(&value, &low, sizeof value);
    char text[32];
    std::snprintf(text, sizeof text, "%g", static_cast<double>(value));
    return text;
}

void record_request(void *, int type, std::uint64_t argument, const char *text) {
    std::string request = type == APKRT_REQUEST_FINISH           ? "FINISH"
                          : type == APKRT_REQUEST_KEEP_SCREEN_ON ? "KEEP_SCREEN_ON " + std::to_string(argument)
                          : type == APKRT_REQUEST_START_FAILED   ? std::string("START_FAILED ") + (text ? text : "")
                          : type == APKRT_REQUEST_FRAME_RATE     ? "FRAME_RATE " + rate(argument)
                                                                 : "request " + std::to_string(type);
    if (on_request != nullptr) on_request(type, argument);
    const std::lock_guard hold(seen.lock);
    seen.requests.push_back(std::move(request));
    seen.changed.notify_all();
}

/// Records the status and returns, so that the thread sleeps and the main thread checks what happened.
void record_end(void *, int status) {
    const std::lock_guard hold(seen.lock);
    seen.ended = true;
    seen.status = status;
    seen.changed.notify_all();
}

std::vector<std::byte> read_file(const fs::path &path) {
    std::ifstream file(path, std::ios::binary);
    std::vector<std::byte> bytes(fs::file_size(path));
    file.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return bytes;
}

/// The ELF file `elf` as <name>.dylib in `directory`, a container signed ad hoc, as apk-convert writes it.
fs::path convert(const fs::path &elf, const fs::path &directory, const std::string &name) {
    auto container = apk::container::convert(read_file(elf), {.install_name = "@rpath/" + name + ".dylib"}).bytes;
    apk::codesign::sign(container, {.identifier = name});
    fs::create_directories(directory);
    const auto path = directory / (name + ".dylib");
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char *>(container.data()), static_cast<std::streamsize>(container.size()));
    return path;
}

}  // namespace

bool Capture::start() {
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

std::string Capture::drain() {
    const std::string marker = "app_host: capture " + std::to_string(++drains) + "\n";
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

std::string lines_with(const std::string &text, const std::string &prefix) {
    std::istringstream input(text);
    std::string result;
    for (std::string line; std::getline(input, line);) {
        if (line.starts_with(prefix)) result += line + "\n";
    }
    return result;
}

std::string read_text(const fs::path &path) {
    std::ifstream file(path);
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

bool prepare(App &app, const Paths &paths, const std::string &run) {
    const fs::path apk = paths.fixtures.parent_path() / "apks" / (app.name + ".apk");
    const fs::path library = paths.fixtures / ("lib" + app.name + ".so");
    const fs::path host = paths.bionic.parent_path() / "bin" / "activity-host";
    for (const auto &needed : {apk, library, host, paths.bionic / "libandroid.so"}) {
        if (!fs::exists(needed)) {
            std::printf("skipped: %s is not built (tools/build-fixtures.sh, tools/build-bionic.sh)\n", needed.c_str());
            return false;
        }
    }

    // The containers, and the host folders of the app's data (fresh for each run).
    app.scratch = paths.scratch / run;
    fs::remove_all(app.scratch);
    const fs::path system_libraries = app.scratch / "system-lib64", app_libraries = app.scratch / "app-lib";
    app.data = app.scratch / "data";
    app.external = app.scratch / "external";
    app.obb = app.scratch / "obb";
    app.container = convert(host, app.scratch / "system-bin", "activity-host");
    for (const char *name : {"libc.so", "libm.so", "libdl.so", "ld-android.so", "liblog.so", "libnetd_client.so",
                             "libandroid.so", "libnativewindow.so", "libz.so"}) {
        convert(paths.bionic / name, system_libraries, name);
    }
    // The audio libraries (ADR-0036), where they are built: only the audio apps need them.
    for (const char *name : {"libaaudio.so", "libOpenSLES.so"}) {
        if (fs::exists(paths.bionic / name)) convert(paths.bionic / name, system_libraries, name);
    }
    convert(library, app_libraries, "lib" + app.name + ".so");
    for (const auto &folder : {app.data, app.external, app.obb}) fs::create_directories(folder);

    // The guest paths of decision 14.
    app.guest = "/data/app/" + app.package;
    char error[1024] = "";
    const auto mount = [&](const std::string &guest, const fs::path &host_path) {
        if (apkrt_mount(guest.c_str(), host_path.c_str(), error, sizeof error) != 0) fail(error);
    };
    mount(app.guest + "/base.apk", apk);
    mount("/data/user/0/" + app.package, app.data);
    mount("/data/data/" + app.package, app.data);
    mount("/storage/emulated/0/Android/data/" + app.package, app.external);
    mount("/sdcard/Android/data/" + app.package, app.external);
    mount("/storage/emulated/0/Android/obb/" + app.package, app.obb);
    mount("/sdcard/Android/obb/" + app.package, app.obb);
    if (fs::is_directory(paths.system)) mount("/system", paths.system);
    if (apkrt_add_library_directory("/system/lib64", system_libraries.c_str(), error, sizeof error) != 0 ||
        apkrt_add_library_directory((app.guest + "/lib/arm64").c_str(), app_libraries.c_str(), error,
                                    sizeof error) != 0) {
        fail(error);
    }
    return true;
}

void begin(const apkrt_configuration &configuration) {
    apkrt_session session{};
    session.end = record_end;
    session.request = record_request;
    session.configuration = configuration;
    if (apkrt_session_begin(&session) != 0) fail("apkrt_session_begin");
}

bool start(const App &app, const std::string &apk, const char *library, const char *function) {
    const std::string library_path = "LD_LIBRARY_PATH=" + app.guest + "/lib/arm64";
    const char *const arguments[] = {"/system/bin/activity-host", "--package", app.package.c_str(), "--apk",
                                     apk.c_str(), "--library", library, "--function", function, nullptr};
    const char *const environment[] = {library_path.c_str(), nullptr};
    char error[1024] = "";
    if (apkrt_start_program(app.container.c_str(), arguments, environment, error, sizeof error) != 0) {
        fail(std::string("apkrt_start_program: ") + error);
        return false;
    }
    return true;
}

std::uint32_t post(apkrt_event_type type, std::uint64_t a0, std::uint64_t a1, std::uint64_t a2) {
    const std::uint32_t serial = apkrt_session_post(type, a0, a1, a2);
    if (serial == 0) fail("the post of event " + std::to_string(type) + " failed");
    return serial;
}

void answered(std::uint32_t serial, const char *what) {
    if (serial != 0 && apkrt_session_wait(serial, 10) != 1) fail(std::string("no answer for ") + what);
}

std::uint64_t pair(std::int32_t low, std::int32_t high) {
    return static_cast<std::uint32_t>(low) | static_cast<std::uint64_t>(static_cast<std::uint32_t>(high)) << 32;
}

std::uint32_t show_window(std::uint64_t window, bool first) {
    post(APKRT_EVENT_NATIVE_WINDOW_CREATED, window, width, height);
    post(APKRT_EVENT_NATIVE_WINDOW_RESIZED, window, width, height);
    // A rectangle whose four values differ, so that a device line shows their order.
    if (first) post(APKRT_EVENT_CONTENT_RECT_CHANGED, pair(6, 120), pair(width - 6, height - 36));
    post(APKRT_EVENT_NATIVE_WINDOW_REDRAW_NEEDED, window);
    return post(APKRT_EVENT_WINDOW_FOCUS_CHANGED, 1);
}

void quit(std::uint64_t window, bool window_destroyed) {
    apkrt_window_remove(window);
    post(APKRT_EVENT_PAUSE);
    post(APKRT_EVENT_WINDOW_FOCUS_CHANGED, 0);
    if (window_destroyed) post(APKRT_EVENT_NATIVE_WINDOW_DESTROYED, window);
    post(APKRT_EVENT_STOP);
    answered(post(APKRT_EVENT_DESTROY), "onDestroy");
}

}  // namespace app_host
