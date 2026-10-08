#include "session.h"

#include "descriptors.h"
#include "guest_context.h"
#include "syscalls.h"
#include "windows.h"

#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

namespace apkrt::session {
namespace {

namespace linux_errno = sys::linux_errno;

constexpr std::uint64_t keep_screen_on = 0x80;  // AWINDOW_FLAG_KEEP_SCREEN_ON
constexpr std::size_t text_limit = 1024;        // of a START_FAILED text, the NUL included

std::atomic_flag claimed = ATOMIC_FLAG_INIT;  // by the first apkrt_session_begin that is valid
apkrt_session described{};                    // written once, before `begun`
std::atomic<bool> begun{false};

// The channel. Posts, answers and the end change it under `lock`, and `changed` wakes the host's waits.
std::mutex lock;
std::condition_variable changed;
int write_end = -1;        // the event pipe's, the runtime's; non-blocking
int read_end = -1;         // until activity-host connects: then its guest descriptor holds it
bool connected = false;
std::uint32_t posted = 0;    // the last serial number posted
std::uint32_t answered = 0;  // the last serial number answered
bool broken = false;         // a post found the pipe full, or no reader: final
bool ended = false;          // the program has ended

// The window flags and format. A change and its report happen under one lock, so the reports come in the
// order of the changes, from whichever threads.
std::mutex flags_lock;
std::uint64_t window_flags = 0;
std::int32_t window_format = 4;  // PixelFormat.RGB_565, which NativeActivity.onCreate sets

/// PixelFormat.formatHasAlpha (Android 15): these have alpha, every other value has none. ViewRootImpl makes
/// a window's surface opaque when its format has none.
bool has_alpha(std::int32_t format) {
    switch (format) {
        case 8:     // A_8
        case 0xA:   // LA_88
        case 7:     // RGBA_4444
        case 6:     // RGBA_5551
        case 1:     // RGBA_8888
        case 0x16:  // RGBA_F16
        case 0x2B:  // RGBA_1010102
        case -3:    // TRANSLUCENT
        case -2:    // TRANSPARENT
            return true;
        default:
            return false;
    }
}

/// Calls one of the host's handlers with the runtime signal blocked: the handler is host code, whose calls must not
/// end with EINTR.
template <typename Call>
void as_host(Call call) {
    sigset_t runtime, before;
    sigemptyset(&runtime);
    sigaddset(&runtime, runtime_signal);
    ::pthread_sigmask(SIG_BLOCK, &runtime, &before);
    call();
    ::pthread_sigmask(SIG_SETMASK, &before, nullptr);
}

/// Calls the host's request handler.
void report(int type, std::uint64_t argument, const char *text) {
    if (described.request == nullptr) return;
    as_host([&] { described.request(described.context, type, argument, text); });
}

// The surfaces' live tokens (ADR-0043 c4), each with its make's format. A request's check, its change of the tokens
// and the host's call happen under one lock, so the host gets the requests in the order the runtime took them.
static_assert(sizeof(apkrt_surface_request) == 48);
constexpr std::size_t surface_limit = 16;
constexpr std::int64_t buffer_side_limit = 8192;
constexpr std::int64_t frame_side_limit = std::int64_t{1} << 20;

struct LiveSurface {
    std::uint64_t token;
    std::int32_t format;
};

std::mutex surfaces_lock;
std::vector<LiveSurface> live_surfaces;

/// APKRT_REQUEST_SURFACE: the request at `address` checked, its buffer's size and opacity set, and handed to the
/// host's surface handler. 0, or the negative errno (guest_interface.h).
long surface(std::uint64_t address) {
    if (described.surface == nullptr) return -linux_errno::eopnotsupp;
    apkrt_surface_request request;
    if (address == 0 ||
        !sys::memory::copy_checked(reinterpret_cast<std::uint64_t>(&request), address, sizeof request)) {
        return -linux_errno::efault;
    }
    if (request.op != APKRT_SURFACE_MAKE && request.op != APKRT_SURFACE_PLACE && request.op != APKRT_SURFACE_REMOVE) {
        return -linux_errno::einval;
    }
    if (request.token == 0) return -linux_errno::einval;
    apkrt_surface_request host{};
    host.op = request.op;
    host.token = request.token;
    if (request.op != APKRT_SURFACE_REMOVE) {
        const std::int64_t frame_width = std::int64_t{request.right} - request.left;
        const std::int64_t frame_height = std::int64_t{request.bottom} - request.top;
        if (frame_width <= 0 || frame_height <= 0 || frame_width > frame_side_limit ||
            frame_height > frame_side_limit) {
            return -linux_errno::einval;
        }
        if (request.z == 0 || request.z < -2 || request.z > 2) return -linux_errno::einval;
        if ((request.width == 0) != (request.height == 0)) return -linux_errno::einval;
        const std::int64_t width = request.width == 0 ? frame_width : request.width;
        const std::int64_t height = request.height == 0 ? frame_height : request.height;
        if (width < 1 || height < 1 || width > buffer_side_limit || height > buffer_side_limit) {
            return -linux_errno::einval;
        }
        host = request;
        host.width = static_cast<std::int32_t>(width);
        host.height = static_cast<std::int32_t>(height);
    }
    const std::lock_guard<std::mutex> hold(surfaces_lock);
    const auto found = std::find_if(live_surfaces.begin(), live_surfaces.end(),
                                    [&](const LiveSurface &live) { return live.token == request.token; });
    switch (request.op) {
        case APKRT_SURFACE_MAKE:
            if (found != live_surfaces.end()) return -linux_errno::eexist;
            if (live_surfaces.size() == surface_limit) return -linux_errno::enospc;
            live_surfaces.push_back({request.token, request.format});
            break;
        case APKRT_SURFACE_PLACE:
            if (found == live_surfaces.end()) return -linux_errno::enoent;
            host.format = found->format;
            break;
        default:  // remove
            if (found == live_surfaces.end()) return -linux_errno::enoent;
            live_surfaces.erase(found);
            break;
    }
    if (request.op != APKRT_SURFACE_REMOVE) host.opaque = has_alpha(host.format) ? 0 : 1;
    as_host([&] { described.surface(described.context, &host); });
    return 0;
}

// The windows' live tokens (ADR-0041, gap 4), each with its make's type, parent, group and format, apart from the
// surfaces'. As for those, a request's check, its change of the tokens and the host's call happen under one lock.
static_assert(sizeof(apkrt_window_request) == 72);
static_assert(offsetof(apkrt_window_request, op) == 0 && offsetof(apkrt_window_request, type) == 4 &&
              offsetof(apkrt_window_request, token) == 8 && offsetof(apkrt_window_request, parent) == 16 &&
              offsetof(apkrt_window_request, left) == 24 && offsetof(apkrt_window_request, top) == 28 &&
              offsetof(apkrt_window_request, right) == 32 && offsetof(apkrt_window_request, bottom) == 36 &&
              offsetof(apkrt_window_request, width) == 40 && offsetof(apkrt_window_request, height) == 44 &&
              offsetof(apkrt_window_request, format) == 48 && offsetof(apkrt_window_request, opaque) == 52 &&
              offsetof(apkrt_window_request, group) == 56 && offsetof(apkrt_window_request, alpha) == 60 &&
              offsetof(apkrt_window_request, dim) == 64 && offsetof(apkrt_window_request, reserved) == 68);
constexpr std::size_t window_limit = 16;
constexpr std::int32_t type_toast = 2005;

struct LiveWindow {
    std::uint64_t token, parent;
    std::int32_t type, format;
    std::uint32_t group;
};

std::mutex windows_lock;
std::vector<LiveWindow> live_windows;

/// LayoutParams' sub-window types, FIRST_SUB_WINDOW to LAST_SUB_WINDOW.
bool sub_window(std::int32_t type) {
    return type >= 1000 && type <= 1999;
}

/// Whether a float's `bits` hold a finite value within 0 to 1, as alpha and dimAmount are.
bool unit_interval(std::uint32_t bits) {
    float value;
    std::memcpy(&value, &bits, sizeof value);
    return std::isfinite(value) && value >= 0.0f && value <= 1.0f;
}

/// APKRT_REQUEST_WINDOW: the request at `address` checked, its buffer's size and opacity set, a place's type, parent,
/// group and format the make's, and handed to the host's window handler. 0, or the negative errno (guest_interface.h).
long window(std::uint64_t address) {
    if (described.window == nullptr) return -linux_errno::eopnotsupp;
    apkrt_window_request request;
    if (address == 0 ||
        !sys::memory::copy_checked(reinterpret_cast<std::uint64_t>(&request), address, sizeof request)) {
        return -linux_errno::efault;
    }
    if (request.op != APKRT_WINDOW_MAKE && request.op != APKRT_WINDOW_PLACE && request.op != APKRT_WINDOW_REMOVE) {
        return -linux_errno::einval;
    }
    if (request.token == 0) return -linux_errno::einval;
    apkrt_window_request host{};
    host.op = request.op;
    host.token = request.token;
    if (request.op != APKRT_WINDOW_REMOVE) {
        // The surfaces' rules for the frame and the size.
        const std::int64_t frame_width = std::int64_t{request.right} - request.left;
        const std::int64_t frame_height = std::int64_t{request.bottom} - request.top;
        if (frame_width <= 0 || frame_height <= 0 || frame_width > frame_side_limit ||
            frame_height > frame_side_limit) {
            return -linux_errno::einval;
        }
        if ((request.width == 0) != (request.height == 0)) return -linux_errno::einval;
        const std::int64_t width = request.width == 0 ? frame_width : request.width;
        const std::int64_t height = request.height == 0 ? frame_height : request.height;
        if (width < 1 || height < 1 || width > buffer_side_limit || height > buffer_side_limit) {
            return -linux_errno::einval;
        }
        if (!unit_interval(request.alpha) || !unit_interval(request.dim) || request.reserved != 0) {
            return -linux_errno::einval;
        }
        host = request;
        host.width = static_cast<std::int32_t>(width);
        host.height = static_cast<std::int32_t>(height);
    }
    const bool sub = sub_window(request.type);
    if (request.op == APKRT_WINDOW_MAKE) {
        // An application window, a sub-window or a toast; a parent for a sub-window only, a group for an application
        // window only.
        if (!(request.type >= 1 && request.type <= 99) && !sub && request.type != type_toast) {
            return -linux_errno::einval;
        }
        if (!sub && request.parent != 0) return -linux_errno::einval;
        if ((sub || request.type == type_toast) && request.group != 0) return -linux_errno::einval;
    }
    const std::lock_guard<std::mutex> hold(windows_lock);
    const auto live = [](std::uint64_t token) {
        return std::find_if(live_windows.begin(), live_windows.end(),
                            [token](const LiveWindow &each) { return each.token == token; });
    };
    const auto found = live(request.token);
    switch (request.op) {
        case APKRT_WINDOW_MAKE:
            if (found != live_windows.end()) return -linux_errno::eexist;
            if (live_windows.size() == window_limit) return -linux_errno::enospc;
            // One level of sub-windows, as on Android 15: the parent is the session's window or a live window that is
            // no sub-window itself.
            if (sub && request.parent != 0) {
                const auto parent = live(request.parent);
                if (parent == live_windows.end()) return -linux_errno::enoent;
                if (sub_window(parent->type)) return -linux_errno::einval;
            }
            live_windows.push_back({request.token, request.parent, request.type, request.format, request.group});
            break;
        case APKRT_WINDOW_PLACE:
            if (found == live_windows.end()) return -linux_errno::enoent;
            host.type = found->type;
            host.parent = found->parent;
            host.group = found->group;
            host.format = found->format;
            break;
        default:  // remove
            if (found == live_windows.end()) return -linux_errno::enoent;
            if (std::any_of(live_windows.begin(), live_windows.end(),
                            [&](const LiveWindow &each) { return each.parent == request.token; })) {
                return -linux_errno::ebusy;  // its sub-windows go first
            }
            live_windows.erase(found);
            break;
    }
    if (request.op != APKRT_WINDOW_REMOVE) host.opaque = has_alpha(host.format) ? 0 : 1;
    as_host([&] { described.window(described.context, &host); });
    return 0;
}

/// A time limit: none below 0 or for NaN, and at most 10^9 s (about 31 years), which fits every clock.
std::chrono::steady_clock::duration limit(double seconds) {
    if (!(seconds > 0)) return {};
    return std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(std::min(seconds, 1e9)));
}

}  // namespace

