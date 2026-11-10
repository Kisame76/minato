// Checks the session's channel (ADR-0028, decisions 2 to 4): events arrive in order at the guest
// descriptor that session_connect gives, with the session's configuration, which session_configuration gives
// again and again without the pipe (ADR-0039, config-test); an answer wakes the host's
// wait, which ends at its time limit otherwise; a full pipe makes the post fail instead of blocking, and
// every later post fails too; the guest's requests reach the host's handler, KEEP_SCREEN_ON only when it
// changes and WINDOW_OPAQUE only when the window's format changes its opacity. A process has one session,
// so the tests build on each other in their order: first without a session, last the full pipe. The test's
// thread plays activity-host and calls the dispatcher directly, as syscalls_test.cpp does. The program's end
// is program_test.cpp's (--session-ends).
#include "signals.h"
#include "syscall_frame.h"
#include "threads.h"
#include "permissions.h"
#include "install_sources.h"

#include "APKRuntime/apkrt.h"
#include "check.h"

#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

// Linux: include/uapi/asm-generic/unistd.h, fcntl.h, errno.h.
constexpr std::uint64_t nr_fcntl = 25, nr_read = 63;
constexpr std::uint64_t f_getfd = 1, f_setfl = 4, fd_cloexec = 1, o_nonblock = 04000;
constexpr long enoent = 2, eagain = 11, enomem = 12, eacces = 13, efault = 14, ebusy = 16, eexist = 17, einval = 22,
               enospc = 28, eopnotsupp = 95;

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
// A PICK or OPEN handler that takes its time, and counts the handlers that run at once.
std::atomic<bool> slow_pick{false}, pick_overlap{false};
std::atomic<int> pick_in_handler{0};
// A PICK handler that waits for another thread to have posted the answer (and says whether that happened).
std::atomic<bool> pick_waits_for_post{false}, pick_posted{false}, pick_saw_post{false};
// An OPEN handler that answers at once, inside the handler.
std::atomic<bool> open_answers_at_once{false};

// The allocations a thread may still make before the next one fails with std::bad_alloc, -1 for no limit: the
// requests' checks run inside the extern "C" dispatch, which must answer -ENOMEM rather than let an exception out.
thread_local long allocations_left = -1;

/// Lifts the limit for as long as it lives, for the test's own handler.
struct Unlimited {
    long saved = std::exchange(allocations_left, -1L);
    ~Unlimited() { allocations_left = saved; }
};

}  // namespace

void *operator new(std::size_t size) {
    if (allocations_left == 0) throw std::bad_alloc();
    if (allocations_left > 0) --allocations_left;
    if (void *memory = std::malloc(size == 0 ? 1 : size)) return memory;
    throw std::bad_alloc();
}
void *operator new[](std::size_t size) { return ::operator new(size); }
void *operator new(std::size_t size, const std::nothrow_t &) noexcept {
    try {
        return ::operator new(size);
    } catch (const std::bad_alloc &) {
        return nullptr;
    }
}
void *operator new[](std::size_t size, const std::nothrow_t &tag) noexcept { return ::operator new(size, tag); }
void operator delete(void *memory) noexcept { std::free(memory); }
void operator delete[](void *memory) noexcept { std::free(memory); }
void operator delete(void *memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void *memory, std::size_t) noexcept { std::free(memory); }
void operator delete(void *memory, const std::nothrow_t &) noexcept { std::free(memory); }
void operator delete[](void *memory, const std::nothrow_t &) noexcept { std::free(memory); }

namespace {

void on_request(void *context, int type, std::uint64_t argument, const char *text) {
    const Unlimited unlimited;
    CHECK(context == &requests);
    {
        const std::lock_guard<std::mutex> hold(requests_lock);
        requests.push_back(
            {type, argument, text != nullptr ? text : "", text != nullptr, ::pthread_self(), runtime_signal_blocked()});
    }
    if ((type == APKRT_REQUEST_PICK || type == APKRT_REQUEST_OPEN || type == APKRT_REQUEST_EXPORT_MEDIA) &&
        slow_pick.load()) {
        if (pick_in_handler.fetch_add(1) != 0) pick_overlap.store(true);
        std::this_thread::sleep_for(30ms);
        pick_in_handler.fetch_sub(1);
    }
    if (type == APKRT_REQUEST_PICK && pick_waits_for_post.load()) {
        for (int i = 0; i < 200 && !pick_posted.load(); ++i) std::this_thread::sleep_for(10ms);
        pick_saw_post.store(pick_posted.load());
    }
    if (type == APKRT_REQUEST_OPEN && open_answers_at_once.load()) {
        CHECK(apkrt_session_post(APKRT_EVENT_OPEN_RESULT, argument, 0, 3) != 0);
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

/// A surface request as the host's surface handler saw it (ADR-0043 c4).
struct Surface {
    apkrt_surface_request request;
    pthread_t thread;
    bool runtime_signal_blocked;
};

std::vector<Surface> surfaces;  // under requests_lock

void on_surface(void *context, const apkrt_surface_request *request) {
    CHECK(context == &requests);
    const std::lock_guard<std::mutex> hold(requests_lock);
    surfaces.push_back({*request, ::pthread_self(), runtime_signal_blocked()});
}

/// The surface requests seen since the last call.
std::vector<Surface> take_surfaces() {
    const std::lock_guard<std::mutex> hold(requests_lock);
    return std::exchange(surfaces, {});
}

/// A surface request as SurfaceView makes one: a media surface below the window, RGB_565, its frame's size.
apkrt_surface_request surface(std::uint32_t op, std::uint64_t token) {
    apkrt_surface_request request{};
    request.op = op;
    request.z = -2;
    request.token = token;
    request.left = 0;
    request.top = 300;
    request.right = 1290;
    request.bottom = 1026;
    request.format = 4;  // PixelFormat.RGB_565
    return request;
}

/// session_request's answer to a surface request at `address`.
long ask_at(std::uint64_t at) {
    return call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_SURFACE, at);
}

long ask(const apkrt_surface_request &request) {
    return ask_at(address(&request));
}

bool same(const apkrt_surface_request &a, const apkrt_surface_request &b) {
    return std::memcmp(&a, &b, sizeof a) == 0;
}

// A window request as the host's window handler saw it (ADR-0041, gap 4), with its fixed layout, which both sides
// assert.
static_assert(sizeof(apkrt_window_request) == 72);
static_assert(offsetof(apkrt_window_request, op) == 0 && offsetof(apkrt_window_request, type) == 4 &&
              offsetof(apkrt_window_request, token) == 8 && offsetof(apkrt_window_request, parent) == 16 &&
              offsetof(apkrt_window_request, left) == 24 && offsetof(apkrt_window_request, top) == 28 &&
              offsetof(apkrt_window_request, right) == 32 && offsetof(apkrt_window_request, bottom) == 36 &&
              offsetof(apkrt_window_request, width) == 40 && offsetof(apkrt_window_request, height) == 44 &&
              offsetof(apkrt_window_request, format) == 48 && offsetof(apkrt_window_request, opaque) == 52 &&
              offsetof(apkrt_window_request, group) == 56 && offsetof(apkrt_window_request, alpha) == 60 &&
              offsetof(apkrt_window_request, dim) == 64 && offsetof(apkrt_window_request, reserved) == 68);

struct Window {
    apkrt_window_request request;
    pthread_t thread;
    bool runtime_signal_blocked;
};

std::vector<Window> windows;  // under requests_lock

void on_window(void *context, const apkrt_window_request *request) {
    CHECK(context == &requests);
    const std::lock_guard<std::mutex> hold(requests_lock);
    windows.push_back({*request, ::pthread_self(), runtime_signal_blocked()});
}

/// The window requests seen since the last call.
std::vector<Window> take_windows() {
    const std::lock_guard<std::mutex> hold(requests_lock);
    return std::exchange(windows, {});
}

/// A window request as a dialog of the session's activity makes one: TYPE_APPLICATION, PixelFormat.TRANSLUCENT, its
/// frame's size, alpha 1 and AlertDialog's dim 0.6.
apkrt_window_request window_of(std::uint32_t op, std::uint64_t token, std::int32_t type = 2) {
    apkrt_window_request request{};
    request.op = op;
    request.type = type;
    request.token = token;
    request.left = 96;
    request.top = 1000;
    request.right = 1194;
    request.bottom = 1796;
    request.format = -3;
    request.alpha = 0x3f800000;  // 1.0f
    request.dim = 0x3f19999a;    // 0.6f
    return request;
}

/// session_request's answer to a window request at `address`.
long ask_window_at(std::uint64_t at) {
    return call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_WINDOW, at);
}

long ask_window(const apkrt_window_request &request) {
    return ask_window_at(address(&request));
}

bool same(const apkrt_window_request &a, const apkrt_window_request &b) {
    return std::memcmp(&a, &b, sizeof a) == 0;
}

/// Runs `body` in a child process and returns whether its checks passed there: for a session of the child's own,
/// as a process has one. It runs on a thread of its own, as descriptors_test.cpp's does.
template <typename Body>
bool in_child(Body body) {
    std::fflush(nullptr);
    const pid_t child = ::fork();
    if (child == 0) {
        apk::testing::failures = 0;  // the child's checks alone
        std::thread(body).join();
        std::fflush(nullptr);
        ::_exit(apk::testing::failures == 0 ? 0 : 1);
    }
    int status = 0;
    ::waitpid(child, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
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

bool same(const apkrt_configuration &a, const apkrt_configuration &b) {
    return a.scale == b.scale && a.width == b.width && a.height == b.height && a.orientation == b.orientation &&
           a.night == b.night && a.refresh_rate == b.refresh_rate && std::strcmp(a.language, b.language) == 0 &&
           std::strcmp(a.country, b.country) == 0;
}

// The apkrt_configuration bytes before its tail padding; begin() clears the padding so no uninitialized host
// bytes reach the guest through connect or configuration.
constexpr std::size_t configuration_fields =
    offsetof(apkrt_configuration, country) + sizeof(apkrt_configuration::country);

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

// Placed before the first test that begins a session in this process: in_child forks and inherits the
// one-session-per-process claim, so a begin after one in the parent is refused in every child.
// ADR-0067 decision 3: under a render scale the guest's view of the session's configuration is the scaled screen.
// apkrt_session_set_render_scale sets the scale (host-facing, 0 / -1 as the window's setter), which the runtime
// applies to what connect and configuration give.

/// The configuration the guest gets from session_configuration, after the host set `scale` (before or after begin).
apkrt_configuration scaled_view(double scale, bool before_begin) {
    apkrt_session session{};
    session.end = on_end;
    session.configuration = iphone();
    if (before_begin) CHECK(apkrt_session_set_render_scale(scale) == 0);
    CHECK(apkrt_session_begin(&session) == 0);
    if (!before_begin) CHECK(apkrt_session_set_render_scale(scale) == 0);
    apkrt_configuration received{};
    CHECK(call(APKRT_NR_SESSION_CONFIGURATION, address(&received)) == 0);
    return received;
}

TEST(a_render_scale_scales_the_configurations_size_and_density_and_nothing_else) {
    for (const bool before : {true, false}) {
        CHECK(in_child([before] {
            const apkrt_configuration view = scaled_view(0.5, before);
            const apkrt_configuration full = iphone();
            CHECK(view.width == 645 && view.height == 1398);  // 1290 and 2796 times 0.5
            CHECK(view.scale == 1.5);                          // the density follows: 480 dpi becomes 240
            CHECK(view.orientation == full.orientation && view.night == full.night);
            CHECK(view.refresh_rate == full.refresh_rate);
            CHECK(std::strcmp(view.language, "de") == 0 && std::strcmp(view.country, "DE") == 0);
            // dp are the same as unscaled: 645 / 1.5 = 430 as 1290 / 3
            CHECK(static_cast<double>(view.width) / view.scale == static_cast<double>(full.width) / full.scale);
        }));
    }
}

TEST(a_render_scale_rounds_the_size_and_is_at_least_one_pixel) {
    CHECK(in_child([] {
        const apkrt_configuration view = scaled_view(0.67, true);
        CHECK(view.width == 864 && view.height == 1873);  // 864.3 and 1873.32
        CHECK(view.scale == 3.0 * 0.67);
    }));
}

TEST(a_render_scale_of_1_changes_nothing) {
    CHECK(in_child([] {
        const apkrt_configuration view = scaled_view(1.0, true);
        CHECK(same(view, iphone()));
    }));
}

TEST(a_render_scale_out_of_range_is_refused_and_keeps_the_one_before) {
    CHECK(in_child([] {
        CHECK(apkrt_session_set_render_scale(0.5) == 0);
        for (const double bad : {0.0, 0.1, 0.2499, 1.0001, 2.0, -1.0, std::nan(""), HUGE_VAL}) {
            CHECK(apkrt_session_set_render_scale(bad) == -1);
        }
        apkrt_session session{};
        session.end = on_end;
        session.configuration = iphone();
        CHECK(apkrt_session_begin(&session) == 0);
        apkrt_configuration received{};
        CHECK(call(APKRT_NR_SESSION_CONFIGURATION, address(&received)) == 0);
        CHECK(received.width == 645);
    }));
}

TEST(without_a_session_nothing_goes_through) {
    CHECK(apkrt_session_post(APKRT_EVENT_CREATE, 0, 0, 0) == 0);
    CHECK(apkrt_session_wait(1, 0.01) == -1);
    apkrt_configuration received{};
    CHECK(call(APKRT_NR_SESSION_CONNECT, address(&received)) == -enoent);
    CHECK(call(APKRT_NR_SESSION_CONFIGURATION, address(&received)) == -enoent);
    CHECK(call(APKRT_NR_SESSION_ANSWER, 1) == -enoent);
    CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_FINISH) == -enoent);
    const auto window = apkrt_window_add(nullptr, 64, 64);
    CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_FRAME_RATE, window, 0x42700000) == -enoent);  // 60.0f
    apkrt_window_remove(window);
    CHECK(ask(surface(APKRT_SURFACE_MAKE, 7)) == -enoent);
    CHECK(ask_window(window_of(APKRT_WINDOW_MAKE, 7)) == -enoent);
}

