// apk-run: runs the program in a container dylib on the Mac, one program per process (ADR-0019).
// The tests start it once per fixture; the exit status is the program's own.
//   apk-run [--mount <guest path>=<host path>]... [--libraries <guest directory>=<host directory>]...
//           [--cwd <guest path>] [--app-environment] [--env <name>=<value>]... [--argv0 <text>]
//           [--configuration <width>x<height>@<scale>[,<language>[-<country>]]] [--lifecycle <step>[,<step>]...]
//           [--pick-file <host file>]... [--pick-download <host directory>] [--pick-data <host directory>]
//           [--pick-state <host directory>] [--export-dir <host directory>]
//           [--pick-photo <host file>]... [--sensor-script <host file>]
//           [--grant-location fine|coarse|denied] [--location-script <host file>]
//           [--device-profile pixel|galaxy] [--frame-rate 60|120]
//           [--pick-create Download] [--pick-tree <path below /storage/emulated/0>] [--webview-fake]
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
// FINISH with --lifecycle and the guest's pick requests (ADR-0057), which get a cancel unless --pick-file says what the
// user picks: then an ACTION_OPEN_DOCUMENT or ACTION_GET_CONTENT request gets the first file, or all of them when it
// allows several, cloned (copied here) into --pick-download, the folder the guest sees as /storage/emulated/0/Download,
// under a name no file there has, with the manifest of the clones' names in the guest's <data folder>/.minato/picks,
// --pick-data's folder being the data folder, as the host of APKRT_EVENT_PICK_RESULT (guest_interface.h) does, through
// descriptors and never through a link the app made (tools/pick_host.h). --pick-photo says the same for the photo
// picker (kind 5): up to its maximum of the files, cloned into the data folder's .minato/photos. --pick-create Download answers an ACTION_CREATE_DOCUMENT request by creating the file
// in --pick-download under the title, --pick-tree an ACTION_OPEN_DOCUMENT_TREE request with that folder (ADR-0057
// step d). An open request (ADR-0062) is answered as a host that showed it: RESULT_OK 300 ms after the request, with
// the line "apk-run: open <ID>: <kind>\t<decoded fields>" on stderr, tabs and newlines written out; with --pick-data a
// share's files follow, each as the host reads it, "apk-run: open <ID> file <name>: <size> bytes <first bytes>".
// --webview-fake, with --configuration, answers the guest's WebViews with the fake host of tools/webview_fake.h, which
// plays the effects of webview-test's pages as Android 15's WebView shows them (ADR-0056 step b2); without it a
// WebView request answers -EOPNOTSUPP, as where no host has a WebView.
// Without --configuration there is no frame clock, by design (vsync-lag-test posts its own ticks): a Java app's first
// traversal posts ViewRootImpl's sync barrier, which only a frame removes, so its main Looper then dispatches no post
// from another thread and no delayed post. A check that needs the main Looper passes a configuration.
// --lifecycle, with --configuration, plays the host's lifecycle as the App does in game mode (ADR-0028 decision 6,
// ADR-0032; activity-host's header lists the events), for a guest that reads the session (ADR-0043 step c1). Its
// steps follow one another, each once the one before is answered and a second has passed, as Android's reference
// scripts wait before HOME and before the return:
//   start       a window of the configuration's size (no layer), CREATE, START, RESUME, INPUT_QUEUE_CREATED without
//               a channel, the window created, resized with its size, the content's rectangle (the whole window),
//               redraw needed and the focus; the window's gate opens once these are posted, before their answers, as
//               the App's SessionBridge opens it with the focus: a Java app draws on the thread that answers them;
//               once they are answered, "apk-run: the start was answered" on stderr;
//   background  the window, the surfaces and the requested windows removed, PAUSE, the focus lost, each surface
//               destroyed, each requested window destroyed, the window destroyed and STOP, whose answers it waits for
//               1.5 s while the guest's GPU work still goes through (SessionBridge's backgroundAnswerBudget, as the
//               App's background order since 2026-10-27), then the background, "scheduled" and SAVE_INSTANCE_STATE;
//   foreground  the background off, a new window, START, RESUME and the window's events as at the start but the
//               rectangle, the gate opening as at the start; once they are answered, "apk-run: the return was
//               answered" on stderr.
//   tap:<x>:<y> (ADR-0045 step 7b2) a finger's tap at <x>, <y> in the window's pixels, on the session's input
//               channel: DOWN and UP at the same time, with the pressures 1 and 0, as Android's `input tap`;
//   swipe:<x1>:<y1>:<x2>:<y2>:<ms>  a finger's swipe from the first place to the second: DOWN, then a MOVE every
//               period of the session's refresh rate with two samples, the period's middle and its end, on the
//               line between the places, as UIKit hands the iPhone's coalesced touches, and UP at the second
//               place <ms> after the DOWN, as `input swipe`'s;
//   hwkey:<code>, device:add, device:remove  (ADR-0068 step 7c2) the same key from a hardware keyboard, device 1000,
//               and the keyboard's coming and going as a device record "Minato Keyboard" (SOURCE_KEYBOARD);
//   text:<op>:<arg0>:<arg1>:<text>  (ADR-0068 part 1, soft keyboard) a text record of the host's keyboard, as the App
//               posts it (APKRT_TEXT_*; the text is the rest of the step, UTF-8 as it is), for the connection of the
//               last "start" the guest's IME request made, which the guest's requests print as "apk-run: ime <serial>:
//               <text>" on stderr; the step waits up to five seconds for that start. With a text in the list the
//               start's INPUT_QUEUE_CREATED names an input channel; stext: is the same for the connection before
//               that one, whose records the guest must drop;
//   key:<code>  (ADR-0045 step 7c2) a key of the virtual keyboard, device -1, at Android key code <code>: DOWN and UP
//               at the same time on the session's input channel, as `input keyevent`'s. With a touch or a key in the
//               list, the start's INPUT_QUEUE_CREATED names an input channel (apkrt_input_add), else none, as
//               before;
//   pad:add, pad:remove, pad:key:<code>:<0|1>, pad:axes:<action>:<axis>=<value>;...  (ADR-0068 decision 5, the
//               controllers) a game controller of the host, device 1001 "Minato Test Pad", the Xbox Wireless
//               Controller's IDs and sources 0x01000511 (JOYSTICK | GAMEPAD | KEYBOARD): its coming and going as device
//               records, a button's down (1) or up (0) as a key record of source 0x501, and an axes record of source
//               0x01000010 with the action (2 MOVE) and the axes as Android numbers them (0 X, 1 Y, 11 Z, 14 RZ, 17
//               LTRIGGER, 18 RTRIGGER, 15 HAT_X, 16 HAT_Y), as the App's GCController side posts them;
//   mouse:add, mouse:remove, mouse:move:<x>:<y>:<dx>:<dy>, mouse:press:<buttons>, mouse:release:<buttons>,
//               mouse:scroll:<vertical>:<horizontal>, mouse:exit  (ADR-0068 decision 5, the mouse step) a mouse of the
//               host, device 1002 "Minato Test Mouse", sources 0x2002 (MOUSE): its coming and going as device records
//               and its reports through apkrt_input_post_mouse, as the App's GCMouse and pointer side posts them: a
//               move to <x>, <y> by <dx>, <dy>, the buttons held after a press or release as
//               AMOTION_EVENT_BUTTON_* (1 primary, 2 secondary, 4 tertiary, 8 back, 16 forward), a scroll in lines
//               (positive up and left), the pointer's leaving the window;
//   imehide     (ADR-0068, the keyboard reopen bug) the user hides the keyboard: APKRT_EVENT_IME_HIDDEN, then
//               APKRT_EVENT_IME_INSETS 0 as UIKit's keyboard frame would make the host post it;
//   turn:<width>:<height>  the display turns: the window takes the new size in px, as the App plays a turn
//               (ADR-0038): APKRT_EVENT_CONFIGURATION_CHANGED, the window's new size (apkrt_window_set_size), then
//               NATIVE_WINDOW_RESIZED, CONTENT_RECT_CHANGED (the whole window) and REDRAW_NEEDED, whose answer it
//               waits for. Without surface layers under graphics-run's layers only the guest's window turns;
//   pause:<ms>  waits <ms> more (up to 30000) before the next step, for an app whose own steps take longer than the
//               second between steps;
//   barinsets:<top>:<bottom>  the system bars' heights in px change, as the host posts APKRT_EVENT_BAR_INSETS at a
//               turn of the display;
//   trimmemory:<level>  the system trims the app's memory: APKRT_EVENT_TRIM_MEMORY with the level of
//               ComponentCallbacks2.onTrimMemory (20 UI_HIDDEN, 40 BACKGROUND, ...);
//   battery:<level>:<state>  (the battery, gap 1 of the compatibility plan) the host's battery changes to <level> %
//               (-1 unknown) in APKRT_BATTERY_STATE_<state> (0 unknown, 1 unplugged, 2 charging, 3 full), as the App's
//               UIDevice side sets it (apkrt_battery_set);
//   network:<flags>  (ADR-0054 step d)) the host's network changes to the APKRT_NETWORK_* flags <flags> (decimal), as
//               graphics/host's path monitor sets them (apkrt_network_set): 35 is Wi-Fi in Low Data Mode, 3 Wi-Fi,
//               3349 cellular over LTE (radio 13 in bits 8 to 15, metered);
//   back        (ADR-0041 decision 10) the system's back without a gesture, as a button sends it:
//               APKRT_EVENT_BACK with COMMIT, progress 0 and EDGE_LEFT, whose answer it does not wait for. The guest's
//               BACK_AT_ROOT request, where Android moves the task to the back and the App opens its menu, prints
//               "apk-run: back at root" on stderr and plays nothing.
// The program starts on a fixed battery, 100 % charging, as a phone on its cable: the emulator's battery that
// Android's reference scripts set (tools/android-reference.sh's battery-test).
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
// ("apk-run: the first post of window <id>"), which java_app_run.py --drawn looks for; MINATO_HEADLESS_LOST_POSTS=<n>
// loses the session's first n posts (headless_post). Before a touch they mark
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
#include "headless_audio_input.h"
#include "headless_host.h"
#include "pick_host.h"
#include "webview_fake.h"

