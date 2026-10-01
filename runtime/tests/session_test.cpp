// Checks the session's channel (ADR-0028, decisions 2 to 4): events arrive in order at the guest
// descriptor that session_connect gives, with the session's configuration; an answer wakes the host's
// wait, which ends at its time limit otherwise; a full pipe makes the post fail instead of blocking, and
// every later post fails too; the guest's requests reach the host's handler, KEEP_SCREEN_ON only when it
// changes and WINDOW_OPAQUE only when the window's format changes its opacity. A process has one session,
// so the tests build on each other in their order: first without a session, last the full pipe. The test's
// thread plays activity-host and calls the dispatcher directly, as syscalls_test.cpp does. The program's end
// is program_test.cpp's (--session-ends).
#include "signals.h"
#include "syscall_frame.h"
#include "threads.h"

#include "APKRuntime/apkrt.h"
#include "check.h"

#include <pthread.h>
#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

// Linux: include/uapi/asm-generic/unistd.h, fcntl.h, errno.h.
constexpr std::uint64_t nr_fcntl = 25, nr_read = 63;
constexpr std::uint64_t f_getfd = 1, f_setfl = 4, fd_cloexec = 1, o_nonblock = 04000;
constexpr long enoent = 2, eagain = 11, ebusy = 16, einval = 22;

template <typename T>
std::uint64_t address(T *pointer) {
    return reinterpret_cast<std::uint64_t>(pointer);
}

/// A syscall of the calling host thread, straight into the dispatcher.
long call(std::uint64_t number, std::uint64_t a0 = 0, std::uint64_t a1 = 0, std::uint64_t a2 = 0) {
    apkrt_syscall_frame frame{};
    frame.x[0] = a0;
    frame.x[1] = a1;
    frame.x[2] = a2;
    frame.x[8] = number;
    apkrt_syscall_dispatch(&frame);
    return static_cast<long>(frame.x[0]);
}

template <typename Condition>
bool within(std::chrono::nanoseconds limit, Condition condition) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (!condition()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::yield();
    }
    return true;
}

/// A request as the host's handler saw it.
struct Request {
    int type;
    std::uint64_t argument;
    std::string text;
    bool had_text;
    pthread_t thread;
    bool runtime_signal_blocked;  // so that none ends the handler's host calls with EINTR
};

/// Whether the calling thread blocks SIGURG, the runtime signal.
bool runtime_signal_blocked() {
    sigset_t blocked;
    ::pthread_sigmask(SIG_BLOCK, nullptr, &blocked);
    return sigismember(&blocked, SIGURG) == 1;
}

std::mutex requests_lock;
std::vector<Request> requests;

// A FRAME_RATE handler that takes its time: it says when it began and when it returns.
std::atomic<bool> slow_frame_rate{false}, frame_rate_began{false}, frame_rate_returned{false};

void on_request(void *context, int type, std::uint64_t argument, const char *text) {
    CHECK(context == &requests);
    {
        const std::lock_guard<std::mutex> hold(requests_lock);
        requests.push_back(
            {type, argument, text != nullptr ? text : "", text != nullptr, ::pthread_self(), runtime_signal_blocked()});
    }
    if (type == APKRT_REQUEST_FRAME_RATE && slow_frame_rate.load()) {
        frame_rate_began.store(true);
        std::this_thread::sleep_for(200ms);
        frame_rate_returned.store(true);
    }
}

/// The requests seen since the last call.
std::vector<Request> take_requests() {
    const std::lock_guard<std::mutex> hold(requests_lock);
    return std::exchange(requests, {});
}

void on_end(void *, int) {}

apkrt_configuration iphone() {
    apkrt_configuration configuration{};
    configuration.scale = 3.0;
    configuration.width = 1290;
    configuration.height = 2796;
    configuration.orientation = 1;  // ACONFIGURATION_ORIENTATION_PORT
    configuration.night = 2;        // ACONFIGURATION_UI_MODE_NIGHT_YES
    configuration.refresh_rate = 120;
    std::strcpy(configuration.language, "de");
    std::strcpy(configuration.country, "DE");
    return configuration;
}