TEST(a_host_without_a_surface_handler_refuses_every_surface_request) {
    CHECK(in_child([] {
        apkrt_session session{};
        session.end = on_end;
        session.configuration = iphone();
        CHECK(apkrt_session_begin(&session) == 0);
        CHECK(ask(surface(APKRT_SURFACE_MAKE, 7)) == -eopnotsupp);
        CHECK(ask_at(0) == -eopnotsupp);
    }));
}

TEST(a_host_without_a_window_handler_refuses_every_window_request) {
    CHECK(in_child([] {
        apkrt_session session{};
        session.end = on_end;
        session.surface = on_surface;
        session.configuration = iphone();
        CHECK(apkrt_session_begin(&session) == 0);
        CHECK(ask_window(window_of(APKRT_WINDOW_MAKE, 7)) == -eopnotsupp);
        CHECK(ask_window_at(0) == -eopnotsupp);
    }));
}

// --- Events and answers -------------------------------------------------------------------------

TEST(events_posted_before_the_connect_wait_in_the_pipe) {
    apkrt_session session{};
    session.context = &requests;
    session.end = on_end;
    session.request = on_request;
    session.surface = on_surface;
    session.window = on_window;
    session.configuration = iphone();
    // Dirty the configuration's tail padding before begin, so begin() must clear it: otherwise these
    // uninitialized host bytes would reach the guest through connect and configuration (write-path finding).
    std::memset(reinterpret_cast<char *>(&session.configuration) + configuration_fields, 0xff,
                sizeof session.configuration - configuration_fields);
    CHECK(apkrt_session_begin(&session) == 0);
    session.configuration.width = 1;  // the runtime keeps its own copy
    CHECK(apkrt_session_post(APKRT_EVENT_CREATE, 0, 0, 0) == 1);
    // session_configuration before the connect leaves the connect to activity-host.
    apkrt_configuration early{};
    CHECK(call(APKRT_NR_SESSION_CONFIGURATION, address(&early)) == 0 && same(early, iphone()));
    // A bad configuration pointer faults cleanly and leaves the session unconnected: the checked write
    // runs before the lock and the state change (ADR-0020 write path), so the real connect below still
    // works. On the raw-memcpy version this call crashed the host instead of returning -EFAULT.
    const auto gone_page = static_cast<std::size_t>(::getpagesize());
    void *const gone = ::mmap(nullptr, gone_page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(gone != MAP_FAILED);
    const auto gone_address = reinterpret_cast<std::uint64_t>(gone);
    CHECK(::munmap(gone, gone_page) == 0);  // now unmapped: a non-null pointer that cannot be written
    CHECK(call(APKRT_NR_SESSION_CONNECT, gone_address) == -efault);
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
    // begin() cleared the tail padding, so connect wrote zeros there, not the 0xff we dirtied it with.
    const auto *connect_bytes = reinterpret_cast<const unsigned char *>(&received);
    for (std::size_t i = configuration_fields; i < sizeof received; ++i) CHECK(connect_bytes[i] == 0);
    if (events < 0) return;
    const auto records = read_events(1);
    CHECK(is(records[0], APKRT_EVENT_CREATE, 1));
}

TEST(a_second_connect_is_refused_and_the_descriptor_closes_on_exec) {
    apkrt_configuration received{};
    CHECK(call(APKRT_NR_SESSION_CONNECT, address(&received)) == -ebusy);
    CHECK(call(nr_fcntl, static_cast<std::uint64_t>(events), f_getfd) == fd_cloexec);
}

TEST(the_configuration_comes_again_and_again_without_the_pipe) {
    for (int round = 0; round < 3; ++round) {
        apkrt_configuration received{};
        CHECK(call(APKRT_NR_SESSION_CONFIGURATION, address(&received)) == 0 && same(received, iphone()));
        const auto *bytes = reinterpret_cast<const unsigned char *>(&received);  // tail padding cleared too
        for (std::size_t i = configuration_fields; i < sizeof received; ++i) CHECK(bytes[i] == 0);
    }
    CHECK(call(APKRT_NR_SESSION_CONFIGURATION, 0) == -efault);
}

// SESSION_CONFIGURATION writes the configuration into a guest buffer; a bad guest pointer must give
// -EFAULT, not fault the host (the write goes through a checked copy). The buffer starts in a gone page,
// then straddles the boundary into it.
TEST(a_configuration_write_to_bad_guest_memory_is_efault_not_a_crash) {
    const auto page = static_cast<std::size_t>(::getpagesize());
    void *const pages = ::mmap(nullptr, 2 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(pages != MAP_FAILED);
    const auto first = reinterpret_cast<std::uint64_t>(pages);
    CHECK(::munmap(reinterpret_cast<void *>(first + page), page) == 0);  // the second page is gone
    for (const std::uint64_t dest : {first + page, first + page - 8}) {
        CHECK(call(APKRT_NR_SESSION_CONFIGURATION, dest) == -efault);
    }
    ::munmap(pages, page);
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
    CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_BACK_AT_ROOT) == 0);  // fire-and-forget, like FINISH
    // setRequestedOrientation's value rides in the argument, also a negative one (UNSPECIFIED, -1), sign-extended.
    CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_ORIENTATION, 6) == 0);
    CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_ORIENTATION, static_cast<std::uint64_t>(-1)) == 0);
    const auto seen = take_requests();
    CHECK(seen.size() == 5);
    if (seen.size() != 5) return;
    CHECK(seen[0].type == APKRT_REQUEST_FINISH && seen[0].argument == 0 && !seen[0].had_text);
    CHECK(seen[1].type == APKRT_REQUEST_START_FAILED && seen[1].had_text && seen[1].text == reason);
    CHECK(seen[2].type == APKRT_REQUEST_BACK_AT_ROOT && seen[2].argument == 0 && !seen[2].had_text);
    CHECK(seen[3].type == APKRT_REQUEST_ORIENTATION && seen[3].argument == 6 && !seen[3].had_text);
    CHECK(seen[4].type == APKRT_REQUEST_ORIENTATION && static_cast<int>(seen[4].argument) == -1);
    for (const auto &request : seen) CHECK(request.thread == ::pthread_self() && request.runtime_signal_blocked);
    CHECK(!runtime_signal_blocked());  // as before the request
}

// START_FAILED and PICK read a guest-written string; a bad or too-short mapping must give -EFAULT, not
// crash the host (the text is read through a checked copy). A pointer into a gone page faults on the
// first byte; one that starts readable but has no NUL before the mapping ends faults as the read runs
// off the end.
TEST(a_start_failed_or_pick_text_in_bad_guest_memory_is_efault_not_a_crash) {
    take_requests();
    const auto page = static_cast<std::size_t>(::getpagesize());
    void *const pages = ::mmap(nullptr, 2 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(pages != MAP_FAILED);
    const auto first = reinterpret_cast<std::uint64_t>(pages);
    std::memset(pages, 'a', page);                                      // no NUL anywhere in the readable page
    CHECK(::munmap(reinterpret_cast<void *>(first + page), page) == 0);  // the second page is gone
    for (const std::uint64_t text : {first + page, first + page - 8}) {  // in the gone page; readable but runs off
        CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_START_FAILED, text) == -efault);
        CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_PICK, text, 1) == -efault);
    }
    ::munmap(pages, page);
    CHECK(take_requests().empty());                                     // nothing reached the host
}

// START_FAILED truncates an over-long text (no NUL within the limit) to the buffer and still reports
// it, returning 0 — it does not refuse like PICK. Pins the helper's "no NUL within the limit" branch
// for this caller (text_limit is 1024, so the kept text is 1023 bytes).
TEST(a_start_failed_text_longer_than_the_limit_is_truncated_not_refused) {
    take_requests();
    const std::string reason(2000, 'a');  // longer than text_limit, no NUL until the string's own
    CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_START_FAILED, address(reason.c_str())) == 0);
    const auto seen = take_requests();
    CHECK(seen.size() == 1);
    if (seen.size() == 1) {
        CHECK(seen[0].type == APKRT_REQUEST_START_FAILED && seen[0].had_text);
        CHECK(seen[0].text == std::string(1023, 'a'));  // truncated to text_limit - 1
    }
}

TEST(a_pick_request_reaches_the_handler_with_its_id_and_text) {
    take_requests();
    static const char pick[] = "5\t1\t20\timage/*\nvideo/*\t";
    CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_PICK, address(pick), 77) == 0);
    const auto seen = take_requests();
    CHECK(seen.size() == 1);
    if (seen.size() != 1) return;
    CHECK(seen[0].type == APKRT_REQUEST_PICK && seen[0].argument == 77 && seen[0].had_text && seen[0].text == pick);
    CHECK(seen[0].thread == ::pthread_self() && seen[0].runtime_signal_blocked);
    // The host answers with the event, its three numbers as posted; that frees the ID.
    const auto serial = apkrt_session_post(APKRT_EVENT_PICK_RESULT, 77, 0, 0);
    CHECK(serial != 0);
    CHECK(call(APKRT_NR_SESSION_ANSWER, serial) == 0);
    CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_PICK, address(pick), 77) == 0);  // the ID is free again
    CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_PICK_CANCEL, 77) == 0);
    take_requests();
}

TEST(a_permission_request_reaches_the_handler_with_its_id_and_text) {
    take_requests();
    static const char ask[] = "1\t1";  // location, fine wanted
    CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_PERMISSION, address(ask), 42) == 0);
    const auto seen = take_requests();
    CHECK(seen.size() == 1);
    if (seen.size() != 1) return;
    CHECK(seen[0].type == APKRT_REQUEST_PERMISSION && seen[0].argument == 42 && seen[0].had_text && seen[0].text == ask);
    CHECK(seen[0].thread == ::pthread_self() && seen[0].runtime_signal_blocked);
    CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_PERMISSION_CANCEL, 42) == 0);  // free the waiting request
    take_requests();
}

// The runtime checks the "<group>\t<detail>" text whole before the host sees it (as for a pick), so a malformed
// group, detail or shape is refused and never reaches the handler.
TEST(a_malformed_permission_request_is_refused_before_the_host_sees_it) {
    take_requests();
    const auto ask = [](const char *text, std::uint64_t id = 9) {
        const long result = call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_PERMISSION, address(text), id);
        if (result == 0) CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_PERMISSION_CANCEL, id) == 0);
        return result;
    };
    CHECK(ask("1\t1") == 0);          // location, fine: accepted
    CHECK(ask("1\t2") == 0);          // location, coarse: accepted
    CHECK(ask("2\t0") == 0);          // microphone: accepted
    CHECK(ask("3\t0") == 0);          // camera: accepted
    CHECK(ask("4\t0") == 0);          // notifications: accepted (ADR-0074)
    take_requests();                  // the accepted requests and their cancels reached the host; drop them
    CHECK(ask("1\t0") == -einval);    // location needs a precision
    CHECK(ask("1\t3") == -einval);    // no such detail
    CHECK(ask("2\t1") == -einval);    // microphone takes no precision
    CHECK(ask("3\t1") == -einval);    // camera takes no precision
    CHECK(ask("4\t1") == -einval);    // notifications take no precision
    CHECK(ask("5\t0") == -einval);    // no such group
    CHECK(ask("1") == -einval);       // no detail field
    CHECK(ask("1\t1\t1") == -einval); // a third field
    CHECK(ask("11\t1") == -einval);   // a two-digit group
    CHECK(ask("1 1") == -einval);     // not tab-separated
    CHECK(ask("") == -einval);        // empty
    CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_PERMISSION, address("1\t1"), 0) == -einval);  // ID 0
    CHECK(take_requests().empty());   // nothing malformed reached the host
}

TEST(permission_requests_are_unique_and_limited) {
    take_requests();
    static const char ask[] = "1\t1";
    for (std::uint64_t id = 1; id <= 4; ++id) {
        CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_PERMISSION, address(ask), id) == 0);
    }
    CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_PERMISSION, address(ask), 2) == -eexist);  // an ID that waits
    CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_PERMISSION, address(ask), 5) == -ebusy);   // the fifth waiting
    for (std::uint64_t id = 1; id <= 4; ++id) {
        CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_PERMISSION_CANCEL, id) == 0);
    }
    CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_PERMISSION, address(ask), 5) == 0);  // the table is free again
    CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_PERMISSION_CANCEL, 5) == 0);
    take_requests();
}

TEST(a_permission_cancel_reaches_the_host_only_while_it_waits) {
    take_requests();
    static const char ask[] = "2\t0";
    CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_PERMISSION, address(ask), 8) == 0);
    take_requests();
    CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_PERMISSION_CANCEL, 8) == 0);
    const auto seen = take_requests();
    CHECK(seen.size() == 1 && seen[0].type == APKRT_REQUEST_PERMISSION_CANCEL && seen[0].argument == 8 &&
          !seen[0].had_text);
    CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_PERMISSION_CANCEL, 8) == -enoent);  // no longer waiting
    CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_PERMISSION_CANCEL, 0) == -einval);  // ID 0
    CHECK(take_requests().empty());
}

TEST(an_ime_request_reaches_the_handler_with_its_serial_and_text) {
    take_requests();
    static const char start[] = "start\t1\t268435458\t0\t0\t0\t-1\t0\t0\thint\twith a tab";
    CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_IME, address(start), 7) == 0);
    CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_IME, address("show"), 7) == 0);
    CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_IME, address("hide"), 7) == 0);
    CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_IME, address("hide"), 0) == 0);  // no connection
    CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_IME, address("stop"), 7) == 0);
    const auto seen = take_requests();
    CHECK(seen.size() == 5);
    if (seen.size() != 5) return;
    CHECK(seen[0].type == APKRT_REQUEST_IME && seen[0].argument == 7 && seen[0].had_text && seen[0].text == start);
    CHECK(seen[1].text == "show" && seen[2].text == "hide" && seen[2].argument == 7);
    CHECK(seen[3].text == "hide" && seen[3].argument == 0 && seen[4].text == "stop" && seen[4].argument == 7);
    CHECK(seen[0].thread == ::pthread_self() && seen[0].runtime_signal_blocked);
}

