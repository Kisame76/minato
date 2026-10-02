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
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <utility>

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

/// Calls the host's request handler, with the runtime signal blocked: the handler is host code, whose
/// calls must not end with EINTR.
void report(int type, std::uint64_t argument, const char *text) {
    if (described.request == nullptr) return;
    sigset_t runtime, before;
    sigemptyset(&runtime);
    sigaddset(&runtime, runtime_signal);
    ::pthread_sigmask(SIG_BLOCK, &runtime, &before);
    described.request(described.context, type, argument, text);
    ::pthread_sigmask(SIG_SETMASK, &before, nullptr);
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