int events = -1;  // activity-host's descriptor of the event pipe

/// Reads `count` records from the event pipe.
std::vector<apkrt_event> read_events(std::size_t count) {
    std::vector<apkrt_event> records(count);
    const auto size = static_cast<long>(count * sizeof(apkrt_event));
    CHECK(call(nr_read, static_cast<std::uint64_t>(events), address(records.data()), static_cast<std::uint64_t>(size)) ==
          size);
    return records;
}

bool is(const apkrt_event &record, std::uint32_t type, std::uint32_t serial, std::uint64_t a0 = 0, std::uint64_t a1 = 0,
        std::uint64_t a2 = 0) {
    return record.type == type && record.serial == serial && record.arguments[0] == a0 && record.arguments[1] == a1 &&
           record.arguments[2] == a2;
}

std::uint64_t pack(std::uint32_t low, std::uint32_t high) {
    return low | static_cast<std::uint64_t>(high) << 32;
}

// --- Without a session --------------------------------------------------------------------------

TEST(without_a_session_nothing_goes_through) {
    CHECK(apkrt_session_post(APKRT_EVENT_CREATE, 0, 0, 0) == 0);
    CHECK(apkrt_session_wait(1, 0.01) == -1);
    apkrt_configuration received{};
    CHECK(call(APKRT_NR_SESSION_CONNECT, address(&received)) == -enoent);
    CHECK(call(APKRT_NR_SESSION_ANSWER, 1) == -enoent);
    CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_FINISH) == -enoent);
    const auto window = apkrt_window_add(nullptr, 64, 64);
    CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_FRAME_RATE, window, 0x42700000) == -enoent);  // 60.0f
    apkrt_window_remove(window);
}

// --- Events and answers -------------------------------------------------------------------------

TEST(events_posted_before_the_connect_wait_in_the_pipe) {
    apkrt_session session{};
    session.context = &requests;
    session.end = on_end;
    session.request = on_request;
    session.configuration = iphone();
    CHECK(apkrt_session_begin(&session) == 0);
    session.configuration.width = 1;  // the runtime keeps its own copy
    CHECK(apkrt_session_post(APKRT_EVENT_CREATE, 0, 0, 0) == 1);
    apkrt_configuration received{};
    const long descriptor = call(APKRT_NR_SESSION_CONNECT, address(&received));
    CHECK(descriptor >= 0);
    events = static_cast<int>(descriptor);
    const auto expected = iphone();
    CHECK(received.scale == expected.scale);
    CHECK(received.width == expected.width && received.height == expected.height);
    CHECK(received.orientation == expected.orientation && received.night == expected.night);
    CHECK(received.refresh_rate == expected.refresh_rate);
    CHECK(std::strcmp(received.language, "de") == 0 && std::strcmp(received.country, "DE") == 0);
    if (events < 0) return;
    const auto records = read_events(1);
    CHECK(is(records[0], APKRT_EVENT_CREATE, 1));
}

TEST(a_second_connect_is_refused_and_the_descriptor_closes_on_exec) {
    apkrt_configuration received{};
    CHECK(call(APKRT_NR_SESSION_CONNECT, address(&received)) == -ebusy);
    CHECK(call(nr_fcntl, static_cast<std::uint64_t>(events), f_getfd) == fd_cloexec);
}