// The host parses the text, so the runtime checks it whole and a serial where one is needed.
TEST(a_malformed_ime_request_is_refused_before_the_host_sees_it) {
    take_requests();
    const auto ask = [](const std::string &text, std::uint64_t serial = 3) {
        return call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_IME, address(text.c_str()), serial);
    };
    CHECK(ask("start\t1\t2\t3\t4\t5\t6\t7\t1\t") == 0);  // an empty hint
    CHECK(ask("start\t-1\t-2\t0\t0\t0\t0\t-3\t0\thint") == 0);
    CHECK(ask("start\t1\t2\t3\t4\t5\t6\t7\t1\t" + std::string(480, 'x')) == 0);
    take_requests();
    CHECK(ask("") == -einval);
    CHECK(ask("shown") == -einval);
    CHECK(ask("show\t") == -einval);
    CHECK(ask("start") == -einval);
    CHECK(ask("start\t1\t2\t3\t4\t5\t6\t7\t1") == -einval);       // no tab before a hint
    CHECK(ask("start\t1\t2\t3\t4\t5\t6\t7\thint") == -einval);     // seven numbers
    CHECK(ask("start\t1\t2\t3\t4\t5\t6\thint") == -einval);         // six, the form before actionId
    CHECK(ask("start\ta\t2\t3\t4\t5\t6\t7\t1\t") == -einval);     // not a number
    CHECK(ask("start\t1\t\t3\t4\t5\t6\t7\t1\t") == -einval);      // an empty number
    CHECK(ask("start\t-\t2\t3\t4\t5\t6\t7\t1\t") == -einval);     // a sign alone
    CHECK(ask("start\t12345678901\t2\t3\t4\t5\t6\t7\t1\t") == -einval);  // eleven digits
    CHECK(ask("start\t1\t2\t3\t4\t5\t6\t7\t1\t" + std::string(520, 'x')) == -einval);  // too long
    CHECK(ask("start\t1\t2\t3\t4\t5\t6\t7\t1\t", 0) == -einval);  // a start needs a serial
    CHECK(ask("show", 0) == -einval);
    CHECK(ask("stop", 0) == -einval);
    CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_IME, 0, 3) == -efault);
    CHECK(take_requests().empty());
}

// ADR-0068 keyboard UX: the served field's place in the window and the text it holds, for the host's text bar.
TEST(an_ime_field_and_extract_request_reach_the_handler_with_their_text) {
    take_requests();
    static const char field[] = "field\t7\t120\t-30\t1170\t900";
    // A hint and a text with tabs and new lines, which only the last field may hold.
    static const char extract[] = "extract\t7\t0\t3\t3\t0\t3\t5\thint\ta\tb\nc \xE2\x80\xA2";
    CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_IME, address(field), 7) == 0);
    CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_IME, address(extract), 7) == 0);
    CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_IME, address("extract\t7\t0\t0\t0\t-1\t-1\t0\t\t"), 7) == 0);
    // The cap of 4 KB around the selection is 4096 characters of up to four bytes.
    const std::string big = "extract\t7\t0\t0\t0\t-1\t-1\t1\t\t" + std::string(4 * 4096, 'x');
    CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_IME, address(big.c_str()), 7) == 0);
    const auto seen = take_requests();
    CHECK(seen.size() == 4);
    if (seen.size() != 4) return;
    CHECK(seen[0].type == APKRT_REQUEST_IME && seen[0].argument == 7 && seen[0].text == field);
    CHECK(seen[1].argument == 7 && seen[1].text == extract);
    CHECK(seen[3].text == big);
}

TEST(a_malformed_ime_field_or_extract_request_is_refused_before_the_host_sees_it) {
    take_requests();
    const auto ask = [](const std::string &text, std::uint64_t serial = 7) {
        return call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_IME, address(text.c_str()), serial);
    };
    CHECK(ask("field\t7\t1\t2\t3\t4") == 0);
    CHECK(ask("extract\t7\t0\t1\t2\t3\t4\t5\t\t") == 0);  // an empty hint and text
    take_requests();
    CHECK(ask("field") == -einval);
    CHECK(ask("field\t7\t1\t2\t3") == -einval);            // four numbers after the tag
    CHECK(ask("field\t7\t1\t2\t3\t4\t5") == -einval);     // six
    CHECK(ask("field\t7\t1\t2\t3\ta") == -einval);        // not a number
    CHECK(ask("field\t8\t1\t2\t3\t4") == -einval);        // another serial than the request's
    CHECK(ask("field\t7\t1\t2\t3\t4", 0) == -einval);     // no serial
    CHECK(ask("extract\t7\t0\t1\t2\t3\t4\t5\thint") == -einval);  // no tab before the text
    CHECK(ask("extract\t7\t1\t2\t3\t4\t5\thint\ttext") == -einval);  // five numbers after the serial
    CHECK(ask("extract\t7\t0\t1\t2\t3\t4\tx\thint\ttext") == -einval);  // not a number
    CHECK(ask("extract\t9\t0\t1\t2\t3\t4\t5\t\ttext") == -einval);  // another serial
    CHECK(ask("extract\t7\t0\t1\t2\t3\t4\t5\t\ttext", 0) == -einval);
    CHECK(ask("extract\t7\t0\t1\t2\t3\t4\t5\t\t" + std::string(4 * 4096 + 2048, 'x')) == -einval);  // too long
    CHECK(ask("start\t1\t2\t3\t4\t5\t6\t7\t1\t" + std::string(520, 'x')) == -einval);  // a start keeps its limit
    CHECK(take_requests().empty());
}

// APKRT_REQUEST_NOTIFICATION (ADR-0074 decision 2): the app's post, cancel or cancel of all, with the text the host
// puts on screen, so the runtime refuses every text outside the limits, and every request without the app's grant.
TEST(a_notification_request_needs_the_grant_and_reaches_the_handler_with_its_text) {
    take_requests();
    apkrt::permissions::reset();
    static const char post[] = "post\t7\tchat\tmessages\t4\t1\t1800000000000\tfriends\tAnna\tGroup\t\tHi!\nSee you";
    CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_NOTIFICATION, address(post), 0) == -eacces);  // not answered
    apkrt::permissions::set(APKRT_PERMISSION_NOTIFICATIONS, APKRT_PERMISSION_DENIED);
    CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_NOTIFICATION, address(post), 0) == -eacces);  // denied
    CHECK(take_requests().empty());
    apkrt::permissions::set(APKRT_PERMISSION_NOTIFICATIONS, APKRT_PERMISSION_GRANTED);
    CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_NOTIFICATION, address(post), 0) == 0);
    CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_NOTIFICATION, address("cancel\t7\tchat"), 0) == 0);
    CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_NOTIFICATION, address("cancelAll"), 0) == 0);
    const auto seen = take_requests();
    CHECK(seen.size() == 3);
    if (seen.size() == 3) {
        CHECK(seen[0].type == APKRT_REQUEST_NOTIFICATION && seen[0].argument == 0 && seen[0].text == post);
        CHECK(seen[1].text == "cancel\t7\tchat" && seen[2].text == "cancelAll");
    }
    apkrt::permissions::reset();
}

TEST(a_malformed_or_oversized_notification_request_is_refused_before_the_host_sees_it) {
    take_requests();
    apkrt::permissions::reset();
    apkrt::permissions::set(APKRT_PERMISSION_NOTIFICATIONS, APKRT_PERMISSION_GRANTED);
    const auto ask = [](const std::string &text, std::uint64_t second = 0) {
        return call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_NOTIFICATION, address(text.c_str()), second);
    };
    const auto refused = [&](const std::string &text) { return ask(text) == -einval; };
    const auto accepted = [&](const std::string &text) { return ask(text) == 0; };
    const auto post = [](const std::string &id, const std::string &tag, const std::string &importance,
                         const std::string &sound, const std::string &when, const std::string &title,
                         const std::string &text) {
        return "post\t" + id + "\t" + tag + "\tch\t" + importance + "\t" + sound + "\t" + when + "\tg\t" + title +
               "\tsub\t\t" + text;
    };
    const auto with_intent = [](const std::string &intent) {
        return "post\t1\ttag\tch\t3\t0\t0\tg\tT\tsub\t" + intent + "\tx";
    };
    CHECK(accepted(with_intent("intent:#Intent;component=a.b/.C;S.room=7;end")));  // a content intent
    CHECK(accepted(with_intent(std::string(2048, 'i'))));                          // its longest
    CHECK(refused(with_intent(std::string(2049, 'i'))));
    CHECK(refused(with_intent("intent:#Intent;S.x=a b;end")));                      // a space
    CHECK(refused(with_intent("intent:#Intent;S.x=\xC3\xA4;end")));                // not ASCII
    CHECK(refused(with_intent("intent:#Intent;S.x=\x7f;end")));
    CHECK(accepted(post("7", "", "3", "0", "0", "", "")));                           // every text field empty
    CHECK(accepted(post("-2147483648", "t", "0", "1", "9223372036854775807", "T", "x")));  // the ends of the numbers
    CHECK(accepted(post("2147483647", std::string(64, 't'), "5", "0", "1", std::string(256, 'T'),
                        std::string(4096, 'x'))));                                    // the longest fields
    CHECK(accepted(post("1", "\xC3\xA4", "3", "0", "0", "\xE2\x82\xAC", "a\nb\n\xF0\x9F\x98\x80")));  // UTF-8
    CHECK(refused(post("2147483648", "", "3", "0", "0", "", "")));                   // an id over int32
    CHECK(refused(post("-2147483649", "", "3", "0", "0", "", "")));
    CHECK(refused(post("x", "", "3", "0", "0", "", "")) && refused(post("", "", "3", "0", "0", "", "")));
    CHECK(refused(post("+1", "", "3", "0", "0", "", "")) && refused(post("01", "", "3", "0", "0", "", "")));
    CHECK(refused(post("1", "", "6", "0", "0", "", "")) && refused(post("1", "", "33", "0", "0", "", "")));
    CHECK(refused(post("1", "", "3", "2", "0", "", "")) && refused(post("1", "", "3", "0", "-1", "", "")));
    CHECK(refused(post("1", "", "3", "0", "9223372036854775808", "", "")));         // when over int64
    CHECK(refused(post("1", std::string(65, 't'), "3", "0", "0", "", "")));          // a tag over 64 bytes
    CHECK(refused(post("1", "", "3", "0", "0", std::string(257, 'T'), "")));         // a title over 256 bytes
    CHECK(refused(post("1", "", "3", "0", "0", "", std::string(4097, 'x'))));        // a text over 4096 bytes
    CHECK(refused(post("1", "a\nb", "3", "0", "0", "", "")));                        // a new line outside the text
    CHECK(refused(post("1", "", "3", "0", "0", "a\nb", "")));
    CHECK(refused(post("1", "", "3", "0", "0", "", "a\rb")) && refused(post("1", "", "3", "0", "0", "", "a\x07")));
    CHECK(refused(post("1", "", "3", "0", "0", "\xE2\x80\xAE", "")));                // U+202E, a bidi override
    CHECK(refused(post("1", "", "3", "0", "0", "", "\xE2\x80\x8B")));                // U+200B
    CHECK(refused(post("1", "", "3", "0", "0", "\xD8\x9C", "")));                    // U+061C, Arabic letter mark
    CHECK(refused(post("1", "", "3", "0", "0", "", "\xCD\x8F")));                    // U+034F, combining grapheme joiner
    CHECK(refused(post("1", "", "3", "0", "0", "", "\xF3\xA0\x80\x80")));            // U+E0000, a stray tag character
    CHECK(refused(post("1", "", "3", "0", "0", "", "\xF3\xA0\x80\x9F")));            // U+E001F, a stray tag character
    // A subdivision flag (U+1F3F4 then flag tags U+E0020-E007F) stays whole; here U+1F3F4, a tag and the cancel tag.
    CHECK(accepted(post("1", "", "3", "0", "0", "", "\xF0\x9F\x8F\xB4\xF3\xA0\x81\xA7\xF3\xA0\x81\xBF")));
    CHECK(refused(post("1", "", "3", "0", "0", "", "\xC0\xAF")));                     // overlong UTF-8
    CHECK(refused(post("1", "\xE2\x82", "3", "0", "0", "", "")));                     // cut at a field's end
    CHECK(refused(post("1", "", "3", "0", "0", "", "x\xE2")));                         // cut at the text's end
    CHECK(refused(post("1", "", "3", "0", "0", "", "x") + "\textra"));                // the text has no tab
    CHECK(refused("post\t1\t\tch\t3\t0\t0\tg\tT\tsub"));                          // a field missing
    CHECK(accepted("cancel\t7\t") && accepted("cancel\t-1\tchat"));
    CHECK(refused("cancel\t7") && refused("cancel\tx\tchat") && refused("cancel\t7\tchat\t"));
    CHECK(refused("cancelAll\t") && refused("cancelall") && refused("show") && refused(""));
    CHECK(ask("cancelAll", 1) == -einval);                                           // the second argument is 0
    CHECK(refused("post\t1\t\tch\t3\t0\t0\tg\tT\tsub\t" + std::string(8200, 'x')));  // past the copy's limit
    take_requests();
    apkrt::permissions::reset();
}

