// A scripted host for the test apps of fixtures/apps on the Mac (ADR-0028, test 3; ADR-0036), as the App plays
// it in game mode: it converts activity-host, the system's libraries and the app's library into containers
// signed ad hoc, mounts the guest paths of ADR-0028's decision 14, captures the guest's stderr (the log, in
// logcat's tag format), begins the session with a configuration and handlers that record the requests and
// the end, starts /system/bin/activity-host, and posts events. Each harness (activity_test.cpp,
// choreographer_test.cpp) plays its app's script with it and checks the app's lines against Android's.
#pragma once

#include "APKRuntime/apkrt.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace app_host {

namespace fs = std::filesystem;

/// An iPhone 15 Pro Max in portrait, at scale 3.
constexpr std::int32_t width = 1290, height = 2796;

extern int failures;
/// The real stderr, also once descriptor 2 is the capture's pipe.
extern int report;

void fail(const std::string &what);

/// What the session's handlers saw, written on guest threads.
struct Seen {
    std::mutex lock;
    std::condition_variable changed;
    std::vector<std::string> requests;  // "FINISH", "KEEP_SCREEN_ON 1", "START_FAILED <text>", "FRAME_RATE 60"
    bool ended = false;
    int status = -1;
};
extern Seen seen;

/// Called with each request as the handler records it, on the guest thread that makes it; for a harness
/// that acts on one at once.
extern void (*on_request)(int type, std::uint64_t argument);

/// Waits up to `limit` for `predicate`, under seen.lock.
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

    bool start();
    /// Everything written so far: a marker line of its own written now comes through after it.
    std::string drain();
};

/// The lines of `text` that begin with `prefix`.
std::string lines_with(const std::string &text, const std::string &prefix);

std::string read_text(const fs::path &path);

/// The paths on a harness's command line: <scratch directory> <fixtures bin> <bionic lib> <guest /system>
/// <fixtures/expected>.
struct Paths {
    fs::path scratch, fixtures, bionic, system, expected;
};

/// An app of fixtures/apps as activity-host runs it.
struct App {
    std::string name;     // lib<name>.so in the fixtures, apks/<name>.apk beside them
    std::string package;
    fs::path scratch;     // this run's, fresh
    fs::path container;   // activity-host's
    fs::path data, external, obb;  // the host folders of the app's data
    std::string guest;    // /data/app/<package>
};

/// Converts and mounts everything `app` needs below `paths.scratch / run`. False, after "skipped: ..." on
/// stdout, when a part is not built.
bool prepare(App &app, const Paths &paths, const std::string &run);

/// Begins the session with `configuration` and the handlers that fill `seen`.
void begin(const apkrt_configuration &configuration);

/// Starts activity-host for `app` with `apk` (a guest path), `library` and `function`. False after a failure.
bool start(const App &app, const std::string &apk, const char *library, const char *function);

/// Posts one event; its serial number, 0 when the post failed.
std::uint32_t post(apkrt_event_type type, std::uint64_t a0 = 0, std::uint64_t a1 = 0, std::uint64_t a2 = 0);

/// Waits for the answer to `serial`, which answers every event before it too.
void answered(std::uint32_t serial, const char *what);

/// Two 32-bit values in one argument, `low` in the low half.
std::uint64_t pair(std::int32_t low, std::int32_t high);

/// The events of a window that the host created, as Android 15 calls them: created, then resized once with
/// the same size, then (at the first) the content's rectangle, then redraw needed and the focus.
std::uint32_t show_window(std::uint64_t window, bool first);

/// The events of the quit after the app's ANativeActivity_finish, with the window's steps of ADR-0028's
/// decision 6: the window goes before onPause, and activity-host destroys the input queue before onDestroy.
/// Waits for the answer to DESTROY.
void quit(std::uint64_t window, bool window_destroyed = true);

}  // namespace app_host