TEST(events_arrive_in_order_with_their_serial_numbers_and_arguments) {
    CHECK(apkrt_session_post(APKRT_EVENT_START, 0, 0, 0) == 2);
    CHECK(apkrt_session_post(APKRT_EVENT_RESUME, 0, 0, 0) == 3);
    CHECK(apkrt_session_post(APKRT_EVENT_NATIVE_WINDOW_CREATED, 7, 1290, 2796) == 4);
    CHECK(apkrt_session_post(APKRT_EVENT_CONTENT_RECT_CHANGED, pack(0, 0), pack(1290, 2796), 0) == 5);
    CHECK(apkrt_session_post(APKRT_EVENT_WINDOW_FOCUS_CHANGED, 1, 0, 0) == 6);
    if (events < 0) return;
    const auto records = read_events(5);  // in one read: they are whole records in the pipe's order
    CHECK(is(records[0], APKRT_EVENT_START, 2));
    CHECK(is(records[1], APKRT_EVENT_RESUME, 3));
    CHECK(is(records[2], APKRT_EVENT_NATIVE_WINDOW_CREATED, 4, 7, 1290, 2796));
    CHECK(is(records[3], APKRT_EVENT_CONTENT_RECT_CHANGED, 5, pack(0, 0), pack(1290, 2796)));
    CHECK(is(records[4], APKRT_EVENT_WINDOW_FOCUS_CHANGED, 6, 1));
}

TEST(an_answer_wakes_the_wait) {
    const auto serial = apkrt_session_post(APKRT_EVENT_PAUSE, 0, 0, 0);
    CHECK(serial == 7);
    std::atomic<int> result{2};
    std::thread host([&] { result.store(apkrt_session_wait(serial, 5.0)); });
    std::this_thread::sleep_for(50ms);
    CHECK(result.load() == 2);
    CHECK(call(APKRT_NR_SESSION_ANSWER, serial) == 0);
    CHECK(within(1s, [&] { return result.load() != 2; }));
    host.join();
    CHECK(result.load() == 1);
}

TEST(answers_come_in_order_so_a_later_answer_counts_for_the_events_before_it) {
    const auto stop = apkrt_session_post(APKRT_EVENT_STOP, 0, 0, 0);
    const auto save = apkrt_session_post(APKRT_EVENT_SAVE_INSTANCE_STATE, 0, 0, 0);
    CHECK(stop == 8 && save == 9);
    CHECK(call(APKRT_NR_SESSION_ANSWER, save) == 0);
    // At once, not only once the time is up.
    const auto start = std::chrono::steady_clock::now();
    CHECK(apkrt_session_wait(stop, 5.0) == 1);
    CHECK(apkrt_session_wait(save, 5.0) == 1);
    CHECK(apkrt_session_wait(1, 5.0) == 1);
    CHECK(std::chrono::steady_clock::now() - start < 1s);
    // A later answer to an earlier event takes nothing back.
    CHECK(call(APKRT_NR_SESSION_ANSWER, stop) == 0);
    CHECK(apkrt_session_wait(save, 0) == 1);
}

TEST(a_wait_ends_at_its_time_limit) {
    const auto serial = apkrt_session_post(APKRT_EVENT_LOW_MEMORY, 0, 0, 0);
    CHECK(serial == 10);
    const auto start = std::chrono::steady_clock::now();
    CHECK(apkrt_session_wait(serial, 0.2) == 0);
    const auto waited = std::chrono::steady_clock::now() - start;
    CHECK(waited >= 200ms && waited < 2s);
    CHECK(apkrt_session_wait(serial, 0) == 0);
}

TEST(an_answer_to_an_event_not_posted_is_refused) {
    CHECK(call(APKRT_NR_SESSION_ANSWER, 1000) == -einval);
    CHECK(call(APKRT_NR_SESSION_ANSWER, 0) == -einval);
    CHECK(apkrt_session_wait(10, 0) == 0);  // nothing changed
    CHECK(call(APKRT_NR_SESSION_ANSWER, 10) == 0);
    CHECK(apkrt_session_wait(10, 0) == 1);
}

TEST(a_wait_for_no_event_or_one_not_posted_fails_at_once) {
    // 0 is what a failed post returns.
    const auto start = std::chrono::steady_clock::now();
    CHECK(apkrt_session_wait(0, 5.0) == -1);
    CHECK(apkrt_session_wait(1000, 5.0) == -1);
    CHECK(std::chrono::steady_clock::now() - start < 1s);
}