// An app installer (APKRT_REQUEST_ASK_INSTALL_SOURCES, APKRT_REQUEST_CONFIRM_INSTALL): "<package>\n<label>" and
// "<path>\n<package>" reach the host with their IDs, as the runtime checked them.
TEST(install_requests_reach_the_handler_with_their_id_and_text) {
    take_requests();
    static const char ask[] = "io.example.store\nExample Store";
    static const char confirm[] = "/data/user/0/io.example.store/cache/install/1.apk\nio.example.store";
    CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_ASK_INSTALL_SOURCES, address(ask), 31) == 0);
    CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_CONFIRM_INSTALL, address(confirm), 32) == 0);
    const auto seen = take_requests();
    CHECK(seen.size() == 2);
    if (seen.size() != 2) return;
    CHECK(seen[0].type == APKRT_REQUEST_ASK_INSTALL_SOURCES && seen[0].argument == 31 && seen[0].text == ask);
    CHECK(seen[1].type == APKRT_REQUEST_CONFIRM_INSTALL && seen[1].argument == 32 && seen[1].text == confirm);
    CHECK(seen[1].thread == ::pthread_self() && seen[1].runtime_signal_blocked);
}

// The host splits the text at its newline, so the runtime refuses any other shape, an ID of 0 and a text past the
// limit (a Linux path of PATH_MAX and a package name), and none of them reaches the handler.
TEST(a_malformed_install_request_is_refused_before_the_host_sees_it) {
    take_requests();
    const auto both = [](const std::string &text, std::uint64_t id = 9) {
        const long ask = call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_ASK_INSTALL_SOURCES, address(text.c_str()), id);
        const long confirm = call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_CONFIRM_INSTALL, address(text.c_str()), id);
        return ask == confirm ? ask : -9999;
    };
    CHECK(both("a\nb") == 0);
    CHECK(both(std::string(4095, 'p') + "\n" + std::string(255, 'q')) == 0);  // the longest path and package
    take_requests();
    CHECK(both("ab") == -einval);                 // no newline
    CHECK(both("a\nb\nc") == -einval);            // two
    CHECK(both("\nb") == -einval);                // an empty first part
    CHECK(both("a\n") == -einval);                // an empty second part
    CHECK(both("") == -einval);                   // empty
    CHECK(both("a\nb", 0) == -einval);            // ID 0
    CHECK(both(std::string(4096, 'p') + "\n" + std::string(256, 'q')) == -einval);  // past the limit
    CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_CONFIRM_INSTALL, 0, 9) == -efault);  // no text
    CHECK(take_requests().empty());
}

// The host's answer to the question records the grant, as apkrt_set_install_sources does before the start, so a
// later start of the same process and the runtime's own reads agree with the framework.
TEST(an_install_sources_answer_records_the_grant) {
    apkrt::install_sources::reset();
    CHECK(apkrt_session_post(APKRT_EVENT_INSTALL_SOURCES_RESULT, 31, 1, 0) != 0);
    CHECK(apkrt::install_sources::granted());
    CHECK(apkrt_session_post(APKRT_EVENT_INSTALL_SOURCES_RESULT, 31, 0, 0) != 0);
    CHECK(!apkrt::install_sources::granted());
    apkrt::install_sources::reset();
}

TEST(a_permission_answer_records_the_grant_and_frees_the_id) {
    take_requests();
    apkrt::permissions::reset();
    static const char ask[] = "1\t1";
    CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_PERMISSION, address(ask), 55) == 0);
    take_requests();
    // The host answers through apkrt_permission_answer: it records the grant (so the location syscalls can gate on
    // it) and frees the waiting ID. The posted APKRT_EVENT_PERMISSION_RESULT rides apkrt_session_post, tested above.
    apkrt_permission_answer(55, APKRT_PERMISSION_LOCATION, APKRT_PERMISSION_GRANTED_COARSE);
    CHECK(apkrt::permissions::outcome(APKRT_PERMISSION_LOCATION) == APKRT_PERMISSION_GRANTED_COARSE);
    CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_PERMISSION_CANCEL, 55) == -enoent);  // the answer freed the ID
    // An unsolicited update (foreground re-read): ID 0, no waiting request, records the new outcome.
    apkrt_permission_answer(0, APKRT_PERMISSION_LOCATION, APKRT_PERMISSION_DENIED);
    CHECK(apkrt::permissions::outcome(APKRT_PERMISSION_LOCATION) == APKRT_PERMISSION_DENIED);
    take_requests();
}

// The host parses what the guest wrote, so the runtime refuses every request outside the limits, and none of them
// reaches the handler.
TEST(a_malformed_or_oversized_pick_request_is_refused_before_the_host_sees_it) {
    take_requests();
    const auto ask = [](const std::string &text, std::uint64_t id = 5) {
        const long result = call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_PICK, address(text.c_str()), id);
        if (result == 0) CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_PICK_CANCEL, id) == 0);
        return result;
    };
    const auto refused = [&](const std::string &text, std::uint64_t id = 5) { return ask(text, id) == -einval; };
    const auto accepted = [&](const std::string &text) { return ask(text, 9) == 0; };
    const std::string types8 = "a/a\nb/b\nc/c\nd/d\ne/e\nf/f\ng/g\nh/h";
    // Good ones first: the limits themselves are allowed.
    CHECK(accepted("1\t0\t0\t\t"));                                   // no types and no title
    CHECK(accepted("3\t0\t0\ttext/plain\tnote.txt"));
    CHECK(accepted("5\t1\t1000\t" + types8 + "\t"));                   // all at their limits
    CHECK(accepted("3\t0\t0\t\t" + std::string(200, 'x')));
    CHECK(accepted("4\t0\t0\t\t"));
    CHECK(accepted("2\t1\t0\t" + std::string(49, 'a') + "/" + std::string(50, 'b') + "\t"));  // 100 bytes
    CHECK(accepted("3\t0\t0\t\ttitle \xc3\xa4\xe2\x82\xac\xf0\x9f\x98\x80"));  // UTF-8 of 1 to 4 bytes
    CHECK(accepted("1\t0\t0\t*/*\t"));
    CHECK(accepted("1\t0\t0\tapplication/vnd.android.package-archive\t"));
    take_requests();
    // The fields.
    CHECK(refused(""));
    CHECK(refused("1"));
    CHECK(refused("1\t0\t0\t"));                  // four fields
    CHECK(refused("1\t0\t0\t\t\t"));              // six
    CHECK(refused("0\t0\t0\t\t"));                // kind 0
    CHECK(refused("6\t0\t0\t\t"));                // kind 6
    CHECK(refused("10\t0\t0\t\t"));               // kind 10
    CHECK(refused("-1\t0\t0\t\t"));
    CHECK(refused("a\t0\t0\t\t"));
    CHECK(refused("1\t2\t0\t\t"));                // flags
    CHECK(refused("1\t\t0\t\t"));
    CHECK(refused("1\t0\t\t\t"));                 // an empty maximum
    CHECK(refused("5\t0\t1001\t\t"));
    CHECK(refused("5\t0\t99999\t\t"));
    CHECK(refused("5\t0\t18446744073709551616\t\t"));
    CHECK(refused("5\t0\t-1\t\t"));
    CHECK(refused("5\t0\t1x\t\t"));
    CHECK(refused("5\t0\t0001\t\t"));             // leading zeros
    CHECK(refused("5\t0\t01\t\t"));
    // A field that does not fit the kind.
    CHECK(refused("1\t0\t5\t\t"));                // a maximum for an open
    CHECK(refused("3\t0\t5\t\t"));
    CHECK(refused("3\t1\t0\t\t"));                // several saves
    CHECK(refused("4\t1\t0\t\t"));                // several folders
    CHECK(refused("4\t0\t0\ttext/plain\t"));      // a type for a folder
    CHECK(refused("1\t0\t0\t\ttitle"));           // a title for an open
    CHECK(refused("5\t0\t1\t\ttitle"));
    CHECK(refused("4\t0\t0\t\ttitle"));
    // The MIME types.
    CHECK(refused("1\t0\t0\t" + types8 + "\ni/i\t"));                        // nine
    CHECK(refused("1\t0\t0\ttext/plain\n\t"));                              // an empty last one
    CHECK(refused("1\t0\t0\t\ntext/plain\t"));
    CHECK(refused("1\t0\t0\ttext\t"));                                       // no slash
    CHECK(refused("1\t0\t0\t/plain\t"));
    CHECK(refused("1\t0\t0\ttext/\t"));
    CHECK(refused("1\t0\t0\ttext/pl/ain\t"));                                // two slashes
    CHECK(refused("1\t0\t0\ttext/pla in\t"));
    CHECK(refused("1\t0\t0\ttext/pla;in\t"));
    CHECK(refused("1\t0\t0\ttext/pl\x01in\t"));
    CHECK(refused("1\t0\t0\ttext/pl\xc3\xa4in\t"));                          // not ASCII
    CHECK(refused("1\t0\t0\t" + std::string(50, 'a') + "/" + std::string(50, 'b') + "\t"));  // 101 bytes
    CHECK(refused("1\t0\t0\t" + std::string(2000, 'a') + "/b\t"));
    // The title.
    CHECK(refused("3\t0\t0\t\t" + std::string(201, 'x')));
    CHECK(refused("3\t0\t0\t\tnew\nline"));
    CHECK(refused("3\t0\t0\t\tcarriage\rreturn"));
    CHECK(refused("3\t0\t0\t\tdel\x7f"));
    CHECK(refused("3\t0\t0\t\tescape\x1b[2J"));
    CHECK(refused("3\t0\t0\t\tc1 \xc2\x85 control"));                // U+0085, which some hosts take for a line break
    CHECK(refused("3\t0\t0\t\tc1 \xc2\x9f control"));
    CHECK(refused("3\t0\t0\t\tline \xe2\x80\xa8 separator"));        // U+2028
    CHECK(refused("3\t0\t0\t\tparagraph \xe2\x80\xa9 separator"));   // U+2029
    CHECK(refused("3\t0\t0\t\tCancel\xe2\x80\xae" "noitcA"));         // U+202E, a bidirectional override
    CHECK(refused("3\t0\t0\t\tisolate \xe2\x81\xa6 x"));             // U+2066
    CHECK(refused("3\t0\t0\t\tzero \xe2\x80\x8b width"));            // U+200B
    CHECK(refused("3\t0\t0\t\tmark \xef\xbb\xbf bom"));              // U+FEFF
    CHECK(refused("3\t0\t0\t\tannotation \xef\xbf\xb9 x"));          // U+FFF9
    CHECK(refused("3\t0\t0\t\tlone \x80 continuation"));
    CHECK(refused("3\t0\t0\t\ttruncated \xe2\x82"));
    CHECK(refused("3\t0\t0\t\ttruncated \xf0"));
    CHECK(refused("3\t0\t0\t\toverlong \xc0\xaf"));
    CHECK(refused("3\t0\t0\t\toverlong \xe0\x80\xaf"));
    CHECK(refused("3\t0\t0\t\tsurrogate \xed\xa0\x80"));
    // An emoji as two surrogates, as JNI's modified UTF-8 gives it.
    CHECK(refused("3\t0\t0\t\tmodified UTF-8 \xed\xa0\xbd\xed\xb8\x80"));
    CHECK(refused("3\t0\t0\t\ttoo high \xf4\x90\x80\x80"));
    CHECK(refused("3\t0\t0\t\tinvalid \xff"));
    CHECK(accepted("3\t0\t0\t\tright to left \xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d, accents \xc3\xa9, \xe2\x81\x84"));
    take_requests();  // the accepted ones reached the handler, and their cancels
    // The whole text: a NUL-less run of the limit's length or more is refused, not cut.
    CHECK(refused(std::string(1535, '1')));
    CHECK(refused("3\t0\t0\t\t" + std::string(2000, 'x')));
    {
        std::vector<char> unterminated(4096, 'x');  // no NUL within the limit
        CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_PICK, address(unterminated.data()), 5) == -einval);
    }
    // The ID and the address.
    CHECK(refused("1\t0\t0\t\t", 0));
    CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_PICK, 0, 5) == -efault);
    CHECK(take_requests().empty());
}

TEST(pick_requests_wait_for_their_answer_four_at_most_each_with_its_own_id) {
    take_requests();
    static const char open[] = "1\t0\t0\t\t";
    const auto pick = [](std::uint64_t id) {
        return call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_PICK, address(open), id);
    };
    CHECK(pick(21) == 0 && pick(22) == 0 && pick(23) == 0 && pick(24) == 0);
    CHECK(pick(25) == -ebusy);   // four wait
    CHECK(pick(22) == -eexist);  // an ID that waits
    CHECK(take_requests().size() == 4);
    // An answer frees its ID, a cancel the same and tells the host, and neither is possible twice.
    CHECK(apkrt_session_post(APKRT_EVENT_PICK_RESULT, 21, 0, 0) != 0);
    CHECK(pick(25) == 0);
    CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_PICK_CANCEL, 22) == 0);
    const auto seen = take_requests();
    CHECK(seen.size() == 2);
    if (seen.size() == 2) {
        CHECK(seen[0].type == APKRT_REQUEST_PICK && seen[0].argument == 25);
        CHECK(seen[1].type == APKRT_REQUEST_PICK_CANCEL && seen[1].argument == 22 && !seen[1].had_text);
    }
    CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_PICK_CANCEL, 22) == -enoent);
    CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_PICK_CANCEL, 21) == -enoent);  // answered
    CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_PICK_CANCEL, 0) == -einval);
    CHECK(take_requests().empty());
    for (const std::uint64_t id : std::initializer_list<std::uint64_t>{23, 24, 25}) {
        CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_PICK_CANCEL, id) == 0);
    }
    take_requests();
}