#include <CoreFoundation/CoreFoundation.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

/// Whether the build sets the windows' buffers itself: graphics-run does, graphics/host's or its window host's fake
/// ones, and defines this to say so (graphics/tests/graphics_run.c); apk-run alone has none.
extern "C" __attribute__((weak)) int apk_run_sets_window_buffers(void) { return 0; }

/// The window host's layers, which graphics-run defines (graphics/tests/window_host.m); apk-run alone has none.
extern "C" __attribute__((weak)) const apk_run_layer_hooks *apk_run_layers(void) { return nullptr; }

/// The host's WebView handler, which graphics-run defines (graphics/host's WKWebView, ADR-0056 c1); apk-run alone has
/// none, and --webview-fake's wins over it.
extern "C" __attribute__((weak)) apkrt_webview_handler apk_run_webview(void) { return nullptr; }

namespace {

int usage() {
    std::fprintf(stderr,
                 "usage: apk-run [--mount <guest path>=<host path>]... [--libraries <guest directory>=<host directory>]...\n"
                 "               [--cwd <guest path>] [--app-environment] [--env <name>=<value>]... [--argv0 <text>]\n"
                 "               [--configuration <width>x<height>@<scale>[,<language>[-<country>]]]\n"
                 "               [--lifecycle <step>[,<step>]...] [--pick-file <host file>]...\n"
                 "               [--pick-download <host directory>] [--pick-data <host directory>]\n"
                 "               [--pick-state <host directory>] [--export-dir <host directory>]\n"
                 "               [--pick-photo <host file>]...\n"
                 "               [--pick-create Download] [--pick-tree <path below /storage/emulated/0>]\n"
                 "               [--webview-fake]\n"
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
    /// A key step (ADR-0045 step 7c2): its Android key code, or -1 for a touch.
    int key_code = -1;
    /// A hardware keyboard step (ADR-0068 step 7c2): hwkey:<code> sets `hardware`; device:add and device:remove set
    /// `device_op` to APKRT_DEVICE_ADDED or _REMOVED, else -1.
    bool hardware = false;
    int device_op = -1;
    /// A text step (ADR-0068 part 1): text:<op>:<arg0>:<arg1>:<text> sets `text_op` (else -1), the arguments and the
    /// text.
    int text_op = -1;
    int text_arg0 = 0, text_arg1 = 0;
    /// stext: the same, for the connection before the last one: its serial is one less, which the guest drops.
    bool text_stale = false;
    std::string text;
    /// A controller step (ADR-0068 decision 5): `pad_op` is 0 for none, 1 add, 2 remove, 3 a key (`pad_key`,
    /// `pad_down`) or 4 an axes record (`pad_action`, `pad_axes`).
    int pad_op = 0;
    int pad_key = 0, pad_down = 0, pad_action = 0;
    std::vector<std::pair<int, float>> pad_axes;
    /// A mouse step (ADR-0068 decision 5): `mouse_op` is 0 for none, 1 add, 2 remove, 3 move, 4 press, 5 release,
    /// 6 scroll or 7 exit; `mouse_buttons` the buttons held after a press or release, `mouse_values` a move's x, y, dx
    /// and dy or a scroll's vertical and horizontal.
    int mouse_op = 0, mouse_buttons = 0;
    float mouse_values[4] = {0, 0, 0, 0};
};

/// `step` as tap:<x>:<y> or swipe:<x1>:<y1>:<x2>:<y2>:<ms> with finite places and <ms> above 0, or key:<code> with
/// a code of 0 or more, or text:<op>:<arg0>:<arg1>:<text> with a known op; false for any other step.
bool parse_touch(const std::string &step, Touch &touch) {
    int used = 0;
    if (step == "mouse:add" || step == "mouse:remove" || step == "mouse:exit") {
        touch.mouse_op = step == "mouse:add" ? 1 : step == "mouse:remove" ? 2 : 7;
        return true;
    }
    if (std::sscanf(step.c_str(), "mouse:move:%f:%f:%f:%f%n", &touch.mouse_values[0], &touch.mouse_values[1],
                    &touch.mouse_values[2], &touch.mouse_values[3], &used) == 4 &&
        used == static_cast<int>(step.size()) && std::isfinite(touch.mouse_values[0]) &&
        std::isfinite(touch.mouse_values[1]) && std::isfinite(touch.mouse_values[2]) &&
        std::isfinite(touch.mouse_values[3])) {
        touch.mouse_op = 3;
        return true;
    }
    if (std::sscanf(step.c_str(), "mouse:scroll:%f:%f%n", &touch.mouse_values[0], &touch.mouse_values[1], &used) == 2 &&
        used == static_cast<int>(step.size()) && std::isfinite(touch.mouse_values[0]) &&
        std::isfinite(touch.mouse_values[1])) {
        touch.mouse_op = 6;
        return true;
    }
    if ((std::sscanf(step.c_str(), "mouse:press:%d%n", &touch.mouse_buttons, &used) == 1 ||
         std::sscanf(step.c_str(), "mouse:release:%d%n", &touch.mouse_buttons, &used) == 1) &&
        used == static_cast<int>(step.size()) && touch.mouse_buttons >= 0 && touch.mouse_buttons < 32) {
        touch.mouse_op = step.compare(0, 11, "mouse:press") == 0 ? 4 : 5;
        return true;
    }
    if (step == "pad:add" || step == "pad:remove") {
        touch.pad_op = step == "pad:add" ? 1 : 2;
        return true;
    }
    if (std::sscanf(step.c_str(), "pad:key:%d:%d%n", &touch.pad_key, &touch.pad_down, &used) == 2 &&
        used == static_cast<int>(step.size()) && touch.pad_key >= 0 && (touch.pad_down == 0 || touch.pad_down == 1)) {
        touch.pad_op = 3;
        return true;
    }
    if (std::sscanf(step.c_str(), "pad:axes:%d:%n", &touch.pad_action, &used) == 1 && used > 0) {
        touch.pad_axes.clear();
        std::istringstream list(step.substr(static_cast<std::size_t>(used)));
        for (std::string pair; std::getline(list, pair, ';');) {
            int axis = 0, taken = 0;
            float value = 0;
            if (std::sscanf(pair.c_str(), "%d=%f%n", &axis, &value, &taken) != 2 ||
                taken != static_cast<int>(pair.size()) || axis < 0 || axis >= APKRT_AXIS_LIMIT ||
                !std::isfinite(value) || touch.pad_axes.size() >= APKRT_AXES_MAX) {
                return false;
            }
            touch.pad_axes.emplace_back(axis, value);
        }
        touch.pad_op = 4;
        return true;
    }
    if (step.compare(0, 6, "stext:") == 0 && parse_touch(step.substr(1), touch)) {
        touch.text_stale = true;
        return true;
    }
    if (std::sscanf(step.c_str(), "text:%d:%d:%d:%n", &touch.text_op, &touch.text_arg0, &touch.text_arg1, &used) == 3 &&
        used > 0 && touch.text_op >= 0 && touch.text_op < APKRT_TEXT_OPS &&
        step.size() - static_cast<std::size_t>(used) <= APKRT_TEXT_BYTES) {
        touch.text = step.substr(static_cast<std::size_t>(used));
        return true;
    }
    touch.text_op = -1;
    used = 0;
    if (std::sscanf(step.c_str(), "key:%d%n", &touch.key_code, &used) == 1 && used == static_cast<int>(step.size()) &&
        touch.key_code >= 0) {
        return true;
    }
    used = 0;
    if (std::sscanf(step.c_str(), "hwkey:%d%n", &touch.key_code, &used) == 1 &&
        used == static_cast<int>(step.size()) && touch.key_code >= 0) {
        touch.hardware = true;
        return true;
    }
    touch.key_code = -1;
    if (step == "device:add" || step == "device:remove") {
        touch.device_op = step == "device:add" ? APKRT_DEVICE_ADDED : APKRT_DEVICE_REMOVED;
        return true;
    }
    used = 0;
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

/// `step` as battery:<level>:<state> with a level of -1 to 100 and a state of 0 to 3; false for any other step.
bool parse_battery(const std::string &step, int &level, int &state) {
    int used = 0;
    return std::sscanf(step.c_str(), "battery:%d:%d%n", &level, &state, &used) == 2 &&
           used == static_cast<int>(step.size()) && level >= -1 && level <= 100 &&
           state >= APKRT_BATTERY_STATE_UNKNOWN && state <= APKRT_BATTERY_STATE_FULL;
}

/// `step` as network:<flags>, the APKRT_NETWORK_* flags with the radio (0 to 65535); false for any other step.
bool parse_network(const std::string &step, int &flags) {
    int used = 0;
    return std::sscanf(step.c_str(), "network:%d%n", &flags, &used) == 1 && used == static_cast<int>(step.size()) &&
           flags >= 0 && flags <= 0xffff;
}

/// `step` as barinsets:<top>:<bottom>, the system bars' heights in px; false for any other step.
bool parse_bar_insets(const std::string &step, int &top, int &bottom) {
    int used = 0;
    return std::sscanf(step.c_str(), "barinsets:%d:%d%n", &top, &bottom, &used) == 2 &&
           used == static_cast<int>(step.size()) && top >= 0 && bottom >= 0;
}

/// `step` as turn:<width>:<height>, the new size of the window in px (both above 0); false for any other step.
bool parse_turn(const std::string &step, int &width, int &height) {
    int used = 0;
    return std::sscanf(step.c_str(), "turn:%d:%d%n", &width, &height, &used) == 2 &&
           used == static_cast<int>(step.size()) && width > 0 && height > 0;
}

/// `step` as pause:<ms>, a wait of 0 to 30000 ms; false for any other step.
bool parse_pause(const std::string &step, int &milliseconds) {
    int used = 0;
    return std::sscanf(step.c_str(), "pause:%d%n", &milliseconds, &used) == 1 &&
           used == static_cast<int>(step.size()) && milliseconds >= 0 && milliseconds <= 30000;
}

/// `step` as trimmemory:<level>, the level of ComponentCallbacks2.onTrimMemory (0 to 100); false for any other step.
bool parse_trim_memory(const std::string &step, int &level) {
    int used = 0;
    return std::sscanf(step.c_str(), "trimmemory:%d%n", &level, &used) == 1 &&
           used == static_cast<int>(step.size()) && level >= 0 && level <= 100;
}

/// --lifecycle's steps: start first, then background and foreground in turn, and touches, battery and network changes
/// and backs after the start or a foreground; false for any other list.
bool parse_lifecycle(const std::string &value, std::vector<std::string> &steps) {
    std::istringstream list(value);
    std::size_t turns = 0;  // the start, the backgrounds and the foregrounds so far
    for (std::string step; std::getline(list, step, ',');) {
        Touch touch;
        int level = 0, state = 0, top = 0, bottom = 0, wide = 0, high = 0;
        if (turns % 2 == 1 && (parse_touch(step, touch) || parse_battery(step, level, state) || step == "back" ||
                              step == "imehide" || parse_bar_insets(step, top, bottom) ||
                              parse_turn(step, wide, high) || parse_pause(step, level) ||
                              parse_trim_memory(step, level) || parse_network(step, level))) {
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

/// What the user picks (--pick-file and the others) and where the host puts the clones and the manifest; the host's
/// work is tools/pick_host.h's, which writes nothing through a link the app made (ADR-0057).
pick_host::Config pick_config;

/// --export-dir: where the model of the host's export to Photos copies what the app finished in MediaStore.
std::string export_directory;
std::atomic<int> exports_running{0};

std::int64_t now_ms();
void on_clone_written(void *context, const char *);

/// The watches on the clones: apkrt_watch_writes' id and the name its callback was given, by clone name. A watch ends
/// when the host forgets the clone (the user let it go, or the clone is gone), so that a file the app makes under
/// that name later is not mistaken for it.
std::mutex clone_watches_lock;
std::map<std::string, std::pair<int, std::string *>> clone_watches;

pick_host::Host &pick_host_of_run() {
    static pick_host::Host host(pick_config);
    static const bool wired = [] {
        host.on_clone = [](const std::string &clone) {
            // The clone is in the folder the guest sees as Download.
            auto *name = new std::string(clone);
            const std::string guest_path = "/storage/emulated/0/Download/" + clone;
            const int id = apkrt_watch_writes(guest_path.c_str(), on_clone_written, name);
            if (id < 0) {
                delete name;  // -EEXIST: the earlier watch on this very file serves
                return;
            }
            const std::lock_guard<std::mutex> lock(clone_watches_lock);
            clone_watches[clone] = {id, name};
        };
        host.write_back().writers = [](const std::string &clone) {
            int id = 0;
            {
                const std::lock_guard<std::mutex> lock(clone_watches_lock);
                const auto found = clone_watches.find(clone);
                if (found == clone_watches.end()) return -1;
                id = found->second.first;
            }
            return apkrt_watch_writers(id);  // -1 when the watch ended meanwhile
        };
        host.write_back().on_forget = [](const std::string &clone) {
            std::pair<int, std::string *> watch{0, nullptr};
            {
                const std::lock_guard<std::mutex> lock(clone_watches_lock);
                const auto found = clone_watches.find(clone);
                if (found == clone_watches.end()) return;
                watch = found->second;
                clone_watches.erase(found);
            }
            apkrt_unwatch_writes(watch.first);  // waits for a callback that is running, so the name can go
            delete watch.second;
        };
        return true;
    }();
    static_cast<void>(wired);
    return host;
}

/// The runtime tells that the guest has finished writing a clone: the host's record hears the close.
void on_clone_written(void *context, const char *) {
    pick_host_of_run().write_back().closed(*static_cast<std::string *>(context), now_ms());
}

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count();
}

/// The program ended: the edits the app made to the clones of the user's files reach the files (ADR-0057, write-back),
/// as a host does at the end of a session.
void write_back_at_end() {
    // The copies for Photos that are still running finish first (a few seconds at most).
    for (int i = 0; i < 500 && exports_running.load() != 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (pick_config.files.empty() || pick_config.download.empty()) return;
    pick_host_of_run().write_back().end_session(now_ms());
}

/// The runtime reports the last close of a clone the app wrote (apkrt_watch_writes); a thread also looks at the clones
/// every 100 ms for what no close reports (a write through a mapping, a rename over the clone), as the host's flush
/// points do, and writes back what is due.
void watch_clones() {
    if (pick_config.files.empty() || pick_config.download.empty()) return;
    // Records of an earlier run over the same folders (--pick-state): what was not written back then is now.
    pick_host_of_run().write_back().load(
            [](const std::string &token) { return std::make_unique<pick_host::PathOriginal>(token); });
    pick_host_of_run().write_back().flush(now_ms(), true);
    std::thread([] {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            pick_host_of_run().write_back().scan(now_ms());
            pick_host_of_run().write_back().flush(now_ms(), false);
        }
    }).detach();
}

/// A pick request is answered like a user who dismisses the picker: RESULT_CANCELED with no pick, a moment after the
/// request, on a thread of its own, as a host's picker answers when its view goes away. With --pick-file a document
/// request (kind 1 or 2) is answered with the user's picks instead. The runtime lets four requests wait at most, so
/// the threads are few; one that cannot start leaves the request unanswered, which the guest's activity survives.
void answer_pick(std::uint64_t id, const std::string &text) {
    const bool documents = !pick_config.files.empty() && (text.starts_with("1\t") || text.starts_with("2\t"));
    const bool photos = !pick_config.photos.empty() && text.starts_with("5\t");
    const bool several = text.size() > 3 && text[2] == '1';
    const bool save = pick_config.create && text.starts_with("3\t");
    const bool folder = !pick_config.tree.empty() && text.starts_with("4\t");
    // A save's title is the fifth field: "3\t<flags>\t<max>\t<types>\t<title>".
    std::string title;
    if (save) {
        std::size_t at = 0;
        for (int tabs = 0; tabs < 4 && at != std::string::npos; ++tabs) at = text.find('\t', at + 1);
        title = at == std::string::npos ? "" : text.substr(at + 1);
        if (title.empty()) title = "Untitled";
    }
    // The photo picker's maximum is the third field: "5\t<flags>\t<max>\t...".
    std::uint64_t maximum = 1;
    if (photos) {
        const auto first = text.find('\t', 2);
        if (first != std::string::npos) {
            maximum = std::max<std::uint64_t>(1, std::strtoull(text.c_str() + first + 1, nullptr, 10));
        }
    }
    try {
        std::thread([id, documents, photos, several, maximum, save, folder, title] {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            std::uint64_t count = 0;
            if ((documents && pick_host_of_run().documents(id, several, count)) ||
                (photos && pick_host_of_run().photos(id, maximum, count)) ||
                ((save || folder) && pick_host_of_run().location(id, save ? title : "", count))) {
                apkrt_session_post(APKRT_EVENT_PICK_RESULT, id, 0xFFFFFFFFu, count);
            } else {
                apkrt_session_post(APKRT_EVENT_PICK_RESULT, id, 0, 0);
            }
        }).detach();
    } catch (const std::system_error &) {
        std::fprintf(stderr, "apk-run: no thread for the answer to pick %llu\n", static_cast<unsigned long long>(id));
    }
}

/// An open request is answered as a host that showed what it asks: RESULT_OK with reason 0, a moment after the request,
/// on a thread of its own. The runtime has checked the text against guest_interface.h's grammar, whose schemes are all
/// the host's allow-list's. As with the picks, at most four wait, and one whose thread cannot start stays unanswered.
void answer_open(std::uint64_t id) {
    try {
        std::thread([id] {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            apkrt_session_post(APKRT_EVENT_OPEN_RESULT, id, 0xFFFFFFFFu, 0);
        }).detach();
    } catch (const std::system_error &) {
        std::fprintf(stderr, "apk-run: no thread for the answer to open %llu\n", static_cast<unsigned long long>(id));
    }
}

/// An open request's text with its fields decoded: every % and its two hex digits as the byte they stand for. The
/// runtime has checked that each % has them.
std::string decoded_open(const char *text) {
    const auto digit = [](char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; };
    std::string decoded;
    for (; *text != '\0'; ++text) {
        if (*text == '%' && text[1] != '\0' && text[2] != '\0') {
            decoded += static_cast<char>(digit(text[1]) << 4 | digit(text[2]));
            text += 2;
        } else {
            decoded += *text;
        }
    }
    return decoded;
}

/// A pick request's text on one line: the tabs and the newlines in it written out.
std::string one_line(const char *text) {
    std::string line;
    for (; *text != '\0'; ++text) {
        if (*text == '\t') {
            line += "\\t";
        } else if (*text == '\n') {
            line += "\\n";
        } else {
            line += *text;
        }
    }
    return line;
}

/// With --pick-data, a share's files as the host reads them (ADR-0062): each file the request names, from the data
/// folder's .minato/share/<ID>, opened through descriptors that follow no link, as guest_interface.h asks of the host,
/// each on a line "apk-run: open <ID> file <name>: <size> bytes <its first 64 bytes>", tabs and newlines written out
/// and other bytes outside printable ASCII as \xHH, or "refused" for one that is no regular file. Read before the
/// answer, after which the guest removes the folder, so a check holds the bytes the share sheet would get.
void print_shared_files(std::uint64_t id, const std::string &text) {
    std::vector<std::string> fields;
    std::istringstream split(text);
    for (std::string field; std::getline(split, field, '\t');) fields.push_back(field);
    if (fields.size() < 4 || fields[0] != "4") return;
    const std::size_t count = std::strtoul(fields[3].c_str(), nullptr, 10);
    if (fields.size() < 4 + count) return;
    const std::string request = std::to_string(id);
    int folder = open(pick_config.data.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    for (const char *name : {".minato", "share", request.c_str()}) {
        if (folder < 0) break;
        const int next = openat(folder, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        close(folder);
        folder = next;
    }
    for (std::size_t index = 0; index < count; ++index) {
        const std::string name = decoded_open(fields[4 + index].c_str());
        const int file =
            folder < 0 ? -1 : openat(folder, name.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
        struct stat facts {};
        std::string bytes;
        bool regular = file >= 0 && fstat(file, &facts) == 0 && S_ISREG(facts.st_mode);
        char buffer[4096];
        for (ssize_t got; regular && (got = read(file, buffer, sizeof buffer)) != 0;) {
            if (got < 0) {
                regular = false;
            } else {
                bytes.append(buffer, static_cast<std::size_t>(got));
            }
        }
        if (file >= 0) close(file);
        std::string shown;
        for (const char letter : bytes.substr(0, 64)) {
            const auto byte = static_cast<unsigned char>(letter);
            char escaped[5];
            if (byte == '\t') {
                shown += "\\t";
            } else if (byte == '\n') {
                shown += "\\n";
            } else if (byte >= 0x20 && byte < 0x7f) {
                shown += static_cast<char>(byte);
            } else {
                std::snprintf(escaped, sizeof escaped, "\\x%02x", byte);
                shown += escaped;
            }
        }
        if (regular) {
            std::fprintf(stderr, "apk-run: open %llu file %s: %zu bytes %s\n", static_cast<unsigned long long>(id),
                         name.c_str(), bytes.size(), shown.c_str());
        } else {
            std::fprintf(stderr, "apk-run: open %llu file %s: refused\n", static_cast<unsigned long long>(id),
                         name.c_str());
        }
    }
    if (folder >= 0) close(folder);
}

/// The serial of the last IME "start" request (ADR-0068 part 1), which the text steps address; 0 before the first.
std::atomic<std::uint64_t> ime_serial{0};

std::uint32_t request_orientation(int screen_orientation);

void on_request(void *, int type, std::uint64_t argument, const char *text) {
    if (type == APKRT_REQUEST_ORIENTATION) {
        std::fprintf(stderr, "apk-run: orientation request %d\n", static_cast<int>(argument));
        request_orientation(static_cast<int>(argument));
        return;
    }
    if (type == APKRT_REQUEST_IME) {
        const std::string request = text != nullptr ? text : "";
        std::fprintf(stderr, "apk-run: ime %llu: %s\n", static_cast<unsigned long long>(argument),
                     one_line(request.c_str()).c_str());
        if (request.compare(0, 5, "start") == 0) ime_serial.store(argument);
        // The model of UIKit's keyboard, whose frame the App reports as the keyboard's height (ADR-0068 step 2d): the
        // portrait keyboard of an iPhone 15 Pro Max with its home indicator, 335 points at scale 3.
        if (request == "show") apkrt_session_post(APKRT_EVENT_IME_INSETS, 1005, 0, 0);
        if (request == "hide") apkrt_session_post(APKRT_EVENT_IME_INSETS, 0, 0, 0);
        return;
    }
    if (type == APKRT_REQUEST_EXPORT_MEDIA) {
        // The model of the host's export to Photos (ADR-0057 step e): the handler only takes the file, a thread of its
        // own copies it into --export-dir, so that the guest and the other requests are not held up.
        const std::string request = text != nullptr ? text : "";
        auto job = std::make_shared<pick_host::ExportJob>(pick_host_of_run().export_begin(request));
        std::fprintf(stderr, "apk-run: export %s: %s\n", one_line(request.c_str()).c_str(),
                     job->ok() && !export_directory.empty() ? "taken" : "refused");
        if (job->ok() && !export_directory.empty()) {
            exports_running.fetch_add(1);
            std::thread([job] {
                job->run(export_directory);
                exports_running.fetch_sub(1);
            }).detach();
        }
        return;
    }
    if (type == APKRT_REQUEST_PICK) {
        std::fprintf(stderr, "apk-run: pick %llu: %s\n", static_cast<unsigned long long>(argument),
                     one_line(text != nullptr ? text : "").c_str());
        answer_pick(argument, text != nullptr ? text : "");
        return;
    }
    if (type == APKRT_REQUEST_PICK_CANCEL) {
        std::fprintf(stderr, "apk-run: pick %llu cancelled\n", static_cast<unsigned long long>(argument));
        return;
    }
    if (type == APKRT_REQUEST_OPEN) {
        std::fprintf(stderr, "apk-run: open %llu: %s\n", static_cast<unsigned long long>(argument),
                     one_line(decoded_open(text != nullptr ? text : "").c_str()).c_str());
        if (!pick_config.data.empty()) print_shared_files(argument, text != nullptr ? text : "");
        answer_open(argument);
        return;
    }
    if (type == APKRT_REQUEST_OPEN_CANCEL) {
        std::fprintf(stderr, "apk-run: open %llu cancelled\n", static_cast<unsigned long long>(argument));
        return;
    }
    if (type == APKRT_REQUEST_WINDOW_OPAQUE) {
        if (const auto *layers = apk_run_layers()) layers->window_opaque(argument != 0);
        return;
    }
    if (type == APKRT_REQUEST_BACK_AT_ROOT) {
        std::fprintf(stderr, "apk-run: back at root\n");
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

/// MINATO_HEADLESS_LOST_POSTS=<n>: the session's first n posts drop their buffer and answer -ENODEV, as a post to a
/// window lost meanwhile does, saying so on stderr; a window's first post is then the first one that is not lost.
std::mutex lost_lock;
int lost_posts = -1;  // -1 until the first post reads the variable
std::map<std::uint64_t, bool> first_lost;

int headless_post(void *, std::uint64_t id) {
    bool first = false;
    const int answer = buffers.post(id, first);
    const std::lock_guard<std::mutex> hold(lost_lock);
    if (lost_posts < 0) {
        const char *environment = std::getenv("MINATO_HEADLESS_LOST_POSTS");
        lost_posts = environment != nullptr ? std::atoi(environment) : 0;
    }
    if (answer == 0 && lost_posts > 0) {
        --lost_posts;
        if (first) first_lost[id] = true;
        std::fprintf(stderr, "apk-run: the post of window %llu is lost\n", static_cast<unsigned long long>(id));
        return -ENODEV;
    }
    if (answer == 0 && first_lost[id]) {
        first_lost[id] = false;
        first = true;
    }
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

/// The window of the start or the return and its size in px, which a turn changes: the turn step's and a guest's
/// request for another orientation (APKRT_REQUEST_ORIENTATION), which comes from the app's thread at any time.
struct Screen {
    std::mutex lock;
    std::uint64_t window = 0;
    int width = 0, height = 0;
};
Screen screen;

/// The lifecycle's window and its size, or no window (0) once it is taken away.
void set_screen(std::uint64_t window, int width, int height) {
    const std::lock_guard<std::mutex> hold(screen.lock);
    screen.window = window;
    screen.width = width;
    screen.height = height;
}

/// A turn as the App plays it (rotation_test.cpp): the configuration changes, the same window takes its new size,
/// then the window's events. Returns the serial of the last event, 0 without a window.
std::uint32_t turn_screen(int width, int height) {
    const std::lock_guard<std::mutex> hold(screen.lock);
    if (screen.window == 0) return 0;
    screen.width = width;
    screen.height = height;
    post(APKRT_EVENT_CONFIGURATION_CHANGED);
    if (apkrt_window_set_size(screen.window, width, height) != 0) {
        std::fprintf(stderr, "apk-run: a turn was refused\n");
    }
    post(APKRT_EVENT_NATIVE_WINDOW_RESIZED, screen.window, static_cast<std::uint64_t>(width),
         static_cast<std::uint64_t>(height));
    post(APKRT_EVENT_CONTENT_RECT_CHANGED, headless::pair(0, 0), headless::pair(width, height));
    return post(APKRT_EVENT_NATIVE_WINDOW_REDRAW_NEEDED, screen.window);
}

/// ActivityInfo.SCREEN_ORIENTATION_*: 1 for the values that force landscape, 2 for those that force portrait, 0 for
/// those that leave the orientation to the sensor, the user or the activity below.
int forced_orientation(int screen_orientation) {
    switch (screen_orientation) {
        case 0: case 6: case 8: case 11: return 1;   // LANDSCAPE, SENSOR_, REVERSE_, USER_LANDSCAPE
        case 1: case 7: case 9: case 12: return 2;   // PORTRAIT, SENSOR_, REVERSE_, USER_PORTRAIT
        default: return 0;
    }
}

/// A guest's request for an orientation: the window turns when a forcing value asks for the other one, as the App
/// turns the iPhone's window; any other value leaves it. Returns the serial of the turn's last event, 0 for none.
std::uint32_t request_orientation(int screen_orientation) {
    const int forced = forced_orientation(screen_orientation);
    int width = 0, height = 0;
    {
        const std::lock_guard<std::mutex> hold(screen.lock);
        width = screen.width;
        height = screen.height;
    }
    if (forced == 0 || width <= 0 || height <= 0) return 0;
    const int wide = std::max(width, height), narrow = std::min(width, height);
    const int new_width = forced == 1 ? wide : narrow, new_height = forced == 1 ? narrow : wide;
    if (new_width == width && new_height == height) return 0;
    return turn_screen(new_width, new_height);
}

/// The host's hardware keyboard (ADR-0068 step 7c2): its device ID, above Android's own and the touchscreen's.
constexpr std::int32_t kHardwareKeyboard = 1000;

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
    if (touch.mouse_op != 0) {
        constexpr std::int32_t kMouse = 1002;
        if (touch.mouse_op == 1 || touch.mouse_op == 2) {
            apkrt_device_event device{};
            device.op = touch.mouse_op == 1 ? APKRT_DEVICE_ADDED : APKRT_DEVICE_REMOVED;
            device.device_id = kMouse;
            device.sources = 0x2002;  // SOURCE_MOUSE
            device.vendor_id = 0x46d;
            device.product_id = 0xc077;
            std::strcpy(device.name, "Minato Test Mouse");
            if (apkrt_input_post_device(input, &device) != 1) {
                std::fprintf(stderr, "apk-run: the post of a device failed\n");
            }
            return;
        }
        apkrt_axes_event mouse{};
        mouse.event_time = down;
        mouse.device_id = kMouse;
        mouse.source = 0x2002;
        mouse.tool_type = 3;  // MOUSE
        mouse.button_state = touch.mouse_buttons;
        switch (touch.mouse_op) {
            case 3:
                mouse.action = 7;  // HOVER_MOVE
                mouse.axis_count = 4;
                mouse.axes[0] = {0, touch.mouse_values[0]};
                mouse.axes[1] = {1, touch.mouse_values[1]};
                mouse.axes[2] = {27, touch.mouse_values[2]};  // RELATIVE_X
                mouse.axes[3] = {28, touch.mouse_values[3]};
                break;
            case 4:
            case 5:
                mouse.action = touch.mouse_op == 4 ? 11 : 12;  // BUTTON_PRESS, BUTTON_RELEASE
                break;
            case 6:
                mouse.action = 8;  // SCROLL
                mouse.axis_count = 2;
                mouse.axes[0] = {9, touch.mouse_values[0]};   // VSCROLL
                mouse.axes[1] = {10, touch.mouse_values[1]};  // HSCROLL
                break;
            default:
                mouse.action = 10;  // HOVER_EXIT
                break;
        }
        if (apkrt_input_post_mouse(input, &mouse) != 1) std::fprintf(stderr, "apk-run: the post of a mouse failed\n");
        return;
    }
    if (touch.pad_op != 0) {
        constexpr std::int32_t kPad = 1001, kPadSources = 0x01000511;
        if (touch.pad_op == 1 || touch.pad_op == 2) {
            apkrt_device_event device{};
            device.op = touch.pad_op == 1 ? APKRT_DEVICE_ADDED : APKRT_DEVICE_REMOVED;
            device.device_id = kPad;
            device.sources = kPadSources;
            device.vendor_id = 0x45e;
            device.product_id = 0x2e0;
            std::strcpy(device.name, "Minato Test Pad");
            if (apkrt_input_post_device(input, &device) != 1) {
                std::fprintf(stderr, "apk-run: the post of a device failed\n");
            }
        } else if (touch.pad_op == 3) {
            apkrt_key_event key{};
            key.down_time = key.event_time = down;
            key.key_code = touch.pad_key;
            key.device_id = kPad;
            key.source = 0x501;  // SOURCE_KEYBOARD | SOURCE_GAMEPAD
            key.action = touch.pad_down == 1 ? 0 : 1;
            if (apkrt_input_post_key(input, &key) != 1) std::fprintf(stderr, "apk-run: the post of a key failed\n");
        } else {
            apkrt_axes_event axes{};
            axes.action = touch.pad_action;
            axes.event_time = down;
            axes.device_id = kPad;
            axes.source = 0x01000010;  // SOURCE_JOYSTICK
            axes.axis_count = static_cast<std::uint32_t>(touch.pad_axes.size());
            for (std::size_t at = 0; at < touch.pad_axes.size(); ++at) {
                axes.axes[at] = {touch.pad_axes[at].first, touch.pad_axes[at].second};
            }
            if (apkrt_input_post_axes(input, &axes) != 1) std::fprintf(stderr, "apk-run: the post of axes failed\n");
        }
        return;
    }
    if (touch.text_op >= 0) {
        for (int waited = 0; ime_serial.load() == 0 && waited < 500; ++waited) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        apkrt_text_event event{};
        event.op = touch.text_op;
        event.arg0 = touch.text_arg0;
        event.arg1 = touch.text_arg1;
        event.serial = static_cast<std::int64_t>(ime_serial.load()) - (touch.text_stale ? 1 : 0);
        event.length = static_cast<std::uint32_t>(touch.text.size());
        std::memcpy(event.text, touch.text.data(), touch.text.size());
        if (apkrt_input_post_text(input, &event) != 1) std::fprintf(stderr, "apk-run: the post of a text failed\n");
        return;
    }
    if (touch.device_op >= 0) {
        // The session's hardware keyboard: one device, which the host adds and removes.
        apkrt_device_event device{};
        device.op = touch.device_op;
        device.device_id = kHardwareKeyboard;
        device.sources = 0x101;  // SOURCE_KEYBOARD
        std::strcpy(device.name, "Minato Keyboard");
        if (apkrt_input_post_device(input, &device) != 1) {
            std::fprintf(stderr, "apk-run: the post of a device failed\n");
        }
        return;
    }
    if (touch.key_code >= 0) {
        // The virtual keyboard's key (device -1) with adb's source 0, as `input keyevent` sends it on Android 15
        // (ADR-0069 step 1), or the hardware keyboard's with AINPUT_SOURCE_KEYBOARD, down and up at once.
        apkrt_key_event key{};
        key.down_time = key.event_time = down;
        key.key_code = touch.key_code;
        key.device_id = touch.hardware ? kHardwareKeyboard : -1;
        key.source = touch.hardware ? 0x101 : 0;
        for (const std::int32_t action : {0, 1}) {
            key.action = action;
            if (apkrt_input_post_key(input, &key) != 1) std::fprintf(stderr, "apk-run: the post of a key failed\n");
        }
        return;
    }
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
    const auto hide = [&]() -> std::uint32_t {
        set_screen(0, width, height);
        apkrt_window_remove(window);
        surfaces.take_away();
        windows.take_away();
        post(APKRT_EVENT_PAUSE);
        post(APKRT_EVENT_WINDOW_FOCUS_CHANGED, 0);
        surfaces.post_destroyed();
        windows.post_destroyed();
        post(APKRT_EVENT_NATIVE_WINDOW_DESTROYED, window);
        return post(APKRT_EVENT_STOP);
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
        {
            // An app's request for another orientation may have turned the window since the last step.
            const std::lock_guard<std::mutex> hold(screen.lock);
            if (screen.window != 0) {
                width = screen.width;
                height = screen.height;
            }
        }
        if (steps[index] == "start") {
            window = add_window();
            set_screen(window, width, height);
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
        } else if (int level = 0, state = 0; parse_battery(steps[index], level, state)) {
            apkrt_battery_set(level, state);
        } else if (int flags = 0; parse_network(steps[index], flags)) {
            apkrt_network_set(static_cast<std::uint32_t>(flags));
        } else if (int milliseconds = 0; parse_pause(steps[index], milliseconds)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
        } else if (int wide = 0, high = 0; parse_turn(steps[index], wide, high)) {
            // A turn as the App plays it (rotation_test.cpp): the configuration changes, the same window takes its
            // new size, then the window's events; the later steps and a return use the new size.
            width = wide;
            height = high;
            if (!answered(turn_screen(width, height), "a turn")) return;
        } else if (int top = 0, bottom = 0; parse_bar_insets(steps[index], top, bottom)) {
            post(APKRT_EVENT_BAR_INSETS, static_cast<std::uint64_t>(top), static_cast<std::uint64_t>(bottom), 0);
        } else if (int trim = 0; parse_trim_memory(steps[index], trim)) {
            post(APKRT_EVENT_TRIM_MEMORY, static_cast<std::uint64_t>(trim), 0, 0);
        } else if (steps[index] == "back") {
            post(APKRT_EVENT_BACK, APKRT_BACK_COMMIT, 0, 0);
        } else if (steps[index] == "imehide") {
            // The user hides the keyboard (the bar's key): the host posts it and, as UIKit's frame changes, the inset.
            post(APKRT_EVENT_IME_HIDDEN, 0, 0, 0);
            post(APKRT_EVENT_IME_INSETS, 0, 0, 0);
        } else if (Touch touch; parse_touch(steps[index], touch)) {
            if (apk_run_sets_window_buffers() == 0) {
                buffers.mark();
                touched = steps[index];
            }
            play_touch(input, touch, rate);
        } else if (steps[index] == "background") {
            surfaces.set_background(true);
            windows.set_background(true);
            // The teardown's answers come first, while the runtime lets the guest's GPU work through: a GL thread
            // that lets its surface go (GLSurfaceView's onPause waits for it) commits ANGLE's work, which the
            // background holds until the return.
            const std::uint32_t stop = hide();
            if (stop != 0) apkrt_session_wait(stop, 1.5);
            apkrt_set_background(1);
            if (apkrt_wait_scheduled(1.0) != 1) std::fprintf(stderr, "apk-run: \"scheduled\" did not answer yes\n");
            if (!answered(post(APKRT_EVENT_SAVE_INSTANCE_STATE), "the background")) return;
            stopped = true;
        } else {
            apkrt_set_background(0);
            surfaces.set_background(false);
            windows.set_background(false);
            window = add_window();
            set_screen(window, width, height);
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

// --- --sensor-script: a fake sensor host that replays a script, for the Java end-to-end test (ADR-0061). ---
struct SensorSample {
    int at_ms;
    std::int32_t type;
    std::vector<float> values;
};

int sensor_feed_start(void *, std::int32_t, std::int64_t) { return 0; }  // CoreMotion is replaced by the script
void sensor_feed_stop(void *, std::int32_t) {}

// The value count of each offered sensor (guest_interface.h's table), for the `rate` shorthand.
int sensor_value_count(std::int32_t type) {
    switch (type) {
        case 1:
        case 2:
        case 4:
        case 9:
        case 10:
            return 3;  // accelerometer, magnetic, gyroscope, gravity, linear acceleration
        case 15:
            return 4;  // game rotation vector
        case 11:
            return 5;  // rotation vector
        case 14:
        case 16:
            return 6;  // magnetic/gyroscope uncalibrated
        default:
            return 0;
    }
}

void run_sensor_feed(std::vector<SensorSample> samples) {
    std::sort(samples.begin(), samples.end(),
              [](const SensorSample &a, const SensorSample &b) { return a.at_ms < b.at_ms; });
    const auto start = std::chrono::steady_clock::now();
    for (const SensorSample &sample : samples) {
        std::this_thread::sleep_until(start + std::chrono::milliseconds(sample.at_ms));
        const double host_time = static_cast<double>(clock_gettime_nsec_np(CLOCK_UPTIME_RAW)) / 1e9;
        apkrt_sensor_push(sample.type, sample.values.data(), sample.values.size(), host_time, 3);  // ACCURACY_HIGH
    }
}

// A --sensor-script line is "<at_ms> <type> <v0> <v1>..." (one sample) or "rate <type> <hz> <seconds>" (samples at
// 1000/hz ms apart, each value a small ramp); '#' comments and blank lines are skipped. Installs a fake host for the
// types the script names and replays the samples on a detached thread, so a Java SensorEventListener test sees them.
void start_sensor_feed(const std::string &path) {
    std::ifstream file(path);
    if (!file) {
        std::fprintf(stderr, "apk-run: cannot open sensor script %s\n", path.c_str());
        return;
    }
    std::vector<SensorSample> samples;
    std::vector<std::int32_t> types;
    const auto note_type = [&](std::int32_t type) {
        if (std::find(types.begin(), types.end(), type) == types.end()) types.push_back(type);
    };
    std::string line;
    while (std::getline(file, line)) {
        std::istringstream in(line);
        std::string first;
        if (!(in >> first) || first[0] == '#') continue;
        if (first == "rate") {
            std::int32_t type = 0;
            double hz = 0;
            double seconds = 0;
            if (!(in >> type >> hz >> seconds) || hz <= 0 || seconds < 0) continue;
            const int count = sensor_value_count(type);
            if (count == 0) continue;
            const int total = static_cast<int>(hz * seconds);
            for (int i = 0; i < total; ++i) {
                SensorSample sample;
                sample.at_ms = static_cast<int>(std::lround(i * 1000.0 / hz));
                sample.type = type;
                for (int k = 0; k < count; ++k) {
                    sample.values.push_back(static_cast<float>(i) * 0.01f + static_cast<float>(k));
                }
                note_type(type);
                samples.push_back(std::move(sample));
            }
            continue;
        }
        SensorSample sample;
        try {
            sample.at_ms = std::stoi(first);
        } catch (const std::exception &) {
            continue;
        }
        if (!(in >> sample.type)) continue;
        float value = 0;
        while (in >> value) sample.values.push_back(value);
        if (sample.values.empty()) continue;
        note_type(sample.type);
        samples.push_back(std::move(sample));
    }
    if (samples.empty()) return;
    const std::vector<std::int32_t> available = types;
    const apkrt_sensor_host host = {nullptr, sensor_feed_start, sensor_feed_stop};
    apkrt_sensor_set_host(&host, available.data(), available.size());
    std::thread(run_sensor_feed, std::move(samples)).detach();
}

// --- --location-script / --grant-location: a fake CoreLocation host that replays a script (ADR-0063), for the Java
// end-to-end location test. --grant-location seeds the per-app grant the runtime gates on, as the host does at a
// session's begin; the fudging of a coarse grant then happens in the runtime, not here. ---
struct ScriptedFix {
    int at_ms;
    apkrt_location_fix fix;
};

std::mutex location_mutex;
apkrt_location_fix location_latest{};  // the newest fix the feed pushed, for a getCurrentLocation answer
bool location_have_latest = false;

int location_start(void *, std::int32_t, std::int32_t) { return 0; }  // CoreLocation is replaced by the script
void location_stop(void *) {}
// getCurrentLocation: answer with the newest scripted fix, or no location when none has come yet.
int location_request_single(void *, std::uint64_t token) {
    apkrt_location_fix fix{};
    bool have;
    {
        const std::lock_guard<std::mutex> hold(location_mutex);
        have = location_have_latest;
        fix = location_latest;
    }
    apkrt_location_push_single(token, have ? &fix : nullptr);
    return 0;
}
void location_cancel_single(void *, std::uint64_t) {}

void run_location_feed(std::vector<ScriptedFix> fixes) {
    std::sort(fixes.begin(), fixes.end(), [](const ScriptedFix &a, const ScriptedFix &b) { return a.at_ms < b.at_ms; });
    const auto start = std::chrono::steady_clock::now();
    for (const ScriptedFix &scripted : fixes) {
        std::this_thread::sleep_until(start + std::chrono::milliseconds(scripted.at_ms));
        apkrt_location_fix fix = scripted.fix;
        fix.time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count();
        fix.elapsed_realtime_ns = static_cast<std::int64_t>(clock_gettime_nsec_np(CLOCK_MONOTONIC));
        {
            const std::lock_guard<std::mutex> hold(location_mutex);
            location_latest = fix;
            location_have_latest = true;
        }
        apkrt_location_push(&fix);
    }
}

apkrt_location_fix scripted_fix(double lat, double lon, float accuracy, bool has_altitude, double altitude) {
    apkrt_location_fix fix{};
    fix.latitude = lat;
    fix.longitude = lon;
    fix.horizontal_accuracy_m = accuracy;
    fix.provider = 3;  // one CoreLocation source, which the framework names fused
    fix.valid = APKRT_LOCATION_HAS_HORIZONTAL_ACCURACY;
    if (has_altitude) {
        fix.altitude = altitude;
        fix.valid |= APKRT_LOCATION_HAS_ALTITUDE;
    }
    return fix;
}

// A --location-script line is "<at_ms> <lat> <lon> <accuracy_m> [altitude]" (one fix) or
// "rate <hz> <seconds> <lat> <lon> <accuracy_m>" (a fix every 1000/hz ms for <seconds>, at the one position);
// '#' comments and blank lines are skipped. Installs a fake host and replays the fixes on a detached thread.
void start_location_feed(const std::string &path) {
    std::ifstream file(path);
    if (!file) {
        std::fprintf(stderr, "apk-run: cannot open location script %s\n", path.c_str());
        return;
    }
    std::vector<ScriptedFix> fixes;
    std::string line;
    while (std::getline(file, line)) {
        std::istringstream in(line);
        std::string first;
        if (!(in >> first) || first[0] == '#') continue;
        if (first == "rate") {
            double hz = 0, seconds = 0, lat = 0, lon = 0, accuracy = 0;
            if (!(in >> hz >> seconds >> lat >> lon >> accuracy) || hz <= 0 || seconds < 0) continue;
            const int total = static_cast<int>(hz * seconds);
            for (int i = 0; i < total; ++i) {
                fixes.push_back({static_cast<int>(std::lround(i * 1000.0 / hz)),
                                 scripted_fix(lat, lon, static_cast<float>(accuracy), false, 0)});
            }
            continue;
        }
        int at_ms = 0;
        try {
            at_ms = std::stoi(first);
        } catch (const std::exception &) {
            continue;
        }
        double lat = 0, lon = 0, accuracy = 0, altitude = 0;
        if (!(in >> lat >> lon >> accuracy)) continue;
        const bool has_altitude = static_cast<bool>(in >> altitude);
        fixes.push_back({at_ms, scripted_fix(lat, lon, static_cast<float>(accuracy), has_altitude, altitude)});
    }
    const apkrt_location_host host = {nullptr, location_start, location_stop, location_request_single,
                                      location_cancel_single};
    apkrt_location_set_host(&host);
    if (!fixes.empty()) std::thread(run_location_feed, std::move(fixes)).detach();
}

// --grant-location's value as a stored outcome; -1 (and *ok false) when it does not name one.
std::int32_t parse_grant(const std::string &value, bool &ok) {
    ok = true;
    if (value == "fine") return APKRT_PERMISSION_GRANTED;
    if (value == "coarse") return APKRT_PERMISSION_GRANTED_COARSE;
    if (value == "denied") return APKRT_PERMISSION_DENIED;
    ok = false;
    return -1;
}

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
    std::string sensor_script;
    std::string location_script;
    std::int32_t location_grant = -1;  // -1: no --grant-location, so the runtime stays at its default (no grant)
    std::string device_profile;        // empty: no --device-profile, so the host's own names (ADR-0065)
    int frame_rate = 0;                // 0: no --frame-rate, so no FPS unlock (ADR-0066)
    bool webview_fake = false;         // --webview-fake: the fake WebView host (ADR-0056 step b2)
    int index = 1;
    for (; index < argc && std::string_view(argv[index]).starts_with("--"); ++index) {
        const std::string_view option = argv[index];
        if (option == "--app-environment") {
            shell.clear();
            continue;
        }
        if (option == "--webview-fake") {
            webview_fake = true;
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
        } else if (option == "--pick-file") {
            pick_config.files.push_back(value);
        } else if (option == "--pick-create") {
            pick_config.create = (value == "Download");
            if (!pick_config.create) return usage();
        } else if (option == "--pick-tree") {
            pick_config.tree = value;
        } else if (option == "--pick-photo") {
            pick_config.photos.push_back(value);
        } else if (option == "--pick-data") {
            pick_config.data = value;
        } else if (option == "--pick-download") {
            pick_config.download = value;
        } else if (option == "--export-dir") {
            export_directory = value;
        } else if (option == "--sensor-script") {
            sensor_script = value;
        } else if (option == "--location-script") {
            location_script = value;
        } else if (option == "--grant-location") {
            bool ok = false;
            location_grant = parse_grant(value, ok);
            if (!ok) return usage();
        } else if (option == "--device-profile") {
            device_profile = value;
        } else if (option == "--frame-rate") {
            // The FPS unlock (ADR-0066): 60 or 120.
            if (value != "60" && value != "120") return usage();
            frame_rate = std::stoi(value);
        } else if (option == "--scudo-regions") {
            // Force the guest libc build (ADR-0035 item 3): 27 (128 MiB scudo regions) or 28 (256 MiB), in place of the
            // address-space probe, so a check exercises either; the runtime reads MINATO_SCUDO_REGIONS (dl.cpp).
            if (value != "27" && value != "28") return usage();
            ::setenv("MINATO_SCUDO_REGIONS", value.c_str(), 1);
        } else if (option == "--pick-state") {
            pick_config.state = value;
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

    // The fixed battery, before the session as the App's BatteryHost sets it: the guest's first look reads it (GET),
    // and
    // only a lifecycle step's change is an event.
    apkrt_battery_set(100, APKRT_BATTERY_STATE_CHARGING);
    if (configured) {
        // The end handler runs on the guest's thread instead of the process's end: it ends the process as that would.
        session.end = [](void *, int status) {
            write_back_at_end();
            ::_exit(status);
        };
        if (!lifecycle.empty()) {
            session.request = on_request;
            session.surface = on_surface;
            session.window = on_window;
        }
        session.webview = webview_fake ? webview_fake::handle : apk_run_webview();
        if (apkrt_session_begin(&session) != 0) {
            std::fprintf(stderr, "apk-run: the session did not begin\n");
            return 2;
        }
        std::thread(run_frame_clock, session.configuration.refresh_rate).detach();
        if (!lifecycle.empty() && apk_run_sets_window_buffers() == 0) {
            static const apkrt_window_buffers headless_buffers = {headless_lock, headless_post, headless_removed,
                                                                  nullptr, nullptr};
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
    // The microphone hears the WAV file of MINATO_AUDIO_INPUT_FILE, or silence (ADR-0064). The host records its grant
    // at a session's begin, as the location's; MINATO_GRANT_MICROPHONE=granted|denied stands for it where a test
    // harness cannot pass options, and without it the runtime stays at its default (no grant).
    apkrt_set_audio_input_host(&headless_audio_input::host);
    if (const char *grant = std::getenv("MINATO_GRANT_MICROPHONE")) {
        const std::string value = grant;
        if (value == "granted") apkrt_permission_answer(0, APKRT_PERMISSION_MICROPHONE, APKRT_PERMISSION_GRANTED);
        if (value == "denied") apkrt_permission_answer(0, APKRT_PERMISSION_MICROPHONE, APKRT_PERMISSION_DENIED);
    }
    // The camera's grant (ADR-0073 decision 8) the same way: MINATO_GRANT_CAMERA=granted|denied.
    if (const char *grant = std::getenv("MINATO_GRANT_CAMERA")) {
        const std::string value = grant;
        if (value == "granted") apkrt_permission_answer(0, APKRT_PERMISSION_CAMERA, APKRT_PERMISSION_GRANTED);
        if (value == "denied") apkrt_permission_answer(0, APKRT_PERMISSION_CAMERA, APKRT_PERMISSION_DENIED);
    }
    if (!sensor_script.empty()) start_sensor_feed(sensor_script);
    // The host records the location grant at a session's begin (ADR-0063 decision 2), so the runtime gates on it from
    // the start; then the fake CoreLocation host replays the script, the runtime fudging a coarse grant on its own.
    if (location_grant >= 0) apkrt_permission_answer(0, APKRT_PERMISSION_LOCATION, location_grant);
    // MINATO_DEVICE_PROFILE stands for --device-profile where a test harness cannot pass options (graphics' CTest
    // tests).
    if (device_profile.empty()) {
        if (const char *environment = std::getenv("MINATO_DEVICE_PROFILE")) device_profile = environment;
    }
    if (apkrt_set_device_profile(device_profile.c_str()) != 0) return usage();
    if (apkrt_set_frame_rate_unlock(frame_rate) != 0) return usage();
    if (!location_script.empty()) start_location_feed(location_script);
    watch_clones();
    if (apkrt_start_program(container, arguments.data(), variables.data(), error, sizeof error) == 0) {
        // The program's end ends the process, or goes to the session's end handler. Until then the main thread runs
        // its run loop, as the app's does, so that what the host's frameworks queue on the main queue runs: an
        // AVPlayerItem's status and end (ADR-0060 e2e1). A timer that never fires keeps the run loop from returning.
        CFRunLoopTimerRef keep = CFRunLoopTimerCreate(nullptr, 1e18, 0, 0, 0, nullptr, nullptr);
        CFRunLoopAddTimer(CFRunLoopGetMain(), keep, kCFRunLoopDefaultMode);
        for (;;) CFRunLoopRun();
    }
    std::fprintf(stderr, "apk-run: %s\n", error);
    if (configured) {
        // The frame clock's thread still posts ticks: no static destructor may run under it, as the session's end
        // handler also ends without them.
        std::fflush(stderr);
        ::_exit(127);
    }
    return 127;
}