TEST(a_wait_without_a_time_limit_waits_for_the_answer) {
    const auto serial = apkrt_session_post(APKRT_EVENT_CONFIGURATION_CHANGED, 0, 0, 0);
    CHECK(serial == 11);
    std::atomic<int> result{2};
    std::thread host([&] { result.store(apkrt_session_wait(serial, INFINITY)); });
    std::this_thread::sleep_for(50ms);
    CHECK(result.load() == 2);
    CHECK(call(APKRT_NR_SESSION_ANSWER, serial) == 0);
    CHECK(within(1s, [&] { return result.load() != 2; }));
    host.join();
    CHECK(result.load() == 1);
    // A time that is no number, or below 0, is none.
    const auto next = apkrt_session_post(APKRT_EVENT_CONFIGURATION_CHANGED, 0, 0, 0);
    const auto start = std::chrono::steady_clock::now();
    CHECK(apkrt_session_wait(next, NAN) == 0);
    CHECK(apkrt_session_wait(next, -1.0) == 0);
    CHECK(std::chrono::steady_clock::now() - start < 1s);
}

// --- Requests -----------------------------------------------------------------------------------

TEST(requests_reach_the_handler_on_the_calling_thread) {
    take_requests();
    CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_FINISH) == 0);
    static const char reason[] = "dlopen failed: library \"libmain.so\" not found";
    CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_START_FAILED, address(reason)) == 0);
    const auto seen = take_requests();
    CHECK(seen.size() == 2);
    if (seen.size() != 2) return;
    CHECK(seen[0].type == APKRT_REQUEST_FINISH && seen[0].argument == 0 && !seen[0].had_text);
    CHECK(seen[1].type == APKRT_REQUEST_START_FAILED && seen[1].had_text && seen[1].text == reason);
    for (const auto &request : seen) CHECK(request.thread == ::pthread_self() && request.runtime_signal_blocked);
    CHECK(!runtime_signal_blocked());  // as before the request
}

TEST(a_request_of_another_type_is_refused) {
    take_requests();
    CHECK(call(APKRT_NR_SESSION_REQUEST, 0) == -einval);
    CHECK(call(APKRT_NR_SESSION_REQUEST, 99) == -einval);
    CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_KEEP_SCREEN_ON, 1) == -einval);  // the host's
    CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_WINDOW_OPAQUE, 1) == -einval);   // the host's too
    CHECK(take_requests().empty());
}

TEST(keep_screen_on_is_reported_only_when_it_changes) {
    constexpr std::uint64_t keep_screen_on = 0x80, fullscreen = 0x400;
    const auto flags = [](std::uint64_t add, std::uint64_t remove) {
        CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_WINDOW_FLAGS, add, remove) == 0);
        return take_requests();
    };
    take_requests();
    const auto on = flags(keep_screen_on, 0);
    CHECK(on.size() == 1 && on[0].type == APKRT_REQUEST_KEEP_SCREEN_ON && on[0].argument == 1 && !on[0].had_text);
    CHECK(flags(keep_screen_on, 0).empty());
    CHECK(flags(fullscreen, 0).empty());
    CHECK(flags(0, fullscreen).empty());
    const auto off = flags(0, keep_screen_on);
    CHECK(off.size() == 1 && off[0].type == APKRT_REQUEST_KEEP_SCREEN_ON && off[0].argument == 0);
    CHECK(flags(0, keep_screen_on).empty());
    const auto together = flags(keep_screen_on | fullscreen, 0);
    CHECK(together.size() == 1 && together[0].argument == 1);
    // Added and removed at once: Android removes first, then adds (Window.setFlags(add, add | remove)).
    CHECK(flags(keep_screen_on, keep_screen_on).empty());
    const auto last = flags(0, keep_screen_on | fullscreen);
    CHECK(last.size() == 1 && last[0].argument == 0);
}