TEST(pick_requests_from_many_threads_reach_the_handler_one_at_a_time) {
    take_requests();
    pick_in_handler.store(0);
    pick_overlap.store(false);
    slow_pick.store(true);
    static const char open[] = "1\t0\t0\t\t";
    std::atomic<int> taken{0};
    std::vector<std::thread> guests;
    for (std::uint64_t id = 31; id < 35; ++id) {
        guests.emplace_back([&, id] {
            if (call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_PICK, address(open), id) == 0) taken.fetch_add(1);
        });
    }
    for (auto &guest : guests) guest.join();
    slow_pick.store(false);
    CHECK(taken.load() == 4);
    CHECK(!pick_overlap.load());
    CHECK(take_requests().size() == 4);
    for (std::uint64_t id = 31; id < 35; ++id) {
        CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_PICK_CANCEL, id) == 0);
    }
    take_requests();
}

TEST(a_pick_handler_may_wait_for_another_thread_to_post_the_answer) {
    take_requests();
    pick_posted.store(false);
    pick_saw_post.store(false);
    pick_waits_for_post.store(true);
    static const char open[] = "1\t0\t0\t\t";
    std::thread poster([] {
        std::this_thread::sleep_for(50ms);
        apkrt_session_post(APKRT_EVENT_PICK_RESULT, 41, 0, 0);
        pick_posted.store(true);
    });
    CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_PICK, address(open), 41) == 0);
    poster.join();
    pick_waits_for_post.store(false);
    CHECK(pick_saw_post.load());
    CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_PICK_CANCEL, 41) == -enoent);  // answered
    take_requests();
}

/// A field of an open request as the guest encodes it (ADR-0062): every byte but RFC 3986's unreserved ones as %XX.
std::string open_field(std::string_view field) {
    static const char hex[] = "0123456789ABCDEF";
    std::string encoded;
    for (const char c : field) {
        const auto byte = static_cast<unsigned char>(c);
        if ((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') ||
            byte == '-' || byte == '.' || byte == '_' || byte == '~') {
            encoded += c;
        } else {
            encoded += '%';
            encoded += hex[byte >> 4];
            encoded += hex[byte & 15];
        }
    }
    return encoded;
}

/// An open request's text: the kind, then the fields encoded.
std::string open_text(int kind, std::initializer_list<std::string_view> fields) {
    std::string text = std::to_string(kind);
    for (const auto field : fields) text += "\t" + open_field(field);
    return text;
}

// APKRT_REQUEST_EXPORT_MEDIA (ADR-0057 step e): a finished image or video, by its path below the app's data folder.
// Like a pick, the text is the guest's and the host opens the path, so the runtime refuses every text outside the
// limits.
TEST(an_export_request_reaches_the_handler_with_its_text_and_no_id) {
    take_requests();
    static const char text[] = "1\t.minato/library/files/Pictures/Minato/shot.jpg";
    CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_EXPORT_MEDIA, address(text), 0) == 0);
    CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_EXPORT_MEDIA, address("2\tclip.mp4"), 0) == 0);
    const auto seen = take_requests();
    CHECK(seen.size() == 2);
    if (seen.size() != 2) return;
    CHECK(seen[0].type == APKRT_REQUEST_EXPORT_MEDIA && seen[0].argument == 0 && seen[0].had_text);
    CHECK(seen[0].text == text && seen[1].text == "2\tclip.mp4");
    CHECK(seen[0].thread == ::pthread_self() && seen[0].runtime_signal_blocked);
}

TEST(a_malformed_or_oversized_export_request_is_refused_before_the_host_sees_it) {
    take_requests();
    const auto ask = [](const std::string &text, std::uint64_t second = 0) {
        return call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_EXPORT_MEDIA, address(text.c_str()), second);
    };
    const auto refused = [&](const std::string &text) { return ask(text) == -einval; };
    const auto accepted = [&](const std::string &text) { return ask(text) == 0; };
    std::string sixteen = "a";
    for (int i = 1; i < 16; ++i) sixteen += "/a";
    std::string longest = std::string(255, 'x');
    for (int i = 0; i < 3; ++i) longest += "/" + std::string(255, 'y');
    longest = longest.substr(0, 1024);
    CHECK(accepted("1\tshot.jpg") && accepted("2\tclip.mp4") && accepted("1\t" + sixteen));
    CHECK(accepted("1\t" + std::string(255, 'n')));                                  // the longest name
    CHECK(accepted("1\t" + longest.substr(0, 255) + "/" + longest.substr(256, 255) + "/" + longest.substr(512, 255) +
                   "/" + longest.substr(768, 200)));                                  // under 1024 bytes
    CHECK(accepted("1\t\xC3\xA4\xE2\x82\xAC.jpg"));                                  // plain UTF-8
    CHECK(refused("") && refused("1") && refused("1\t") && refused("\tshot.jpg"));    // fields missing
    CHECK(refused("0\tshot.jpg") && refused("3\tshot.jpg") && refused("01\tshot.jpg") && refused("x\tshot.jpg"));
    CHECK(refused("1 shot.jpg") && refused("1\tshot.jpg\t") && refused("1\ta\tb"));
    CHECK(refused("1\t/shot.jpg") && refused("1\ta//b") && refused("1\ta/") && refused("1\t./a") &&
          refused("1\ta/.") && refused("1\t../a") && refused("1\ta/../b") && refused("1\t.."));
    CHECK(refused("1\t" + sixteen + "/a"));                                           // 17 components
    CHECK(refused("1\t" + std::string(256, 'n')));                                    // a name over 255 bytes
    CHECK(refused("1\t" + std::string(1025, 'a')));                                   // over 1024 bytes
    CHECK(refused("1\t" + std::string(1500, 'a')));
    CHECK(refused("1\t" + std::string(255, 'a') + "/" + std::string(255, 'b') + "/" + std::string(255, 'c') + "/" +
                  std::string(255, 'd') + "/e"));                                     // names fine, 1025 bytes in all
    CHECK(refused("1\ta\nb") && refused("1\ta\rb"));                    // line breaks in a name
    CHECK(refused("1\ta\x01" "b") && refused("1\ta\x7F" "b") && refused("1\ta\xC2\x85" "b"));      // controls
    CHECK(refused("1\ta\xE2\x80\x8D" "b") && refused("1\ta\xE2\x80\xAE" "b") && refused("1\ta\xEF\xBB\xBF" "b"));
    CHECK(refused("1\ta\xFF" "b") && refused("1\ta\xC0\x80" "b") && refused("1\ta\xED\xA0\x80" "b"));  // not UTF-8
    // The limit of the path itself: 1024 bytes are allowed, 1025 are not (five names, four slashes).
    const std::string names4 = std::string(255, 'a') + "/" + std::string(255, 'b') + "/" + std::string(255, 'c') +
                               "/" + std::string(254, 'd') + "/";
    CHECK(accepted("1\t" + names4 + "e") && refused("1\t" + names4 + "ee"));
    CHECK(refused("1\ta\xE0\x80\x80") && refused("1\ta\xF0\x80\x80\x80") && refused("1\ta\xF4\x90\x80\x80") &&
          refused("1\ta\xC1\xBF") && refused("1\ta\xE2\x82") && refused("1\ta\xC3"));      // overlong, too high, cut
    CHECK(refused("1\ta\xE2\x81\xA6" "b"));                                       // U+2066, a bidirectional isolate
    CHECK(refused("1\t" + std::string(1535, 'z')));                           // 1537 bytes: no NUL within the limit
    CHECK(ask("1\tshot.jpg", 1) == -einval);                                          // a1 is 0
    CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_EXPORT_MEDIA, 0, 0) == -efault);   // no text
    CHECK(take_requests().size() == 7);                                               // only the accepted ones
}

TEST(an_export_text_ends_at_its_first_nul_and_nothing_after_it_reaches_the_host) {
    take_requests();
    static const char text[] = "1\tshot.jpg\0../../x";
    CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_EXPORT_MEDIA, address(text), 0) == 0);
    const auto seen = take_requests();
    CHECK(seen.size() == 1 && seen[0].text == "1\tshot.jpg");
}

TEST(an_export_text_in_bad_guest_memory_is_efault_not_a_crash) {
    take_requests();
    const auto page = static_cast<std::size_t>(::getpagesize());
    void *const pages = ::mmap(nullptr, 2 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(pages != MAP_FAILED);
    const auto first = reinterpret_cast<std::uint64_t>(pages);
    std::memset(pages, 'a', page);                                      // no NUL anywhere in the readable page
    CHECK(::munmap(reinterpret_cast<void *>(first + page), page) == 0);  // the second page is gone
    for (const std::uint64_t text : {first + page, first + page - 8}) {
        CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_EXPORT_MEDIA, text, 0) == -efault);
    }
    ::munmap(pages, page);
    CHECK(take_requests().empty());
}

TEST(export_requests_and_picks_from_many_threads_reach_the_handler_one_at_a_time) {
    take_requests();
    pick_in_handler.store(0);
    pick_overlap.store(false);
    slow_pick.store(true);
    static const char open[] = "1\t0\t0\t\t";
    std::atomic<int> exported{0}, picked{0};
    std::vector<std::thread> guests;
    for (int t = 0; t < 3; ++t) {
        guests.emplace_back([&, t] {
            for (int i = 0; i < 4; ++i) {
                const std::string text = "1\tfile-" + std::to_string(t) + "-" + std::to_string(i) + ".jpg";
                if (call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_EXPORT_MEDIA, address(text.c_str()), 0) == 0) {
                    exported.fetch_add(1);
                }
            }
        });
    }
    for (std::uint64_t id = 41; id < 43; ++id) {
        guests.emplace_back([&, id] {
            if (call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_PICK, address(open), id) == 0) picked.fetch_add(1);
        });
    }
    for (auto &guest : guests) guest.join();
    slow_pick.store(false);
    CHECK(exported.load() == 12 && picked.load() == 2);                 // no table, no limit of waiting requests
    CHECK(!pick_overlap.load());                                        // no two requests were in the handler at once
    CHECK(take_requests().size() == 14);
    for (std::uint64_t id = 41; id < 43; ++id) {
        CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_PICK_CANCEL, id) == 0);
    }
    take_requests();
}

TEST(an_open_request_reaches_the_handler_with_its_id_and_text) {
    take_requests();
    const std::string view = open_text(1, {"https://example.org/a?b=c"});
    CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_OPEN, address(view.c_str()), 88) == 0);
    const auto seen = take_requests();
    CHECK(seen.size() == 1);
    if (seen.size() != 1) return;
    CHECK(seen[0].type == APKRT_REQUEST_OPEN && seen[0].argument == 88 && seen[0].had_text && seen[0].text == view);
    CHECK(seen[0].thread == ::pthread_self() && seen[0].runtime_signal_blocked);
    // The host answers with the event; that frees the ID.
    const auto serial = apkrt_session_post(APKRT_EVENT_OPEN_RESULT, 88, 0xFFFFFFFFu, 0);
    CHECK(serial != 0);
    CHECK(call(APKRT_NR_SESSION_ANSWER, serial) == 0);
    CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_OPEN, address(view.c_str()), 88) == 0);  // free again
    CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_OPEN_CANCEL, 88) == 0);
    const auto cancelled = take_requests();
    CHECK(cancelled.size() == 2);
    if (cancelled.size() == 2) {
        CHECK(cancelled[1].type == APKRT_REQUEST_OPEN_CANCEL && cancelled[1].argument == 88 && !cancelled[1].had_text);
    }
}