EndHandler end_handler() {
    if (!begun.load(std::memory_order_acquire)) return {nullptr, nullptr};
    return {described.end, described.context};
}

void end() {
    const std::lock_guard<std::mutex> hold(lock);
    ended = true;
    changed.notify_all();
}

int refresh_rate() {
    return begun.load(std::memory_order_acquire) ? described.configuration.refresh_rate : 0;
}

long connect(std::uint64_t configuration) {
    if (!begun.load(std::memory_order_acquire)) return -linux_errno::enoent;
    if (configuration == 0) return -linux_errno::efault;
    int host;
    {
        const std::lock_guard<std::mutex> hold(lock);
        if (connected) return -linux_errno::ebusy;
        connected = true;
        host = std::exchange(read_end, -1);
    }
    std::memcpy(reinterpret_cast<void *>(configuration), &described.configuration, sizeof described.configuration);
    return descriptors::add(host, true);
}

long configuration(std::uint64_t address) {
    if (!begun.load(std::memory_order_acquire)) return -linux_errno::enoent;
    if (address == 0) return -linux_errno::efault;
    // Set at the begin, before `begun`, and never again: no lock.
    std::memcpy(reinterpret_cast<void *>(address), &described.configuration, sizeof described.configuration);
    return 0;
}

long answer(std::uint64_t serial) {
    if (!begun.load(std::memory_order_acquire)) return -linux_errno::enoent;
    const std::lock_guard<std::mutex> hold(lock);
    if (serial == 0 || serial > posted) return -linux_errno::einval;
    if (serial > answered) {
        answered = static_cast<std::uint32_t>(serial);
        changed.notify_all();
    }
    return 0;
}