TEST(keep_screen_on_is_reported_in_the_order_it_changes_across_threads) {
    // Two threads turn the flag on and off at once. Every report is a change, so on and off alternate,
    // and the last report is what the flags hold.
    constexpr std::uint64_t keep_screen_on = 0x80;
    constexpr int rounds = 20'000;
    take_requests();
    std::atomic<bool> go{false};
    const auto toggle = [&go](std::uint64_t add, std::uint64_t remove) {
        return std::thread([&go, add, remove] {
            while (!go.load()) {
            }
            for (int round = 0; round < rounds; ++round) {
                call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_WINDOW_FLAGS, add, remove);
            }
        });
    };
    std::thread on = toggle(keep_screen_on, 0), off = toggle(0, keep_screen_on);
    go.store(true);
    on.join();
    off.join();
    const auto seen = take_requests();
    bool alternating = true;
    std::uint64_t expected = 1;  // the flag was off before
    for (const auto &request : seen) {
        alternating = alternating && request.type == APKRT_REQUEST_KEEP_SCREEN_ON && request.argument == expected;
        expected ^= 1;
    }
    CHECK(alternating);
    // Turning it off now reports a change exactly when the last report said on.
    CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_WINDOW_FLAGS, 0, keep_screen_on) == 0);
    CHECK(take_requests().size() == (seen.size() % 2 == 1 ? 1U : 0U));
}

// --- The window's format (Grafik's opaque layer, after Android 15's ViewRootImpl.updateOpacity) ---------

// PixelFormat's values (android.graphics.PixelFormat at android-15.0.0_r1).
constexpr std::int32_t translucent = -3, transparent = -2, opaque = -1, unknown = 0, rgba_8888 = 1, rgbx_8888 = 2,
                       rgb_888 = 3, rgb_565 = 4, rgba_5551 = 6, rgba_4444 = 7, a_8 = 8, l_8 = 9, la_88 = 0xA,
                       rgb_332 = 0xB, ycbcr_420_sp = 0x11, rgba_f16 = 0x16, rgba_1010102 = 0x2B;

/// Asks for the window format `format`, as ANativeActivity_setWindowFormat passes an int32_t, and returns the
/// requests the host saw.
std::vector<Request> window_format(std::int32_t format) {
    CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_WINDOW_FORMAT, static_cast<std::uint64_t>(format)) == 0);
    return take_requests();
}

/// Whether `seen` is exactly one WINDOW_OPAQUE report with the argument `value`.
bool reported_opaque(const std::vector<Request> &seen, std::uint64_t value) {
    return seen.size() == 1 && seen[0].type == APKRT_REQUEST_WINDOW_OPAQUE && seen[0].argument == value &&
           !seen[0].had_text && seen[0].runtime_signal_blocked;
}

TEST(the_window_starts_opaque_and_only_a_change_of_its_opacity_is_reported) {
    // The session began with RGB_565, NativeActivity's format, and reported nothing for it.
    for (const auto &request : take_requests()) CHECK(request.type != APKRT_REQUEST_WINDOW_OPAQUE);
    CHECK(window_format(rgbx_8888).empty());
    CHECK(window_format(rgb_565).empty());
    CHECK(reported_opaque(window_format(rgba_8888), 0));
    CHECK(window_format(rgba_8888).empty());
    CHECK(window_format(translucent).empty());  // another format, but with alpha too
    CHECK(reported_opaque(window_format(rgb_565), 1));
    CHECK(window_format(rgbx_8888).empty());
    // Only the low 32 bits count, as libandroid passes an int32_t.
    CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_WINDOW_FORMAT, 0xFFFF'FFFDULL) == 0);  // -3
    CHECK(reported_opaque(take_requests(), 0));
    CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_WINDOW_FORMAT, 0x1'0000'0004ULL) == 0);  // 4
    CHECK(reported_opaque(take_requests(), 1));
}