// The host parses what the guest wrote, so the runtime refuses every open request outside the grammar of
// guest_interface.h, and none of them reaches the handler.
TEST(a_malformed_or_oversized_open_request_is_refused_before_the_host_sees_it) {
    take_requests();
    const auto ask = [](const std::string &text, std::uint64_t id = 6) {
        const long result = call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_OPEN, address(text.c_str()), id);
        if (result == 0) CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_OPEN_CANCEL, id) == 0);
        return result;
    };
    const auto refused = [&](const std::string &text, std::uint64_t id = 6) { return ask(text, id) == -einval; };
    const auto accepted = [&](const std::string &text) { return ask(text, 9) == 0; };
    // Good ones first, the limits themselves included.
    CHECK(accepted(open_text(1, {"https://example.org"})));
    CHECK(accepted(open_text(1, {"http://example.org"})));
    CHECK(accepted(open_text(1, {"HTTPS://example.org"})));                      // the scheme's case is ignored
    CHECK(accepted(open_text(1, {"tel:+49301234567"})));
    CHECK(accepted(open_text(1, {"maps://?q=Berlin"})));
    CHECK(accepted(open_text(1, {"https://example.org/" + std::string(4076, 'a')})));  // 4096 bytes
    CHECK(accepted(open_text(1, {"https://example.org/\xc3\xa4\xe2\x82\xac\xf0\x9f\x98\x80"})));
    CHECK(accepted("1\thttps%3a%2f%2fexample.org"));                            // lower-case hex digits
    CHECK(accepted(open_text(2, {"mailto:a@example.org?subject=Hi%20there&body=Line%0D%0A"})));
    CHECK(accepted(open_text(2, {"MAILTO:?subject=x"})));
    CHECK(accepted(open_text(3, {"sms:+491701234567&body=See%20you"})));
    CHECK(accepted(open_text(4, {"", "", "0"})));
    CHECK(accepted(open_text(4, {std::string(1000, 's'), std::string(65536, 't'), "0"})));
    CHECK(accepted(open_text(4, {"Subject", "Text\twith a tab\nand a line\r\n", "2", "a.png", "b c.txt", "image/png",
                                 "text/plain"})));
    CHECK(accepted(open_text(4, {"", "", "1", "\xc3\xa4 \xd7\xa9.jpg", "image/*"})));
    CHECK(accepted(open_text(4, {"", "", "1", std::string(255, 'n'), "*/*"})));
    CHECK(accepted(open_text(4, {"", "", "10", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "a/a", "a/a", "a/a",
                                 "a/a", "a/a", "a/a", "a/a", "a/a", "a/a", "a/a"})));
    CHECK(accepted(open_text(4, {"", "", "1", "a.png", std::string(49, 'a') + "/" + std::string(50, 'b')})));
    take_requests();
    // The kind and the number of fields.
    CHECK(refused(""));
    CHECK(refused("1"));
    CHECK(refused("1\t"));                                                     // an empty URL
    CHECK(refused("0\t" + open_field("https://example.org")));
    CHECK(refused("5\t" + open_field("https://example.org")));
    CHECK(refused("01\t" + open_field("https://example.org")));
    CHECK(refused("11\t" + open_field("https://example.org")));
    CHECK(refused("a\t" + open_field("https://example.org")));
    CHECK(refused(open_text(1, {"https://example.org", ""})));                 // a field too many
    CHECK(refused(open_text(2, {"mailto:a@example.org", ""})));
    CHECK(refused(open_text(3, {})));
    CHECK(refused(open_text(4, {"", ""})));
    CHECK(refused(open_text(4, {"", "", "0", ""})));
    CHECK(refused(open_text(4, {"", "", "1", "a.png"})));                      // a name without its type
    CHECK(refused(open_text(4, {"", "", "1", "a.png", "image/png", "x"})));
    CHECK(refused(open_text(4, {"", "", "2", "a.png", "image/png"})));
    // The count of the files.
    CHECK(refused(open_text(4, {"", "", ""})));
    CHECK(refused(open_text(4, {"", "", "00"})));
    CHECK(refused(open_text(4, {"", "", "01", "a.png", "image/png"})));
    CHECK(refused(open_text(4, {"", "", "-1"})));
    CHECK(refused(open_text(4, {"", "", "1x", "a.png", "image/png"})));
    CHECK(refused(open_text(4, {"", "", "18446744073709551617", "a.png", "image/png"})));
    CHECK(refused(open_text(4, {"", "", "11", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "a/a", "a/a",
                                "a/a", "a/a", "a/a", "a/a", "a/a", "a/a", "a/a", "a/a", "a/a"})));
    // The percent-encoding: only RFC 3986's unreserved bytes raw, every % followed by two hex digits.
    CHECK(refused("1\thttps://example.org"));                                  // ':' and '/' raw
    CHECK(refused("1\thttps%3A%2F%2Fexample.org%2Fa b"));
    CHECK(refused("1\thttps%3A%2F%2Fexample.org%2F\xc3\xa4"));                 // a raw byte above 127
    CHECK(refused("1\thttps%3A%2F%2Fexample.org%2F%"));
    CHECK(refused("1\thttps%3A%2F%2Fexample.org%2F%4"));
    CHECK(refused("1\thttps%3A%2F%2Fexample.org%2F%G1"));
    CHECK(refused("1\thttps%3A%2F%2Fexample.org%2F%1g"));
    CHECK(refused("1\thttps%3A%2F%2Fexample.org%2F%%41"));
    CHECK(refused("1\thttps%3A%2F%2Fexample.org%2F%+1"));
    CHECK(refused("1\t%68ttps%3A%2F%2Fexample.org"));                          // an unreserved byte encoded
    CHECK(refused("1\thttps%3A%2F%2Fexample%2Eorg"));
    CHECK(refused("1\thttps%3A%2F%2Fexample.org%2F%7e"));
    CHECK(refused("%31\thttps%3A%2F%2Fexample.org"));                          // the kind encoded
    CHECK(refused("4\t\t\t%31\ta.png\timage%2Fpng"));                          // the count encoded
    CHECK(refused("1\thttps%3A%2F%2Fexample.org\n"));
    CHECK(refused("1\thttps%3A%2F%2Fexample.org\r"));
    CHECK(refused("1\t\thttps%3A%2F%2Fexample.org"));
    // UTF-8 once decoded, and no NUL in any field.
    for (const std::string bad : {"\x80", "\xc0\xaf", "\xe0\x80\xaf", "\xed\xa0\x80", "\xed\xa0\xbd\xed\xb8\x80",
                                  "\xf4\x90\x80\x80", "\xff", "\xe2\x82", "\xf0"}) {
        CHECK(refused(open_text(1, {"https://example.org/" + bad})));
        CHECK(refused(open_text(4, {"", "text " + bad, "0"})));
        CHECK(refused(open_text(4, {bad, "", "0"})));
        CHECK(refused(open_text(4, {"", "", "1", "a" + bad, "image/png"})));
    }
    CHECK(refused(open_text(4, {"", std::string("a\0b", 3), "0"})));
    CHECK(refused(open_text(4, {std::string("a\0b", 3), "", "0"})));
    CHECK(refused(open_text(1, {std::string("https://example.org/\0", 21)})));
    // The URL: its kind's scheme, no control, separator or formatting characters, 1 to 4096 bytes.
    for (const std::string url : {"file:///etc/passwd", "javascript:alert(1)", "data:text/html,hi",
                                  "intent://x#Intent;end", "market://details?id=a.b", "ftp://example.org",
                                  "minato://open", "mailto:a@example.org", "sms:123", "httpx://example.org",
                                  "http", "https//example.org", ":https://example.org", " https://example.org",
                                  "ht\ttps://example.org", "maps", "tel"}) {
        CHECK(refused(open_text(1, {url})));
    }
    CHECK(refused(open_text(2, {"https://example.org"})));
    CHECK(refused(open_text(2, {"sms:123"})));
    CHECK(refused(open_text(2, {"mailto"})));
    CHECK(refused(open_text(3, {"mailto:a@example.org"})));
    CHECK(refused(open_text(3, {"tel:123"})));
    CHECK(refused(open_text(3, {"smsto:123"})));
    for (const std::string bad : {"\n", "\r", "\t", "\x01", "\x1b", "\x7f", "\xc2\x85", "\xc2\x9f", "\xe2\x80\xa8",
                                  "\xe2\x80\xae", "\xe2\x81\xa6", "\xe2\x80\x8b", "\xef\xbb\xbf"}) {
        CHECK(refused(open_text(1, {"https://example.org/" + bad})));
        CHECK(refused(open_text(2, {"mailto:a@example.org" + bad})));
    }
    // No space and no other invisible character in a URL: U+0020, U+00A0, U+2009, U+3000, U+00AD, U+061C, U+180E,
    // U+E0001, U+0890, U+0891.
    for (const std::string bad : {" ", "\xc2\xa0", "\xe2\x80\x89", "\xe3\x80\x80", "\xc2\xad", "\xd8\x9c",
                                  "\xe1\xa0\x8e", "\xf3\xa0\x80\x81", "\xe0\xa2\x90", "\xe0\xa2\x91"}) {
        CHECK(refused(open_text(1, {"https://bank.example" + bad + ".evil.org"})));
        CHECK(refused(open_text(3, {"sms:123" + bad})));
    }
    CHECK(refused(open_text(1, {"https://example.org/" + std::string(4077, 'a')})));  // 4097 bytes
    // The subject and the text.
    CHECK(refused(open_text(4, {std::string(1001, 's'), "", "0"})));
    CHECK(refused(open_text(4, {"", std::string(65537, 't'), "0"})));
    // The names.
    for (const std::string &name :
         std::initializer_list<std::string>{"", ".", "..", "a/b", "/", "a\nb", "a\tb", "a\x7f", "a\xc2\x85",
                                            "a\xe2\x80\xae", "a\xe2\x80\x8b", "a\xef\xbb\xbf", std::string(256, 'n')}) {
        CHECK(refused(open_text(4, {"", "", "1", name, "image/png"})));
    }
    CHECK(refused(open_text(4, {"", "", "2", "a.png", "a.png", "image/png", "image/png"})));  // a name twice
    CHECK(refused(open_text(4, {"", "", "3", "a.png", "b.png", "a.png", "image/png", "image/png", "image/png"})));
    // A name may have U+0020, but no other space and no format character.
    for (const std::string bad :
         {"\xc2\xa0", "\xe3\x80\x80", "\xc2\xad", "\xd8\x9c", "\xf3\xa0\x80\x81", "\xe0\xa2\x90"}) {
        CHECK(refused(open_text(4, {"", "", "1", "a" + bad + ".png", "image/png"})));
    }
    // 25 fields: a share of ten files and a field more.
    CHECK(refused(open_text(4, {"", "", "10", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "a/a", "a/a", "a/a",
                                "a/a", "a/a", "a/a", "a/a", "a/a", "a/a", "a/a", "a/a"})));
    // The MIME types.
    for (const std::string &type :
         std::initializer_list<std::string>{"", "image", "image/", "/png", "image/png/x", "image/p ng", "image/p;ng",
                                            "image/p\xc3\xa4ng", std::string(50, 'a') + "/" + std::string(50, 'b')}) {
        CHECK(refused(open_text(4, {"", "", "1", "a.png", type})));
    }
    take_requests();
    // The whole text: more than 204800 bytes is refused, not cut, also without a NUL within reach.
    {
        // 1000 bytes of subject and 65536 of text, each byte encoded as %XX: 199608 bytes, within the limit; ten names
        // of 255 such bytes more are over it, though each field is within its own.
        const std::string subject(1000, '!'), text(65536, '!');
        const std::string shorter(64812, '!');  // so that the rest leaves the first name 402 bytes to the limit
        const auto text_of_share = [&]() -> const std::string & { return shorter; };
        const std::string within = open_text(4, {subject, text, "0"});
        CHECK(within.size() <= 204800);
        CHECK(accepted(within));
        take_requests();
        std::string over = open_text(4, {subject, text, "10"});
        for (char last = '0'; last <= '9'; ++last) over += "\t" + open_field(std::string(254, '!') + last);
        for (int i = 0; i < 10; ++i) over += "\ta%2Fa";
        CHECK(over.size() > 204800);
        CHECK(refused(over));
        // Exactly at the limit: the first name of '!' (3 bytes encoded) and 'a' (1 byte) makes the text 204800 bytes,
        // and one 'a' more 204801.
        const auto with_first = [&](std::size_t bangs, std::size_t as) {
            std::string text = open_text(4, {subject, text_of_share(), "10"});
            text += "\t" + open_field(std::string(bangs, '!') + std::string(as, 'a'));
            for (char last = '1'; last <= '9'; ++last) text += "\t" + open_field(std::string(254, '!') + last);
            for (int i = 0; i < 10; ++i) text += "\ta%2Fa";
            return text;
        };
        const std::size_t rest = with_first(0, 0).size();
        CHECK(rest < 204800 && 204800 - rest <= 3 * 254);
        const std::size_t wanted = 204800 - rest, bangs = (wanted > 254 ? (wanted - 254 + 1) / 2 : 0);
        const std::size_t as = wanted - 3 * bangs;
        CHECK(bangs + as + 1 <= 255);
        CHECK(with_first(bangs, as).size() == 204800);
        CHECK(accepted(with_first(bangs, as)));
        take_requests();
        CHECK(with_first(bangs, as + 1).size() == 204801);
        CHECK(refused(with_first(bangs, as + 1)));
    }
    {
        std::vector<char> unterminated(300000, 'x');  // no NUL within the limit
        CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_OPEN, address(unterminated.data()), 6) == -einval);
    }
    // The ID and the address.
    CHECK(refused(open_text(1, {"https://example.org"}), 0));
    CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_OPEN, 0, 6) == -efault);
    CHECK(take_requests().empty());
}

// An open request's text in bad guest memory gives -EFAULT, not a crash, as PICK's.
TEST(an_open_text_in_bad_guest_memory_is_efault_not_a_crash) {
    take_requests();
    const auto page = static_cast<std::size_t>(::getpagesize());
    void *const pages = ::mmap(nullptr, 2 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(pages != MAP_FAILED);
    const auto first = reinterpret_cast<std::uint64_t>(pages);
    std::memset(pages, 'a', page);                                      // no NUL anywhere in the readable page
    CHECK(::munmap(reinterpret_cast<void *>(first + page), page) == 0);  // the second page is gone
    for (const std::uint64_t text : {first + page, first + page - 8, first}) {
        CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_OPEN, text, 1) == -efault);
    }
    // A good text that ends right before the gone page is read whole.
    const std::string view = open_text(1, {"https://example.org"});
    std::memcpy(reinterpret_cast<void *>(first + page - view.size() - 1), view.c_str(), view.size() + 1);
    CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_OPEN, first + page - view.size() - 1, 1) == 0);
    CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_OPEN_CANCEL, 1) == 0);
    // So are a PICK's and a START_FAILED's, read in pieces that end at the page's end.
    static const char pick[] = "1\t0\t0\t\t";
    std::memcpy(reinterpret_cast<void *>(first + page - sizeof pick), pick, sizeof pick);
    CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_PICK, first + page - sizeof pick, 1) == 0);
    CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_PICK_CANCEL, 1) == 0);
    std::memcpy(reinterpret_cast<void *>(first + page - 2), "x", 2);
    CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_START_FAILED, first + page - 2) == 0);
    ::munmap(pages, page);
    const auto seen = take_requests();
    CHECK(seen.size() == 5 && seen[0].text == view && seen[2].text == pick && seen[4].text == "x");
}