long request(std::uint64_t type, std::uint64_t a0, std::uint64_t a1) {
    if (!begun.load(std::memory_order_acquire)) return -linux_errno::enoent;
    switch (type) {
        case APKRT_REQUEST_FINISH:
            report(APKRT_REQUEST_FINISH, 0, nullptr);
            return 0;
        case APKRT_REQUEST_WINDOW_FLAGS: {
            // As Window.setFlags(add, add | remove): what is both added and removed stays added.
            const std::lock_guard<std::mutex> hold(flags_lock);
            const bool before = (window_flags & keep_screen_on) != 0;
            window_flags = (window_flags & ~a1) | a0;
            const bool on = (window_flags & keep_screen_on) != 0;
            if (on != before) report(APKRT_REQUEST_KEEP_SCREEN_ON, on ? 1 : 0, nullptr);
            return 0;
        }
        case APKRT_REQUEST_WINDOW_FORMAT: {
            // libandroid passes an int32_t: its low 32 bits.
            const auto format = static_cast<std::int32_t>(static_cast<std::uint32_t>(a0));
            const std::lock_guard<std::mutex> hold(flags_lock);
            const bool before = !has_alpha(window_format);
            window_format = format;
            const bool opaque = !has_alpha(format);
            if (opaque != before) report(APKRT_REQUEST_WINDOW_OPAQUE, opaque ? 1 : 0, nullptr);
            return 0;
        }
        case APKRT_REQUEST_START_FAILED: {
            if (a0 == 0) return -linux_errno::efault;
            char text[text_limit];
            const char *guest = reinterpret_cast<const char *>(a0);
            const std::size_t length = ::strnlen(guest, text_limit - 1);
            std::memcpy(text, guest, length);
            text[length] = '\0';
            report(APKRT_REQUEST_START_FAILED, 0, text);
            return 0;
        }
        case APKRT_REQUEST_FRAME_RATE:
            windows::set_frame_rate(a0, static_cast<std::uint32_t>(a1),
                                    [](std::uint32_t bits) { report(APKRT_REQUEST_FRAME_RATE, bits, nullptr); });
            return 0;
        case APKRT_REQUEST_SURFACE:
            return surface(a0);
        case APKRT_REQUEST_WINDOW:
            return window(a0);
        default:
            return -linux_errno::einval;
    }
}

}  // namespace apkrt::session