TEST(a_format_makes_the_window_non_opaque_exactly_when_format_has_alpha_says_so) {
    for (const auto format : {a_8, la_88, rgba_4444, rgba_5551, rgba_8888, rgba_f16, rgba_1010102, translucent,
                              transparent}) {
        CHECK(reported_opaque(window_format(format), 0));
        CHECK(reported_opaque(window_format(rgb_565), 1));
    }
    // Every other value is opaque, also one PixelFormat does not know.
    for (const auto format : {opaque, unknown, rgbx_8888, rgb_888, rgb_565, l_8, rgb_332, ycbcr_420_sp, 12345}) {
        CHECK(window_format(format).empty());
    }
}

TEST(the_window_s_opacity_is_reported_in_the_order_it_changes_across_threads) {
    // Two threads ask for a format with alpha and one without at once. Every report is a change, so 0 and 1
    // alternate, and the last report is what the format says.
    constexpr int rounds = 20'000;
    take_requests();
    std::atomic<bool> go{false};
    const auto ask = [&go](std::int32_t format) {
        return std::thread([&go, format] {
            while (!go.load()) {
            }
            for (int round = 0; round < rounds; ++round) {
                call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_WINDOW_FORMAT, static_cast<std::uint64_t>(format));
            }
        });
    };
    std::thread with_alpha = ask(rgba_8888), without = ask(rgb_565);
    go.store(true);
    with_alpha.join();
    without.join();
    const auto seen = take_requests();
    bool alternating = true;
    std::uint64_t expected = 0;  // the window was opaque before
    for (const auto &request : seen) {
        alternating = alternating && request.type == APKRT_REQUEST_WINDOW_OPAQUE && request.argument == expected;
        expected ^= 1;
    }
    CHECK(alternating);
    // RGB_565 now reports a change exactly when the last report said non-opaque.
    CHECK(window_format(rgb_565).size() == (seen.size() % 2 == 1 ? 1U : 0U));
}

// --- The frame rate (ADR-0036, decision 2) ------------------------------------------------------

/// A float's bits, as libnativewindow passes a rate and the host gets it.
std::uint64_t bits(float rate) {
    std::uint32_t value;
    std::memcpy(&value, &rate, sizeof value);
    return value;
}

/// ANativeWindow_setFrameRate's request for `window`, and what reached the host.
std::vector<Request> frame_rate(std::uint64_t window, float rate) {
    CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_FRAME_RATE, window, bits(rate)) == 0);
    return take_requests();
}

bool reported(const std::vector<Request> &seen, float rate) {
    return seen.size() == 1 && seen[0].type == APKRT_REQUEST_FRAME_RATE && seen[0].argument == bits(rate) &&
           !seen[0].had_text && seen[0].thread == ::pthread_self() && seen[0].runtime_signal_blocked;
}

TEST(a_windows_frame_rate_is_reported_when_it_changes) {
    const auto window = apkrt_window_add(nullptr, 1290, 2796);
    take_requests();
    CHECK(reported(frame_rate(window, 60), 60));
    CHECK(frame_rate(window, 60).empty());
    CHECK(reported(frame_rate(window, 0), 0));  // no preference
    CHECK(frame_rate(window, 0).empty());
    CHECK(reported(frame_rate(window, 120), 120));
    CHECK(reported(frame_rate(window, 59.94f), 59.94f));
    apkrt_window_remove(window);
}

TEST(a_new_window_starts_without_a_frame_rate_and_each_keeps_its_own) {
    const auto first = apkrt_window_add(nullptr, 1290, 2796);
    take_requests();
    CHECK(frame_rate(first, 0).empty());
    CHECK(reported(frame_rate(first, 60), 60));
    const auto second = apkrt_window_add(nullptr, 1290, 2796);
    CHECK(reported(frame_rate(second, 60), 60));
    CHECK(frame_rate(first, 60).empty());
    CHECK(reported(frame_rate(first, 30), 30));
    CHECK(frame_rate(second, 60).empty());
    apkrt_window_remove(first);
    apkrt_window_remove(second);
}