TEST(open_requests_wait_for_their_answer_four_at_most_each_with_its_own_id) {
    take_requests();
    static const std::string view = open_text(1, {"https://example.org"});
    const auto open = [](std::uint64_t id) {
        return call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_OPEN, address(view.c_str()), id);
    };
    CHECK(open(51) == 0 && open(52) == 0 && open(53) == 0 && open(54) == 0);
    CHECK(open(55) == -ebusy);   // four wait
    CHECK(open(52) == -eexist);  // an ID that waits
    CHECK(take_requests().size() == 4);
    // The picks have a table of their own: four waiting opens leave room for a pick, with an ID an open has.
    static const char pick[] = "1\t0\t0\t\t";
    CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_PICK, address(pick), 51) == 0);
    CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_PICK_CANCEL, 51) == 0);
    CHECK(apkrt_session_post(APKRT_EVENT_PICK_RESULT, 52, 0, 0) != 0);  // a pick's answer frees no open
    CHECK(open(52) == -eexist);
    take_requests();
    // An answer frees its ID, a cancel the same and tells the host, and neither is possible twice.
    CHECK(apkrt_session_post(APKRT_EVENT_OPEN_RESULT, 51, 0, 0) != 0);
    CHECK(open(55) == 0);
    CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_OPEN_CANCEL, 52) == 0);
    const auto seen = take_requests();
    CHECK(seen.size() == 2);
    if (seen.size() == 2) {
        CHECK(seen[0].type == APKRT_REQUEST_OPEN && seen[0].argument == 55);
        CHECK(seen[1].type == APKRT_REQUEST_OPEN_CANCEL && seen[1].argument == 52 && !seen[1].had_text);
    }
    CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_OPEN_CANCEL, 52) == -enoent);
    CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_OPEN_CANCEL, 51) == -enoent);  // answered
    CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_OPEN_CANCEL, 0) == -einval);
    CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_PICK_CANCEL, 53) == -enoent);  // an open is no pick
    CHECK(take_requests().empty());
    for (const std::uint64_t id : std::initializer_list<std::uint64_t>{53, 54, 55}) {
        CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_OPEN_CANCEL, id) == 0);
    }
    take_requests();
}

TEST(an_open_handler_may_answer_inside_the_handler) {
    take_requests();
    open_answers_at_once.store(true);
    static const std::string view = open_text(1, {"tel:123"});
    CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_OPEN, address(view.c_str()), 71) == 0);
    open_answers_at_once.store(false);
    CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_OPEN_CANCEL, 71) == -enoent);  // answered
    CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_OPEN, address(view.c_str()), 71) == 0);  // free again
    CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_OPEN_CANCEL, 71) == 0);
    take_requests();
}

TEST(open_requests_from_many_threads_reach_the_handler_one_at_a_time) {
    take_requests();
    pick_in_handler.store(0);
    pick_overlap.store(false);
    slow_pick.store(true);
    static const std::string view = open_text(1, {"https://example.org"});
    std::atomic<int> taken{0};
    std::vector<std::thread> guests;
    for (std::uint64_t id = 61; id < 65; ++id) {
        guests.emplace_back([&, id] {
            if (call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_OPEN, address(view.c_str()), id) == 0) taken.fetch_add(1);
        });
    }
    for (auto &guest : guests) guest.join();
    slow_pick.store(false);
    CHECK(taken.load() == 4);
    CHECK(!pick_overlap.load());
    CHECK(take_requests().size() == 4);
    for (std::uint64_t id = 61; id < 65; ++id) {
        CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_OPEN_CANCEL, id) == 0);
    }
    take_requests();
}

// Without memory a request answers -ENOMEM and lets no exception out of the dispatch: an OPEN needs two buffers, one
// for the text and one for its decoded fields, and nothing more; a PICK needs none, also not for its waiting table.
TEST(requests_without_memory_answer_enomem_and_throw_nothing) {
    take_requests();
    // Fields longer than a std::string holds in itself, so that a decoder into strings would allocate.
    static const std::string view =
        open_text(4, {std::string(100, 's'), std::string(100, 't'), "1", "a.png", "image/png"});
    for (const long allowed : {0L, 1L, 2L}) {
        long result = 1;
        bool threw = false;
        allocations_left = allowed;
        try {
            result = call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_OPEN, address(view.c_str()), 93);
        } catch (...) {
            threw = true;
        }
        allocations_left = -1;
        CHECK(!threw);
        CHECK(result == (allowed < 2 ? -enomem : 0));
    }
    CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_OPEN_CANCEL, 93) == 0);
    static const char pick[] = "1\t0\t0\t\t";
    for (std::uint64_t id = 94; id < 98; ++id) {  // the table's four places, none of them allocated
        allocations_left = 0;
        const long result = call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_PICK, address(pick), id);
        allocations_left = -1;
        CHECK(result == 0);
    }
    for (std::uint64_t id = 94; id < 98; ++id) {
        CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_PICK_CANCEL, id) == 0);
    }
    take_requests();
}

// A text that crosses a page boundary is read whole, its NUL in the next page.
TEST(a_text_across_a_page_boundary_is_read_whole) {
    take_requests();
    const auto page = static_cast<std::size_t>(::getpagesize());
    void *const pages = ::mmap(nullptr, 2 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(pages != MAP_FAILED);
    const auto first = reinterpret_cast<std::uint64_t>(pages);
    const std::string view = open_text(1, {"https://example.org/" + std::string(200, 'a')});
    const std::uint64_t start = first + page - 100;  // 100 bytes in the first page, the rest and the NUL in the second
    std::memcpy(reinterpret_cast<void *>(start), view.c_str(), view.size() + 1);
    CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_OPEN, start, 99) == 0);
    CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_OPEN_CANCEL, 99) == 0);
    static const char reason[] = "a reason";
    std::memcpy(reinterpret_cast<void *>(first + page - 4), reason, sizeof reason);
    CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_START_FAILED, first + page - 4) == 0);
    ::munmap(pages, 2 * page);
    const auto seen = take_requests();
    CHECK(seen.size() == 3);
    if (seen.size() == 3) {
        CHECK(seen[0].text == view);
        CHECK(seen[2].text == reason);
    }
}

// '*' is a MIME type's whole type or subtype, as in */* or image/*, never a part of one.
TEST(a_star_is_a_whole_mime_token) {
    take_requests();
    const auto pick = [](const std::string &type) {
        const std::string text = "1\t0\t0\t" + type + "\t";
        const long result = call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_PICK, address(text.c_str()), 101);
        if (result == 0) CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_PICK_CANCEL, 101) == 0);
        return result;
    };
    const auto share = [](const std::string &type) {
        const std::string text = open_text(4, {"", "", "1", "a.png", type});
        const long result = call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_OPEN, address(text.c_str()), 102);
        if (result == 0) CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_OPEN_CANCEL, 102) == 0);
        return result;
    };
    for (const char *type : {"*/*", "image/*", "*/png"}) {
        CHECK(pick(type) == 0);
        CHECK(share(type) == 0);
    }
    for (const char *type : {"im*ge/png", "image/p*g", "**/png", "image/**", "*image/png", "image/png*"}) {
        CHECK(pick(type) == -einval);
        CHECK(share(type) == -einval);
    }
    take_requests();
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

// --- The window's format (the opaque layer, after Android 15's ViewRootImpl.updateOpacity) ---------

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

// --- Surfaces (ADR-0043 c4) -----------------------------------------------------------------------

TEST(a_surface_request_reaches_the_surface_handler_with_its_size_and_opacity) {
    take_requests();
    take_surfaces();
    // A make in the frame's size: the frame is 1290 x 726 pixels.
    CHECK(ask(surface(APKRT_SURFACE_MAKE, 7)) == 0);
    // A place with a fixed size and another frame: format and opacity stay the make's.
    auto place = surface(APKRT_SURFACE_PLACE, 7);
    place.z = 1;
    place.top = 0;
    place.bottom = 2796;
    place.width = 640;
    place.height = 480;
    place.format = 1;  // ignored: a format change is a remove and a make
    CHECK(ask(place) == 0);
    apkrt_surface_request remove{};  // only the op and the token count: z 0 and one side 0 are no matter
    remove.op = APKRT_SURFACE_REMOVE;
    remove.token = 7;
    remove.left = 5;
    remove.width = 7;
    remove.format = 1;
    CHECK(ask(remove) == 0);
    // RGBA_8888, TRANSLUCENT and OPAQUE, which the holder makes RGB_565, but which has no alpha either.
    for (const std::int32_t format : {1, -3, -1}) {
        auto make = surface(APKRT_SURFACE_MAKE, 8);
        make.format = format;
        CHECK(ask(make) == 0);
        CHECK(ask(surface(APKRT_SURFACE_REMOVE, 8)) == 0);
    }
    const auto seen = take_surfaces();
    CHECK(seen.size() == 9);
    CHECK(take_requests().empty());
    if (seen.size() != 9) return;

    auto made = surface(APKRT_SURFACE_MAKE, 7);
    made.width = 1290;
    made.height = 726;
    made.opaque = 1;
    CHECK(same(seen[0].request, made));
    auto placed = place;
    placed.format = 4;
    placed.opaque = 1;
    CHECK(same(seen[1].request, placed));
    apkrt_surface_request removed{};
    removed.op = APKRT_SURFACE_REMOVE;
    removed.token = 7;
    CHECK(same(seen[2].request, removed));
    CHECK(seen[3].request.format == 1 && seen[3].request.opaque == 0);
    CHECK(seen[5].request.format == -3 && seen[5].request.opaque == 0);
    CHECK(seen[7].request.format == -1 && seen[7].request.opaque == 1);
    for (const auto &each : seen) CHECK(each.thread == ::pthread_self() && each.runtime_signal_blocked);
    CHECK(!runtime_signal_blocked());
}