extern "C" int apkrt_session_begin(const struct apkrt_session *session) {
    using namespace apkrt::session;
    if (session == nullptr || session->end == nullptr) return -1;
    // The event pipe: the host writes without blocking and without SIGPIPE; both ends close on exec.
    int ends[2];
    if (::pipe(ends) != 0) return -1;
    for (const int end : ends) ::fcntl(end, F_SETFD, FD_CLOEXEC);
    ::fcntl(ends[1], F_SETFL, O_NONBLOCK);
    ::fcntl(ends[1], F_SETNOSIGPIPE, 1);
    if (claimed.test_and_set()) {
        ::close(ends[0]);
        ::close(ends[1]);
        return -1;
    }
    described = *session;
    read_end = ends[0];
    write_end = ends[1];
    begun.store(true, std::memory_order_release);
    return 0;
}

extern "C" uint32_t apkrt_session_post(uint32_t type, uint64_t a0, uint64_t a1, uint64_t a2) {
    using namespace apkrt::session;
    if (!begun.load(std::memory_order_acquire)) return 0;
    const std::lock_guard<std::mutex> hold(lock);
    if (broken || ended) return 0;
    const apkrt_event record{type, posted + 1, {a0, a1, a2}};
    // A record is far below PIPE_BUF: the write takes it whole or, in a full pipe, not at all.
    if (::write(write_end, &record, sizeof record) != static_cast<ssize_t>(sizeof record)) {
        broken = true;
        changed.notify_all();
        return 0;
    }
    return ++posted;
}

extern "C" int apkrt_session_wait(uint32_t serial, double seconds) {
    using namespace apkrt::session;
    if (!begun.load(std::memory_order_acquire)) return -1;
    const auto deadline = std::chrono::steady_clock::now() + limit(seconds);
    std::unique_lock<std::mutex> hold(lock);
    // 0 is what a failed post returns; a serial not posted yet has no event to wait for.
    if (serial == 0 || serial > posted) return -1;
    while (true) {
        if (answered >= serial) return 1;
        if (broken || ended) return -1;
        if (changed.wait_until(hold, deadline) == std::cv_status::timeout) {
            if (answered >= serial) return 1;
            return broken || ended ? -1 : 0;
        }
    }
}