TEST(the_removal_of_a_window_waits_for_its_frame_rate_report_under_way) {
    const auto window = apkrt_window_add(nullptr, 1290, 2796);
    take_requests();
    slow_frame_rate.store(true);
    std::thread guest([window] { call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_FRAME_RATE, window, bits(60)); });
    CHECK(within(2s, [] { return frame_rate_began.load(); }));
    CHECK(apkrt_window_remove(window) == 0);
    CHECK(frame_rate_returned.load());
    guest.join();
    slow_frame_rate.store(false);
    const auto seen = take_requests();
    CHECK(seen.size() == 1 && seen[0].type == APKRT_REQUEST_FRAME_RATE && seen[0].argument == bits(60));
    // None starts once the removal has returned.
    CHECK(frame_rate(window, 120).empty());
}

TEST(no_frame_rate_is_reported_for_a_removed_or_unknown_window) {
    const auto window = apkrt_window_add(nullptr, 1290, 2796);
    CHECK(apkrt_window_remove(window) == 0);
    take_requests();
    CHECK(frame_rate(window, 60).empty());
    CHECK(frame_rate(window + 1000, 60).empty());
    CHECK(frame_rate(0, 60).empty());
}

// --- Last: the full pipe ------------------------------------------------------------------------

TEST(a_full_pipe_makes_the_post_fail_at_once_and_every_later_post_too) {
    if (events < 0) {
        CHECK(events >= 0);
        return;
    }
    const auto fd = static_cast<std::uint64_t>(events);
    CHECK(call(nr_fcntl, fd, f_setfl, o_nonblock) == 0);
    // What the tests before posted and did not read: from PAUSE (7) on, in order.
    apkrt_event record{};
    std::uint32_t last = 6;
    bool leftovers_in_order = true;
    for (long got; (got = call(nr_read, fd, address(&record), sizeof record)) != -eagain; ++last) {
        leftovers_in_order = leftovers_in_order && got == static_cast<long>(sizeof record) && record.serial == last + 1;
        if (got <= 0) break;
    }
    CHECK(leftovers_in_order && last >= 12);

    // A guest that stops reading: the posts fill the pipe, and the first that does not fit fails at once.
    const auto start = std::chrono::steady_clock::now();
    std::uint32_t posted = 0;
    bool in_order = true;
    for (std::uint32_t serial; posted < 100'000; ++posted) {
        serial = apkrt_session_post(APKRT_EVENT_NATIVE_WINDOW_REDRAW_NEEDED, 7, 0, 0);
        if (serial == 0) break;
        in_order = in_order && serial == last + 1 + posted;
    }
    CHECK(std::chrono::steady_clock::now() - start < 2s);
    CHECK(posted > 0 && posted < 100'000);
    CHECK(in_order);

    // What went in are whole records in order.
    std::uint32_t taken = 0;
    bool whole = true;
    for (long got; (got = call(nr_read, fd, address(&record), sizeof record)) != -eagain; ++taken) {
        whole = whole && got == static_cast<long>(sizeof record) && is(record, APKRT_EVENT_NATIVE_WINDOW_REDRAW_NEEDED,
                                                                        last + 1 + taken, 7);
        if (got <= 0) break;
    }
    CHECK(whole && taken == posted);

    // The failure is final, although the pipe is empty again, and a wait for an answer ends at once.
    CHECK(apkrt_session_post(APKRT_EVENT_PAUSE, 0, 0, 0) == 0);
    const auto waited = std::chrono::steady_clock::now();
    CHECK(apkrt_session_wait(last + posted, 5.0) == -1);
    CHECK(std::chrono::steady_clock::now() - waited < 1s);
}

}  // namespace

int main() {
    // The test's own thread is the program's first thread, as in the other tests of the syscall layer.
    static apkrt::threads::GuestThread first{};
    first.tid = ::getpid();
    apkrt::threads::add(first);
    apkrt::signals::install();
    return apk::testing::run_all();
}