TEST(the_runtime_refuses_a_surface_request_the_host_could_not_take) {
    take_surfaces();
    CHECK(ask_at(0) == -efault);
    // A request on a page that is gone, and one that runs past the end of its mapping.
    const auto page = static_cast<std::size_t>(::getpagesize());
    void *const pages = ::mmap(nullptr, 2 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(pages != MAP_FAILED);
    const auto first = reinterpret_cast<std::uint64_t>(pages);
    CHECK(::munmap(reinterpret_cast<void *>(first + page), page) == 0);
    const auto made = surface(APKRT_SURFACE_MAKE, 7);
    std::memcpy(reinterpret_cast<void *>(first + page - 16), &made, 16);
    CHECK(ask_at(first + page - 16) == -efault);
    CHECK(ask_at(first + page) == -efault);
    ::munmap(pages, page);

    const auto refused = [](auto change) {
        auto request = surface(APKRT_SURFACE_MAKE, 7);
        change(request);
        return ask(request) == -einval;
    };
    CHECK(refused([](auto &r) { r.op = 0; }));
    CHECK(refused([](auto &r) { r.op = 4; }));
    CHECK(refused([](auto &r) { r.token = 0; }));
    CHECK(refused([](auto &r) { r.right = r.left; }));
    CHECK(refused([](auto &r) { r.bottom = r.top - 1; }));
    // An empty frame with a fixed size: the frame itself is refused, not only the size it would give.
    CHECK(refused([](auto &r) { r.right = r.left; r.width = 640; r.height = 480; }));
    CHECK(refused([](auto &r) { r.bottom = r.top - 1; r.width = 640; r.height = 480; }));
    for (const std::int32_t z : {0, 3, -3}) CHECK(refused([z](auto &r) { r.z = z; }));
    CHECK(refused([](auto &r) { r.width = 640; }));    // a fixed size needs both sides
    CHECK(refused([](auto &r) { r.height = 480; }));
    CHECK(refused([](auto &r) { r.width = -640; r.height = 480; }));
    CHECK(refused([](auto &r) { r.width = 8193; r.height = 480; }));
    CHECK(refused([](auto &r) { r.width = 640; r.height = 8193; }));
    CHECK(refused([](auto &r) { r.right = 8193; }));  // the frame's size is the buffer's
    CHECK(refused([](auto &r) { r.left = -(1 << 20); r.width = 640; r.height = 480; }));  // a frame side above 2^20
    CHECK(refused([](auto &r) { r.left = INT32_MIN; r.right = INT32_MAX; r.width = 640; r.height = 480; }));
    // The limits themselves pass.
    auto largest = surface(APKRT_SURFACE_MAKE, 9);
    largest.left = -(1 << 20) + 1290;
    largest.width = 8192;
    largest.height = 8192;
    CHECK(ask(largest) == 0);
    CHECK(ask(surface(APKRT_SURFACE_REMOVE, 9)) == 0);
    const auto seen = take_surfaces();
    CHECK(seen.size() == 2);
}

TEST(each_live_token_has_one_surface_and_sixteen_are_live_at_most) {
    take_surfaces();
    for (std::uint64_t token = 1; token <= 16; ++token) CHECK(ask(surface(APKRT_SURFACE_MAKE, token)) == 0);
    CHECK(ask(surface(APKRT_SURFACE_MAKE, 17)) == -enospc);
    CHECK(ask(surface(APKRT_SURFACE_MAKE, 5)) == -eexist);
    CHECK(ask(surface(APKRT_SURFACE_PLACE, 99)) == -enoent);
    CHECK(ask(surface(APKRT_SURFACE_REMOVE, 99)) == -enoent);
    CHECK(ask(surface(APKRT_SURFACE_REMOVE, 5)) == 0);
    CHECK(ask(surface(APKRT_SURFACE_PLACE, 5)) == -enoent);  // removed
    CHECK(ask(surface(APKRT_SURFACE_MAKE, 17)) == 0);         // its place is free again
    CHECK(ask(surface(APKRT_SURFACE_MAKE, 5)) == -enospc);
    // A token stays live until the guest removes it, also after the host took its surface away (SURFACE_DESTROYED):
    // its place and remove still answer 0, and the host ignores them.
    CHECK(ask(surface(APKRT_SURFACE_PLACE, 17)) == 0);
    for (std::uint64_t token = 1; token <= 17; ++token) {
        if (token != 5) CHECK(ask(surface(APKRT_SURFACE_REMOVE, token)) == 0);
    }
    const auto seen = take_surfaces();
    CHECK(seen.size() == 16 + 1 + 1 + 1 + 16);  // the makes, remove 5, make 17, place 17, the removes
    if (seen.size() < 19) return;
    CHECK(seen[16].request.op == APKRT_SURFACE_REMOVE && seen[16].request.token == 5);
    CHECK(seen[17].request.op == APKRT_SURFACE_MAKE && seen[17].request.token == 17);
    CHECK(seen[18].request.op == APKRT_SURFACE_PLACE && seen[18].request.token == 17);
}

TEST(surface_requests_from_many_threads_reach_the_host_one_at_a_time_in_each_tokens_order) {
    take_surfaces();
    std::vector<std::thread> threads;
    for (std::uint64_t token = 101; token <= 108; ++token) {
        threads.emplace_back([token] {
            for (int round = 0; round < 50; ++round) {
                CHECK(ask(surface(APKRT_SURFACE_MAKE, token)) == 0);
                auto place = surface(APKRT_SURFACE_PLACE, token);
                place.width = 100 + round;
                place.height = 100;
                CHECK(ask(place) == 0);
                CHECK(ask(surface(APKRT_SURFACE_REMOVE, token)) == 0);
            }
        });
    }
    for (auto &thread : threads) thread.join();
    const auto seen = take_surfaces();
    CHECK(seen.size() == 8 * 50 * 3);
    // Per token: make, place, remove, round after round, the place with its round's width.
    for (std::uint64_t token = 101; token <= 108; ++token) {
        std::uint32_t expected = APKRT_SURFACE_MAKE;
        int round = 0;
        bool in_order = true;
        for (const auto &each : seen) {
            if (each.request.token != token) continue;
            in_order = in_order && each.request.op == expected;
            if (expected == APKRT_SURFACE_PLACE) in_order = in_order && each.request.width == 100 + round;
            if (expected == APKRT_SURFACE_REMOVE) ++round;
            expected = expected == APKRT_SURFACE_REMOVE ? APKRT_SURFACE_MAKE : expected + 1;
        }
        CHECK(in_order && round == 50);
    }
}

TEST(one_tokens_makes_and_removes_from_two_threads_reach_the_host_in_the_order_the_runtime_took_them) {
    take_surfaces();
    // Each thread goes on until the token went back and forth `pairs` times, so that the two meet however the
    // machine schedules them (a fixed count of rounds let one thread finish before the other began, under load);
    // the bound on the rounds ends a run in which they never would.
    constexpr int pairs = 50;
    std::atomic<int> made{0}, removed{0};
    std::thread maker([&] {
        for (int round = 0; round < 1'000'000 && made.load() < pairs; ++round) {
            if (ask(surface(APKRT_SURFACE_MAKE, 200)) == 0) ++made;
        }
    });
    std::thread remover([&] {
        for (int round = 0; round < 1'000'000 && removed.load() < pairs; ++round) {
            if (ask(surface(APKRT_SURFACE_REMOVE, 200)) == 0) ++removed;
        }
    });
    maker.join();
    remover.join();
    if (made.load() > removed.load()) {
        CHECK(ask(surface(APKRT_SURFACE_REMOVE, 200)) == 0);
        ++removed;
    }
    const auto seen = take_surfaces();
    CHECK(made.load() == removed.load() && seen.size() == static_cast<std::size_t>(2 * made.load()));
    bool alternating = true;
    for (std::size_t index = 0; index < seen.size(); ++index) {
        alternating = alternating && seen[index].request.op == (index % 2 == 0 ? APKRT_SURFACE_MAKE : APKRT_SURFACE_REMOVE);
    }
    CHECK(alternating && made.load() == pairs);
}

// --- Window requests (ADR-0041, gap 4) ------------------------------------------------------------

TEST(a_window_request_reaches_the_window_handler_with_its_size_and_opacity) {
    take_requests();
    take_windows();
    // A dialog of the session's activity in its frame's size, 1098 x 796 pixels.
    CHECK(ask_window(window_of(APKRT_WINDOW_MAKE, 7)) == 0);
    // A place with a fixed size, another frame, alpha and dim: type, parent, group and format stay the make's.
    auto place = window_of(APKRT_WINDOW_PLACE, 7);
    place.top = 0;
    place.bottom = 2796;
    place.width = 640;
    place.height = 480;
    place.alpha = 0x3f000000;  // 0.5f
    place.dim = 0;
    place.type = 99;
    place.group = 5;
    place.format = 4;
    CHECK(ask_window(place) == 0);
    // A sub-window on the dialog, a toast and a translucent activity's window, each with its format's opacity.
    auto panel = window_of(APKRT_WINDOW_MAKE, 8, 1000);
    panel.parent = 7;
    panel.format = 1;  // RGBA_8888
    CHECK(ask_window(panel) == 0);
    CHECK(ask_window(window_of(APKRT_WINDOW_MAKE, 9, 2005)) == 0);
    auto activity = window_of(APKRT_WINDOW_MAKE, 10, 1);
    activity.group = 1;
    activity.format = 4;  // RGB_565
    CHECK(ask_window(activity) == 0);
    apkrt_window_request remove{};  // only the op and the token count
    remove.op = APKRT_WINDOW_REMOVE;
    remove.token = 8;
    remove.type = 7;
    remove.parent = 99;
    remove.width = 5;
    remove.alpha = 0x7fc00000;  // NaN
    remove.reserved = 1;
    CHECK(ask_window(remove) == 0);
    for (const std::uint64_t token : {7ull, 9ull, 10ull}) CHECK(ask_window(window_of(APKRT_WINDOW_REMOVE, token)) == 0);
    const auto seen = take_windows();
    CHECK(seen.size() == 9);
    CHECK(take_requests().empty());
    if (seen.size() != 9) return;

    auto made = window_of(APKRT_WINDOW_MAKE, 7);
    made.width = 1098;
    made.height = 796;
    made.opaque = 0;  // TRANSLUCENT has alpha
    CHECK(same(seen[0].request, made));
    auto placed = place;
    placed.type = 2;
    placed.group = 0;
    placed.format = -3;
    placed.opaque = 0;
    CHECK(same(seen[1].request, placed));
    CHECK(seen[2].request.parent == 7 && seen[2].request.type == 1000 && seen[2].request.opaque == 0);
    CHECK(seen[3].request.type == 2005 && seen[3].request.group == 0);
    CHECK(seen[4].request.group == 1 && seen[4].request.opaque == 1);
    apkrt_window_request removed{};
    removed.op = APKRT_WINDOW_REMOVE;
    removed.token = 8;
    CHECK(same(seen[5].request, removed));
    for (const auto &each : seen) CHECK(each.thread == ::pthread_self() && each.runtime_signal_blocked);
    CHECK(!runtime_signal_blocked());
}

TEST(the_runtime_refuses_a_window_request_the_host_could_not_take) {
    take_windows();
    CHECK(ask_window_at(0) == -efault);
    const auto page = static_cast<std::size_t>(::getpagesize());
    void *const pages = ::mmap(nullptr, 2 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(pages != MAP_FAILED);
    const auto first = reinterpret_cast<std::uint64_t>(pages);
    CHECK(::munmap(reinterpret_cast<void *>(first + page), page) == 0);
    const auto made = window_of(APKRT_WINDOW_MAKE, 7);
    std::memcpy(reinterpret_cast<void *>(first + page - 16), &made, 16);
    CHECK(ask_window_at(first + page - 16) == -efault);
    ::munmap(pages, page);

    const auto refused = [](auto change, std::uint32_t op = APKRT_WINDOW_MAKE) {
        auto request = window_of(op, 7);
        change(request);
        return ask_window(request) == -einval;
    };
    CHECK(refused([](auto &r) { r.op = 0; }));
    CHECK(refused([](auto &r) { r.op = 4; }));
    CHECK(refused([](auto &r) { r.token = 0; }));
    CHECK(refused([](auto &r) { r.right = r.left; }));
    CHECK(refused([](auto &r) { r.bottom = r.top - 1; }));
    CHECK(refused([](auto &r) { r.left = -(1 << 20); r.width = 640; r.height = 480; }));  // a frame side above 2^20
    CHECK(refused([](auto &r) { r.width = 640; }));  // a fixed size needs both sides
    CHECK(refused([](auto &r) { r.width = 640; r.height = 8193; }));
    CHECK(refused([](auto &r) { r.right = r.left + 8193; }));  // the frame's size is the buffer's
    for (const std::int32_t type : {0, -1, 100, 999, 2000, 2004, 2006}) {
        CHECK(refused([type](auto &r) { r.type = type; }));
    }
    for (const std::uint32_t bits : {0x7fc00000u, 0x7f800000u, 0xbf000000u, 0x3fc00000u}) {  // NaN, inf, -0.5, 1.5
        CHECK(refused([bits](auto &r) { r.alpha = bits; }));
        CHECK(refused([bits](auto &r) { r.dim = bits; }));
    }
    CHECK(refused([](auto &r) { r.reserved = 1; }));
    CHECK(refused([](auto &r) { r.parent = 9; }));                    // a parent on a dialog
    CHECK(refused([](auto &r) { r.type = 2005; r.group = 1; }));       // a group on a toast
    CHECK(refused([](auto &r) { r.type = 1000; r.group = 1; }));       // and on a sub-window
    // A place's alpha, dim and reserved are checked too; the limits themselves pass.
    auto largest = window_of(APKRT_WINDOW_MAKE, 9);
    largest.left = -(1 << 20) + 1194;
    largest.width = 8192;
    largest.height = 8192;
    largest.alpha = 0;
    largest.dim = 0x3f800000;  // 1.0f
    CHECK(ask_window(largest) == 0);
    auto place = window_of(APKRT_WINDOW_PLACE, 9);
    place.alpha = 0x3fc00000;
    CHECK(ask_window(place) == -einval);
    place = window_of(APKRT_WINDOW_PLACE, 9);
    place.reserved = 2;
    CHECK(ask_window(place) == -einval);
    CHECK(ask_window(window_of(APKRT_WINDOW_REMOVE, 9)) == 0);
    CHECK(take_windows().size() == 2);
}

TEST(a_sub_windows_parent_is_the_sessions_window_or_a_live_window_that_is_no_sub_window) {
    take_windows();
    auto sub = [](std::uint64_t token, std::uint64_t parent) {
        auto request = window_of(APKRT_WINDOW_MAKE, token, 1002);  // TYPE_APPLICATION_SUB_PANEL
        request.parent = parent;
        return request;
    };
    CHECK(ask_window(sub(20, 0)) == 0);            // on the session's window
    CHECK(ask_window(sub(21, 99)) == -enoent);     // a parent that is not live
    CHECK(ask_window(sub(22, 20)) == -einval);     // a sub-window as parent: one level, as on Android 15
    CHECK(ask_window(window_of(APKRT_WINDOW_MAKE, 23)) == 0);
    CHECK(ask_window(sub(24, 23)) == 0);
    CHECK(ask_window(window_of(APKRT_WINDOW_REMOVE, 23)) == -ebusy);  // its sub-window is live
    CHECK(ask_window(window_of(APKRT_WINDOW_REMOVE, 24)) == 0);
    CHECK(ask_window(window_of(APKRT_WINDOW_REMOVE, 23)) == 0);
    CHECK(ask_window(sub(25, 23)) == -enoent);     // gone
    CHECK(ask_window(window_of(APKRT_WINDOW_REMOVE, 20)) == 0);
    CHECK(take_windows().size() == 6);  // the makes of 20, 23 and 24, and their removes
}

TEST(each_live_window_token_has_one_window_and_sixteen_are_live_at_most_beside_the_surfaces) {
    take_windows();
    take_surfaces();
    for (std::uint64_t token = 1; token <= 16; ++token) CHECK(ask_window(window_of(APKRT_WINDOW_MAKE, token)) == 0);
    CHECK(ask_window(window_of(APKRT_WINDOW_MAKE, 17)) == -enospc);
    CHECK(ask_window(window_of(APKRT_WINDOW_MAKE, 5)) == -eexist);
    CHECK(ask_window(window_of(APKRT_WINDOW_PLACE, 99)) == -enoent);
    CHECK(ask_window(window_of(APKRT_WINDOW_REMOVE, 99)) == -enoent);
    // The surfaces' tokens are another namespace: token 5 has a window and gets a surface too.
    CHECK(ask(surface(APKRT_SURFACE_MAKE, 5)) == 0);
    CHECK(ask(surface(APKRT_SURFACE_REMOVE, 5)) == 0);
    CHECK(ask_window(window_of(APKRT_WINDOW_REMOVE, 5)) == 0);
    CHECK(ask_window(window_of(APKRT_WINDOW_PLACE, 5)) == -enoent);
    CHECK(ask_window(window_of(APKRT_WINDOW_MAKE, 17)) == 0);  // its place is free again
    for (std::uint64_t token = 1; token <= 17; ++token) {
        if (token != 5) CHECK(ask_window(window_of(APKRT_WINDOW_REMOVE, token)) == 0);
    }
    CHECK(take_windows().size() == 16 + 1 + 1 + 16);
    CHECK(take_surfaces().size() == 2);
}

TEST(window_requests_from_many_threads_reach_the_host_one_at_a_time_in_each_tokens_order) {
    take_windows();
    std::vector<std::thread> threads;
    for (std::uint64_t token = 101; token <= 108; ++token) {
        threads.emplace_back([token] {
            for (int round = 0; round < 50; ++round) {
                CHECK(ask_window(window_of(APKRT_WINDOW_MAKE, token)) == 0);
                auto place = window_of(APKRT_WINDOW_PLACE, token);
                place.width = 100 + round;
                place.height = 100;
                CHECK(ask_window(place) == 0);
                CHECK(ask_window(window_of(APKRT_WINDOW_REMOVE, token)) == 0);
            }
        });
    }
    for (auto &thread : threads) thread.join();
    const auto seen = take_windows();
    CHECK(seen.size() == 8 * 50 * 3);
    for (std::uint64_t token = 101; token <= 108; ++token) {
        std::uint32_t expected = APKRT_WINDOW_MAKE;
        int round = 0;
        bool in_order = true;
        for (const auto &each : seen) {
            if (each.request.token != token) continue;
            in_order = in_order && each.request.op == expected;
            if (expected == APKRT_WINDOW_PLACE) in_order = in_order && each.request.width == 100 + round;
            if (expected == APKRT_WINDOW_REMOVE) ++round;
            expected = expected == APKRT_WINDOW_REMOVE ? APKRT_WINDOW_MAKE : expected + 1;
        }
        CHECK(in_order && round == 50);
    }
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
