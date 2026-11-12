// Checks the host's window registry and the window's gate (ADR-0028, decisions 6 and 7): a new window's gate is
// closed; the layer comes back retained, and NULL once the window is removed; an open gate answers at once and a
// closed one waits; a signal ends the wait with EINTR after its handler ran, and an SA_RESTART handler makes the
// runtime wait on; opening wakes the wait; removal answers "gone" at once, also to a thread that waits; the
// background kind waits until the foreground, and ID 0 ignores windows; "scheduled" gives every provider the one
// deadline and says when one misses it; the windows' buffers (ADR-0043, decision 3) take lock and post on the
// calling thread, hear of a removed window, and without them both answer ENOSYS. Host threads call the dispatcher
// directly, as blocking_test.cpp does; the guest threads are clone children that make their syscalls through the
// veneer's instructions (threads_probe.S), the gate's at a site as the converter lays it out, which a restart runs
// again (window_probe.S), and the test's own thread plays the host.
#include "signals.h"
#include "syscall_frame.h"
#include "threads.h"

#include "APKRuntime/apkrt.h"
#include "check.h"

#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach.h>
#include <pthread.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

extern "C" {
// threads_probe.S loads and stores these around a clone; here they only have to exist.
struct ProbeRegisters {
    std::uint64_t x[10];
    __uint128_t q[8];
    std::uint64_t sp;
};
ProbeRegisters probe_marks;
ProbeRegisters probe_child_registers;

long probe_clone(std::uint64_t flags, void *child_stack, int *parent_tid, void *tls, int *child_tid,
                 void (*function)(void *), void *argument);
long probe_syscall(std::uint64_t number, std::uint64_t a0, std::uint64_t a1 = 0, std::uint64_t a2 = 0,
                   std::uint64_t a3 = 0);
long probe_site_syscall(std::uint64_t number, std::uint64_t a0, std::uint64_t a1);
void blocking_sigreturn();
}

namespace {

using namespace std::chrono_literals;

// Linux: include/uapi/linux/sched.h, asm-generic/unistd.h, asm-generic/signal.h, errno.h.
constexpr std::uint64_t pthread_flags = 0x100 | 0x200 | 0x400 | 0x800 | 0x10000 | 0x40000 | 0x100000 | 0x200000;
constexpr std::uint64_t nr_futex = 98, nr_tgkill = 131, nr_rt_sigaction = 134;
constexpr std::uint64_t futex_wait = 0;
constexpr std::uint64_t sigusr1 = 10, sigusr2 = 12;
constexpr std::uint64_t sa_restart = 0x1000'0000, sa_restorer = 0x0400'0000;
constexpr long eintr = 4, eagain = 11, efault = 14, enodev = 19, einval = 22, enosys = 38, enodata = 61,
               etimedout = 110;

constexpr std::uint64_t present = APKRT_GATE_PRESENT, background = APKRT_GATE_BACKGROUND,
                        present_try = APKRT_GATE_PRESENT_TRY;
constexpr long go_on = 0, gone = 1, closed = 2;

template <typename T>
std::uint64_t address(T *pointer) {
    return reinterpret_cast<std::uint64_t>(pointer);
}

/// Waits up to `limit` for `condition`; whether it came true.
template <typename Condition>
bool within(std::chrono::nanoseconds limit, Condition condition) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (!condition()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::yield();
    }
    return true;
}

/// Whether a host thread sleeps in the kernel, as a blocking syscall leaves it.
bool sleeping(pthread_t thread) {
    thread_basic_info_data_t info;
    mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
    return ::thread_info(::pthread_mach_thread_np(thread), THREAD_BASIC_INFO, reinterpret_cast<thread_info_t>(&info),
                         &count) == KERN_SUCCESS &&
           info.run_state == TH_STATE_WAITING;
}

/// A syscall of the calling host thread, straight into the dispatcher.
long call(std::uint64_t number, std::uint64_t a0, std::uint64_t a1 = 0) {
    apkrt_syscall_frame frame{};
    frame.x[0] = a0;
    frame.x[1] = a1;
    frame.x[8] = number;
    apkrt_syscall_dispatch(&frame);
    return static_cast<long>(frame.x[0]);
}

long gate(std::uint64_t id, std::uint64_t kind) {
    return call(APKRT_NR_WINDOW_GATE, id, kind);
}

/// A blocking call on a host thread of its own, which the test watches.
class Waiter {
public:
    template <typename Call>
    explicit Waiter(Call function)
        : thread_([this, function] {
              apkrt::signals::current();  // the thread's signal state, before the call
              started_.store(true);
              result_.store(function());
              finished_.store(true);
          }) {}
    Waiter(const Waiter &) = delete;
    Waiter &operator=(const Waiter &) = delete;
    ~Waiter() {
        if (thread_.joinable()) thread_.join();
    }

    /// Whether the thread sleeps in its call within two seconds, and still a moment later.
    bool sleeps() {
        return within(2s, [&] {
            if (!started_.load() || finished_.load() || !sleeping(thread_.native_handle())) return false;
            std::this_thread::sleep_for(2ms);
            return !finished_.load() && sleeping(thread_.native_handle());
        });
    }

    bool finishes_within(std::chrono::nanoseconds limit) {
        return within(limit, [&] { return finished_.load(); });
    }

    /// The call's result once it came back within two seconds; otherwise the test gives up.
    long result() {
        if (!finishes_within(2s)) {
            std::fprintf(stderr, "window_test: a call does not come back, giving up\n");
            std::_Exit(1);
        }
        thread_.join();
        return result_.load();
    }

private:
    std::atomic<bool> started_{false};
    std::atomic<bool> finished_{false};
    std::atomic<long> result_{-1};
    std::thread thread_;  // last: it starts once the members above exist
};

// --- The window, host side ----------------------------------------------------------------------

TEST(a_new_window_has_an_id_of_its_own_its_size_and_no_presents) {
    const auto first = apkrt_window_add(nullptr, 1080, 2340);
    const auto second = apkrt_window_add(nullptr, 1280, 720);
    CHECK(first >= 1);
    CHECK(second > first);
    std::int32_t width = 0, height = 0;
    CHECK(apkrt_window_size(first, &width, &height) == 0);
    CHECK(width == 1080 && height == 2340);
    CHECK(apkrt_window_size(second, &width, &height) == 0);
    CHECK(width == 1280 && height == 720);
    CHECK(apkrt_window_presents(first) == 0);
    apkrt_window_remove(first);
    apkrt_window_remove(second);
}

TEST(the_layer_comes_back_retained_and_null_once_the_window_is_removed) {
    // Any CoreFoundation object stands in for the CAMetalLayer: the registry only retains it.
    CFMutableArrayRef layer = CFArrayCreateMutable(nullptr, 0, &kCFTypeArrayCallBacks);
    CHECK(CFGetRetainCount(layer) == 1);
    const auto id = apkrt_window_add(const_cast<void *>(static_cast<const void *>(layer)), 1080, 2340);
    CHECK(CFGetRetainCount(layer) == 2);
    void *taken = apkrt_window_layer(id);
    CHECK(taken == layer);
    CHECK(CFGetRetainCount(layer) == 3);
    if (taken != nullptr) CFRelease(taken);
    CHECK(apkrt_window_remove(id) == 0);
    CHECK(CFGetRetainCount(layer) == 1);
    CHECK(apkrt_window_layer(id) == nullptr);
    CFRelease(layer);
    // The tests on the Mac have no layer.
    const auto without = apkrt_window_add(nullptr, 64, 64);
    CHECK(without >= 1);
    CHECK(apkrt_window_layer(without) == nullptr);
    apkrt_window_remove(without);
}

TEST(a_removed_window_is_gone_for_good) {
    const auto id = apkrt_window_add(nullptr, 1080, 2340);
    CHECK(apkrt_window_remove(id) == 0);
    CHECK(apkrt_window_remove(id) == -1);
    CHECK(apkrt_window_set_gate(id, 1) == -1);
    std::int32_t width = 7, height = 7;
    CHECK(apkrt_window_size(id, &width, &height) == -1);
    CHECK(apkrt_window_presents(id) == 0);
    CHECK(gate(id, present) == gone);
    CHECK(gate(id, background) == gone);
    // A window that comes back is a new ID.
    const auto again = apkrt_window_add(nullptr, 1080, 2340);
    CHECK(again > id);
    apkrt_window_remove(again);
}

TEST(presents_are_counted_for_the_fps_label) {
    const auto id = apkrt_window_add(nullptr, 1080, 2340);
    for (int time = 0; time < 3; ++time) apkrt_window_presented(id);
    CHECK(apkrt_window_presents(id) == 3);
    apkrt_window_presented(id + 1000);  // unknown: nothing happens
    apkrt_window_remove(id);
    CHECK(apkrt_window_presents(id) == 0);
}

// ADR-0038 c): at a turn the window keeps its ID, its gate and its presents and takes a new size, which the host
// reads with apkrt_window_size and the guest with window_size.
TEST(a_turn_gives_the_same_window_a_new_size_that_host_and_guest_read) {
    const auto id = apkrt_window_add(nullptr, 1290, 2796);
    apkrt_window_set_gate(id, 1);
    apkrt_window_presented(id);
    CHECK(apkrt_window_set_size(id, 2796, 1290) == 0);
    std::int32_t width = 0, height = 0;
    CHECK(apkrt_window_size(id, &width, &height) == 0);
    CHECK(width == 2796 && height == 1290);
    std::int32_t size[2] = {0, 0};
    CHECK(call(APKRT_NR_WINDOW_SIZE, id, address(size)) == 0);
    CHECK(size[0] == 2796 && size[1] == 1290);
    CHECK(gate(id, present_try) == go_on);
    CHECK(apkrt_window_presents(id) == 1);
    apkrt_window_remove(id);
}

TEST(a_size_not_above_0_and_a_removed_window_are_refused) {
    const auto id = apkrt_window_add(nullptr, 1290, 2796);
    CHECK(apkrt_window_set_size(id, 0, 1290) == -1);
    CHECK(apkrt_window_set_size(id, 2796, -1) == -1);
    std::int32_t width = 0, height = 0;
    CHECK(apkrt_window_size(id, &width, &height) == 0);
    CHECK(width == 1290 && height == 2796);
    CHECK(call(APKRT_NR_WINDOW_SIZE, id, 0) == -efault);
    apkrt_window_remove(id);
    CHECK(apkrt_window_set_size(id, 2796, 1290) == -1);
    std::int32_t size[2] = {7, 7};
    CHECK(call(APKRT_NR_WINDOW_SIZE, id, address(size)) == -einval);
    CHECK(call(APKRT_NR_WINDOW_SIZE, 0, address(size)) == -einval);
    CHECK(size[0] == 7 && size[1] == 7);
}

// ADR-0067 step 2: a window's render scale. The host keeps the visible window's size (apkrt_window_size) and gives the
// layer pair the low size (apkrt_window_render_size); the guest's window_size, which backs ANativeWindow_getWidth and
// _getHeight, answers the low size. Insert before the test of the turns read by several threads.

struct Sizes {
    std::int32_t full[2], render[2], guest[2];
    int status[3];
};

Sizes sizes_of(std::uint64_t id) {
    Sizes sizes{{-1, -1}, {-1, -1}, {-1, -1}, {0, 0, 0}};
    sizes.status[0] = apkrt_window_size(id, &sizes.full[0], &sizes.full[1]);
    sizes.status[1] = apkrt_window_render_size(id, &sizes.render[0], &sizes.render[1]);
    sizes.status[2] = static_cast<int>(call(APKRT_NR_WINDOW_SIZE, id, address(sizes.guest)));
    return sizes;
}

TEST(without_a_render_scale_the_render_and_the_guest_size_are_the_full_size) {
    const auto id = apkrt_window_add(nullptr, 1290, 2796);
    const Sizes sizes = sizes_of(id);
    CHECK(sizes.status[0] == 0 && sizes.status[1] == 0 && sizes.status[2] == 0);
    CHECK(sizes.full[0] == 1290 && sizes.full[1] == 2796);
    CHECK(sizes.render[0] == 1290 && sizes.render[1] == 2796);
    CHECK(sizes.guest[0] == 1290 && sizes.guest[1] == 2796);
    CHECK(apkrt_window_set_render_scale(id, 1.0) == 0);  // the same as none
    const Sizes again = sizes_of(id);
    CHECK(again.render[0] == 1290 && again.guest[1] == 2796);
    apkrt_window_remove(id);
}

TEST(a_render_scale_gives_render_and_guest_the_scaled_size_and_the_host_the_full_one) {
    const auto id = apkrt_window_add(nullptr, 1290, 2796);
    CHECK(apkrt_window_set_render_scale(id, 0.5) == 0);
    Sizes sizes = sizes_of(id);
    CHECK(sizes.full[0] == 1290 && sizes.full[1] == 2796);
    CHECK(sizes.render[0] == 645 && sizes.render[1] == 1398);
    CHECK(sizes.guest[0] == 645 && sizes.guest[1] == 1398);
    // Rounded to the nearest: 1290 * 0.67 = 864.3, 2796 * 0.67 = 1873.32.
    CHECK(apkrt_window_set_render_scale(id, 0.67) == 0);
    sizes = sizes_of(id);
    CHECK(sizes.render[0] == 864 && sizes.render[1] == 1873);
    CHECK(sizes.guest[0] == 864 && sizes.guest[1] == 1873);
    CHECK(sizes.full[0] == 1290 && sizes.full[1] == 2796);
    // Back to 1: the full size again.
    CHECK(apkrt_window_set_render_scale(id, 1.0) == 0);
    sizes = sizes_of(id);
    CHECK(sizes.render[0] == 1290 && sizes.guest[1] == 2796);
    apkrt_window_remove(id);
}

TEST(a_render_size_is_at_least_one_pixel_an_axis) {
    const auto id = apkrt_window_add(nullptr, 3, 1);
    CHECK(apkrt_window_set_render_scale(id, 0.25) == 0);
    const Sizes sizes = sizes_of(id);
    CHECK(sizes.render[0] == 1 && sizes.render[1] == 1);  // 0.75 and 0.25
    CHECK(sizes.guest[0] == 1 && sizes.guest[1] == 1);
    apkrt_window_remove(id);
}

TEST(a_turn_keeps_the_render_scale_and_the_scaled_size_follows) {
    const auto id = apkrt_window_add(nullptr, 1290, 2796);
    CHECK(apkrt_window_set_render_scale(id, 0.5) == 0);
    CHECK(apkrt_window_set_size(id, 2796, 1290) == 0);
    const Sizes sizes = sizes_of(id);
    CHECK(sizes.full[0] == 2796 && sizes.full[1] == 1290);
    CHECK(sizes.render[0] == 1398 && sizes.render[1] == 645);
    CHECK(sizes.guest[0] == 1398 && sizes.guest[1] == 645);
    apkrt_window_remove(id);
}

TEST(a_scale_out_of_range_or_for_no_window_is_refused_and_changes_nothing) {
    const auto id = apkrt_window_add(nullptr, 1290, 2796);
    CHECK(apkrt_window_set_render_scale(id, 0.5) == 0);
    for (const double bad : {0.0, 0.1, 0.2499, 1.0001, 1.5, -0.5, std::nan(""), HUGE_VAL, -HUGE_VAL}) {
        CHECK(apkrt_window_set_render_scale(id, bad) == -1);
    }
    const Sizes sizes = sizes_of(id);
    CHECK(sizes.render[0] == 645 && sizes.guest[1] == 1398);  // the 0.5 stays
    CHECK(apkrt_window_set_render_scale(0, 0.5) == -1);
    CHECK(apkrt_window_set_render_scale(id + 1000, 0.5) == -1);
    apkrt_window_remove(id);
    CHECK(apkrt_window_set_render_scale(id, 0.5) == -1);
    std::int32_t width = 7, height = 7;
    CHECK(apkrt_window_render_size(id, &width, &height) == -1);
    CHECK(apkrt_window_render_size(0, &width, &height) == -1);
    CHECK(width == 7 && height == 7);
}

TEST(a_render_scale_belongs_to_one_window_and_a_new_window_has_none) {
    const auto scaled = apkrt_window_add(nullptr, 1000, 2000);
    const auto other = apkrt_window_add(nullptr, 1000, 2000);
    CHECK(apkrt_window_set_render_scale(scaled, 0.25) == 0);
    const Sizes a = sizes_of(scaled), b = sizes_of(other);
    CHECK(a.render[0] == 250 && a.guest[1] == 500);
    CHECK(b.render[0] == 1000 && b.guest[1] == 2000);
    apkrt_window_remove(scaled);
    apkrt_window_remove(other);
    const auto fresh = apkrt_window_add(nullptr, 1000, 2000);
    CHECK(sizes_of(fresh).render[0] == 1000);
    apkrt_window_remove(fresh);
}

// The host turns its window on its main thread while render threads read its size: every read is one size whole.
// Four readers, so that one gets in between two writes of a turn that took the lock twice.
TEST(a_size_read_during_turns_is_always_one_of_them_whole) {
    const auto id = apkrt_window_add(nullptr, 1290, 2796);
    std::atomic<bool> done{false};
    std::atomic<int> torn{0};
    std::vector<std::thread> readers;
    for (int reader = 0; reader < 4; ++reader) {
        readers.emplace_back([&] {
            for (int reads = 0; !done.load() || reads < 1000; ++reads) {
                std::int32_t size[2] = {0, 0};
                const long result = call(APKRT_NR_WINDOW_SIZE, id, address(size));
                const bool portrait = size[0] == 1290 && size[1] == 2796, landscape = size[0] == 2796 && size[1] == 1290;
                if (result != 0 || (!portrait && !landscape)) torn.fetch_add(1);
            }
        });
    }
    for (int turn = 0; turn < 200000; ++turn) {
        const bool landscape = turn % 2 == 0;
        apkrt_window_set_size(id, landscape ? 2796 : 1290, landscape ? 1290 : 2796);
    }
    done.store(true);
    for (auto &reader : readers) reader.join();
    CHECK(torn.load() == 0);
    apkrt_window_remove(id);
}

// --- The windows' buffers (ADR-0043, decision 3) ------------------------------------------------

/// graphics/host's buffers as the tests play them: each call is noted with its thread, and lock and post answer
/// what the test sets. lock fills the guest's struct with a buffer of its own and the dirty rectangle 1 2 3 4.
struct FakeBuffers {
    std::mutex lock;
    std::vector<std::string> calls;
    std::vector<std::thread::id> threads;
    int lock_answer = 0, post_answer = 0, readback_answer = 0;
    std::uint32_t pixels[64 * 48];
};
FakeBuffers fake;

void note(const std::string &call) {
    const std::lock_guard<std::mutex> hold(fake.lock);
    fake.calls.push_back(call);
    fake.threads.push_back(std::this_thread::get_id());
}

int fake_lock(void *context, std::uint64_t id, apkrt_window_buffer *buffer) {
    auto *const buffers = static_cast<FakeBuffers *>(context);
    char line[160];
    std::snprintf(line, sizeof line, "lock %llu %dx%d format %d dirty %d %d %d %d", static_cast<unsigned long long>(id),
                  buffer->width, buffer->height, buffer->format, buffer->dirty[0], buffer->dirty[1], buffer->dirty[2],
                  buffer->dirty[3]);
    note(line);
    buffer->stride = buffer->width;
    buffer->bits = buffers->pixels;
    const std::int32_t dirty[4] = {1, 2, 3, 4};
    std::copy(dirty, dirty + 4, buffer->dirty);
    return buffers->lock_answer;
}

int fake_post(void *context, std::uint64_t id) {
    note("post " + std::to_string(id));
    return static_cast<FakeBuffers *>(context)->post_answer;
}

void fake_removed(void *, std::uint64_t id) {
    note("removed " + std::to_string(id));
}

/// The host's readback: notes the id and the request, and on a 0 answer fills the runtime's staging (never guest
/// memory) with a known pixel, so the test sees the copy reach the guest. stride is a multiple of four in the tests.
int fake_readback(void *context, std::uint64_t id, const apkrt_window_readback *request) {
    auto *const buffers = static_cast<FakeBuffers *>(context);
    char line[160];
    std::snprintf(line, sizeof line, "readback %llu %d %d %d %d -> %ux%u stride %u",
                  static_cast<unsigned long long>(id),
                  request->left, request->top, request->right, request->bottom, request->width, request->height,
                  request->stride);
    note(line);
    if (buffers->readback_answer == 0) {
        auto *const out = static_cast<std::uint32_t *>(request->pixels);
        for (std::size_t i = 0; i < static_cast<std::size_t>(request->height) * (request->stride / 4); ++i) {
            out[i] = 0xDDCCBBAAu;
        }
    }
    return buffers->readback_answer;
}

const apkrt_window_buffers fake_buffers{fake_lock, fake_post, fake_removed, &fake, nullptr, fake_readback};

/// The calls so far, which it forgets; with `on_this_thread`, whether each came on the calling thread.
std::vector<std::string> taken_calls(bool *on_this_thread = nullptr) {
    const std::lock_guard<std::mutex> hold(fake.lock);
    if (on_this_thread != nullptr) {
        *on_this_thread = std::all_of(fake.threads.begin(), fake.threads.end(),
                                      [](std::thread::id thread) { return thread == std::this_thread::get_id(); });
    }
    fake.threads.clear();
    fake.lock_answer = fake.post_answer = fake.readback_answer = 0;
    return std::exchange(fake.calls, {});
}

apkrt_window_buffer whole(std::int32_t width, std::int32_t height, std::int32_t format) {
    return {width, height, 0, format, nullptr, {0, 0, width, height}};
}

TEST(without_buffers_lock_and_post_answer_enosys_as_the_stubs_did) {
    CHECK(apkrt_set_window_buffers(nullptr) == 0);
    const auto id = apkrt_window_add(nullptr, 64, 48);
    auto buffer = whole(64, 48, 1);
    CHECK(call(APKRT_NR_WINDOW_LOCK, id, address(&buffer)) == -enosys);
    CHECK(call(APKRT_NR_WINDOW_POST, id) == -enosys);
    CHECK(buffer.bits == nullptr);
    apkrt_window_remove(id);
}

TEST(the_buffers_come_whole_or_not_at_all) {
    CHECK(apkrt_set_window_buffers(nullptr) == 0);
    const apkrt_window_buffers without_post{fake_lock, nullptr, fake_removed, &fake, nullptr, nullptr};
    const apkrt_window_buffers without_lock{nullptr, fake_post, fake_removed, &fake, nullptr, nullptr};
    const apkrt_window_buffers without_removed{fake_lock, fake_post, nullptr, &fake, nullptr, nullptr};
    CHECK(apkrt_set_window_buffers(&without_post) == -1);
    CHECK(apkrt_set_window_buffers(&without_lock) == -1);
    CHECK(apkrt_set_window_buffers(&without_removed) == -1);
    const auto id = apkrt_window_add(nullptr, 64, 48);
    auto buffer = whole(64, 48, 1);
    CHECK(call(APKRT_NR_WINDOW_LOCK, id, address(&buffer)) == -enosys);  // still none
    apkrt_window_remove(id);
    CHECK(taken_calls().empty());
}

TEST(lock_and_post_reach_the_buffers_on_the_calling_thread_with_the_guest_s_buffer) {
    CHECK(apkrt_set_window_buffers(&fake_buffers) == 0);
    const auto id = apkrt_window_add(nullptr, 64, 48);
    apkrt_window_set_gate(id, 1);
    apkrt_window_buffer buffer{50, 20, 0, 4, nullptr, {10, 5, 30, 15}};
    CHECK(call(APKRT_NR_WINDOW_LOCK, id, address(&buffer)) == 0);
    CHECK(buffer.stride == 50 && buffer.bits == fake.pixels);
    CHECK(buffer.dirty[0] == 1 && buffer.dirty[1] == 2 && buffer.dirty[2] == 3 && buffer.dirty[3] == 4);
    CHECK(call(APKRT_NR_WINDOW_POST, id) == 0);
    bool on_this_thread = false;
    const std::string with_id = std::to_string(id);
    CHECK(taken_calls(&on_this_thread) ==
          std::vector<std::string>({"lock " + with_id + " 50x20 format 4 dirty 10 5 30 15", "post " + with_id}));
    CHECK(on_this_thread);
    // Their answers are the calls'.
    fake.lock_answer = -einval;
    CHECK(call(APKRT_NR_WINDOW_LOCK, id, address(&buffer)) == -einval);
    fake.post_answer = -enodev;
    CHECK(call(APKRT_NR_WINDOW_POST, id) == -enodev);
    taken_calls();
    apkrt_window_remove(id);
    CHECK(taken_calls() == std::vector<std::string>({"removed " + with_id}));
}

// window_readback (ADR-0043 amendment, the Magnifier's loupe): the runtime guards the request, refuses a window the
// guest does not own and a rectangle the host rejects, then copies the host's pixels out to the guest.
TEST(window_readback_copies_the_rectangle_out_and_guards_the_request) {
    CHECK(apkrt_set_window_buffers(&fake_buffers) == 0);
    const auto id = apkrt_window_add(nullptr, 64, 48);
    const std::string with_id = std::to_string(id);
    std::uint32_t out[10 * 8] = {};
    const apkrt_window_readback_request request{5, 4, 15, 12, 10, 8, 10 * 4, 1, address(out)};

    // A valid request: the host's pixels reach the guest, and the host got the rectangle and the destination size.
    CHECK(call(APKRT_NR_WINDOW_READBACK, id, address(&request)) == 0);
    CHECK(out[0] == 0xDDCCBBAAu && out[10 * 8 - 1] == 0xDDCCBBAAu);
    CHECK(taken_calls() == std::vector<std::string>({"readback " + with_id + " 5 4 15 12 -> 10x8 stride 40"}));

    // A window the guest does not own is refused with -ENODATA, before the host is asked.
    CHECK(call(APKRT_NR_WINDOW_READBACK, id + 1000, address(&request)) == -enodata);
    CHECK(taken_calls().empty());

    // A rectangle outside the window: the host answers -EINVAL and the runtime passes it on.
    fake.readback_answer = -einval;
    CHECK(call(APKRT_NR_WINDOW_READBACK, id, address(&request)) == -einval);
    CHECK(taken_calls() == std::vector<std::string>({"readback " + with_id + " 5 4 15 12 -> 10x8 stride 40"}));

    // The runtime's own guards, each before the host: format, sizes, stride, the byte cap, the addresses.
    auto refused = [&](apkrt_window_readback_request bad) { return call(APKRT_NR_WINDOW_READBACK, id, address(&bad)); };
    apkrt_window_readback_request bad = request;
    bad.format = 5;
    CHECK(refused(bad) == -einval);
    bad = request, bad.width = 0;
    CHECK(refused(bad) == -einval);
    bad = request, bad.height = 0;
    CHECK(refused(bad) == -einval);
    bad = request, bad.stride = 10 * 4 - 1;
    CHECK(refused(bad) == -einval);
    // Over APKRT_HARDWARE_BUFFER_MAX_BYTES.
    bad = request, bad.width = 0x10000, bad.height = 0x10000, bad.stride = 0x10000 * 4;
    CHECK(refused(bad) == -einval);
    CHECK(call(APKRT_NR_WINDOW_READBACK, id, 0) == -efault);  // no request
    CHECK(taken_calls().empty());  // none of these reached the host
    apkrt_window_remove(id);
    taken_calls();
}

TEST(window_readback_without_a_host_read_path_answers_enodata) {
    const apkrt_window_buffers no_readback{fake_lock, fake_post, fake_removed, &fake, nullptr, nullptr};
    CHECK(apkrt_set_window_buffers(&no_readback) == 0);
    const auto id = apkrt_window_add(nullptr, 64, 48);
    std::uint32_t out[2 * 2] = {};
    const apkrt_window_readback_request request{0, 0, 2, 2, 2, 2, 2 * 4, 1, address(out)};
    CHECK(call(APKRT_NR_WINDOW_READBACK, id, address(&request)) == -enodata);
    CHECK(taken_calls().empty());
    apkrt_window_remove(id);
    taken_calls();
}

TEST(lock_and_post_need_a_window_the_host_gave_and_lock_a_buffer_and_a_removed_window_is_gone) {
    CHECK(apkrt_set_window_buffers(&fake_buffers) == 0);
    const auto id = apkrt_window_add(nullptr, 64, 48);
    auto buffer = whole(64, 48, 1);
    CHECK(call(APKRT_NR_WINDOW_LOCK, id, 0) == -efault);
    CHECK(call(APKRT_NR_WINDOW_LOCK, 0, address(&buffer)) == -einval);
    CHECK(call(APKRT_NR_WINDOW_LOCK, id + 1000, address(&buffer)) == -einval);  // never given
    CHECK(call(APKRT_NR_WINDOW_POST, 0) == -einval);
    CHECK(call(APKRT_NR_WINDOW_POST, id + 1000) == -einval);
    CHECK(apkrt_window_remove(id) == 0);
    CHECK(call(APKRT_NR_WINDOW_LOCK, id, address(&buffer)) == -enodev);  // as Android's abandoned BufferQueue
    CHECK(taken_calls() == std::vector<std::string>({"removed " + std::to_string(id)}));
}

TEST(a_post_after_the_removal_still_reaches_the_buffers_which_take_their_buffer_back) {
    CHECK(apkrt_set_window_buffers(&fake_buffers) == 0);
    const auto id = apkrt_window_add(nullptr, 64, 48);
    auto buffer = whole(64, 48, 2);
    CHECK(call(APKRT_NR_WINDOW_LOCK, id, address(&buffer)) == 0);
    CHECK(apkrt_window_remove(id) == 0);
    fake.post_answer = -enodev;
    CHECK(call(APKRT_NR_WINDOW_POST, id) == -enodev);
    const std::string with_id = std::to_string(id);
    CHECK(taken_calls() == std::vector<std::string>({"lock " + with_id + " 64x48 format 2 dirty 0 0 64 48",
                                                     "removed " + with_id, "post " + with_id}));
    CHECK(apkrt_set_window_buffers(nullptr) == 0);
}

// The first-frame line (MINATO_LOADER_LOG, 3 s and 10 s after the window came): the guest's software locks and posts,
// each answered or failed, and the host's presents by kind, beside the window's size and gate.
TEST(the_first_frame_line_counts_software_locks_and_posts_and_each_kind_of_present) {
    CHECK(apkrt_set_window_buffers(&fake_buffers) == 0);
    const auto id = apkrt_window_add(nullptr, 64, 48);
    char line[256];
    CHECK(apkrt_window_diagnostic(id, line, sizeof line) == 0);
    CHECK(std::string(line) == "64x48, gate closed, software locks 0 (failed 0), posts 0 (failed 0), buffers shown 0, "
                                "hidden as transparent 0, EGL swaps 0, Vulkan presents 0, video frames 0");
    apkrt_window_set_gate(id, 1);
    auto buffer = whole(64, 48, 1);
    CHECK(call(APKRT_NR_WINDOW_LOCK, id, address(&buffer)) == 0);
    CHECK(call(APKRT_NR_WINDOW_POST, id) == 0);
    fake.lock_answer = -einval;
    CHECK(call(APKRT_NR_WINDOW_LOCK, id, address(&buffer)) == -einval);
    fake.post_answer = -enodev;
    CHECK(call(APKRT_NR_WINDOW_POST, id) == -enodev);
    taken_calls();
    for (int time = 0; time < 2; ++time) apkrt_window_presented_as(id, APKRT_PRESENT_EGL);
    apkrt_window_presented_as(id, APKRT_PRESENT_VULKAN);
    for (int time = 0; time < 3; ++time) apkrt_window_presented_as(id, APKRT_PRESENT_BUFFER_SHOWN);
    apkrt_window_presented_as(id, APKRT_PRESENT_BUFFER_HIDDEN);
    apkrt_window_presented_as(id, APKRT_PRESENT_VIDEO);
    apkrt_window_presented_as(id, 99);  // no kind of its own: a present all the same
    apkrt_window_presented(id);
    CHECK(apkrt_window_presents(id) == 10);
    CHECK(apkrt_window_diagnostic(id, line, sizeof line) == 0);
    CHECK(std::string(line) == "64x48, gate open, software locks 1 (failed 1), posts 1 (failed 1), buffers shown 3, "
                                "hidden as transparent 1, EGL swaps 2, Vulkan presents 1, video frames 1");
    char small[8];
    CHECK(apkrt_window_diagnostic(id, small, sizeof small) == 0);  // cut, and ended
    CHECK(std::string(small) == "64x48, ");
    CHECK(apkrt_window_remove(id) == 0);
    CHECK(apkrt_window_diagnostic(id, line, sizeof line) == -1);
    taken_calls();
    CHECK(apkrt_set_window_buffers(nullptr) == 0);
}

// With MINATO_LOADER_LOG the runtime writes the line itself, 3 s after the window came, tagged apkrt.
TEST(with_the_loader_log_the_first_frame_line_comes_3_s_after_the_window) {
    char path[] = "/tmp/window_test_first_frame_XXXXXX";
    const int file = ::mkstemp(path);
    CHECK(file >= 0);
    const int saved = ::dup(STDERR_FILENO);
    ::dup2(file, STDERR_FILENO);
    ::setenv("MINATO_LOADER_LOG", "1", 1);
    const auto id = apkrt_window_add(nullptr, 64, 48);
    ::unsetenv("MINATO_LOADER_LOG");
    apkrt_window_presented_as(id, APKRT_PRESENT_EGL);
    const std::string expected = "I/apkrt   : window " + std::to_string(id) +
                                 " at 3 s: 64x48, gate closed, software locks 0 (failed 0), posts 0 (failed 0), "
                                 "buffers shown 0, hidden as transparent 0, EGL swaps 1, Vulkan presents 0, "
                                 "video frames 0\n";
    std::string text;
    for (int round = 0; round < 50 && text.find(expected) == std::string::npos; ++round) {
        std::this_thread::sleep_for(100ms);
        if (round < 25) continue;  // not before 2.5 s
        char bytes[1024];
        const ssize_t length = ::pread(file, bytes, sizeof bytes, 0);
        text.assign(bytes, length > 0 ? static_cast<std::size_t>(length) : 0);
    }
    ::dup2(saved, STDERR_FILENO);
    ::close(saved);
    ::close(file);
    ::unlink(path);
    CHECK(text == expected);
    apkrt_window_remove(id);
}

// --- The gate -----------------------------------------------------------------------------------

TEST(a_new_window_s_gate_is_closed_and_opening_it_wakes_the_present) {
    const auto id = apkrt_window_add(nullptr, 1080, 2340);
    Waiter waiter([id] { return gate(id, present); });
    CHECK(waiter.sleeps());
    CHECK(apkrt_window_set_gate(id, 1) == 0);
    CHECK(waiter.result() == go_on);
    apkrt_window_remove(id);
}

TEST(an_open_gate_answers_at_once_and_a_closed_one_waits_again) {
    const auto id = apkrt_window_add(nullptr, 1080, 2340);
    CHECK(apkrt_window_set_gate(id, 1) == 0);
    CHECK(gate(id, present) == go_on);
    CHECK(apkrt_window_set_gate(id, 0) == 0);
    Waiter waiter([id] { return gate(id, present); });
    CHECK(waiter.sleeps());
    CHECK(apkrt_window_set_gate(id, 1) == 0);
    CHECK(waiter.result() == go_on);
    apkrt_window_remove(id);
}

TEST(removal_answers_gone_at_once_also_to_a_thread_that_waits) {
    const auto id = apkrt_window_add(nullptr, 1080, 2340);
    Waiter waiter([id] { return gate(id, present); });
    CHECK(waiter.sleeps());
    CHECK(apkrt_window_remove(id) == 0);
    CHECK(waiter.result() == gone);
    CHECK(gate(id, present) == gone);
}

TEST(an_unknown_window_is_gone_and_a_present_needs_a_window) {
    CHECK(gate(1'000'000, present) == gone);
    CHECK(gate(1'000'000, background) == gone);
    CHECK(gate(1'000'000, present_try) == gone);
    CHECK(gate(0, present) == -einval);
    CHECK(gate(0, present_try) == -einval);
    const auto id = apkrt_window_add(nullptr, 1080, 2340);
    CHECK(gate(id, 0) == -einval);
    CHECK(gate(id, 4) == -einval);
    apkrt_window_remove(id);
}

TEST(the_trying_present_never_waits_and_tells_a_closed_gate) {
    const auto id = apkrt_window_add(nullptr, 1080, 2340);
    CHECK(gate(id, present_try) == closed);  // a new window's gate
    CHECK(apkrt_window_set_gate(id, 1) == 0);
    CHECK(gate(id, present_try) == go_on);
    apkrt_set_background(1);  // the background is not the present's business
    CHECK(gate(id, present_try) == go_on);
    apkrt_set_background(0);
    CHECK(apkrt_window_set_gate(id, 0) == 0);
    CHECK(gate(id, present_try) == closed);
    CHECK(apkrt_window_remove(id) == 0);
    CHECK(gate(id, present_try) == gone);
}

// --- The background -----------------------------------------------------------------------------

TEST(the_background_kind_waits_until_the_foreground) {
    CHECK(apkrt_is_background() == 0);
    CHECK(gate(0, background) == go_on);
    const auto id = apkrt_window_add(nullptr, 1080, 2340);
    CHECK(gate(id, background) == go_on);  // the window's gate does not matter here
    apkrt_set_background(1);
    CHECK(apkrt_is_background() == 1);
    Waiter without_window([] { return gate(0, background); });
    Waiter with_window([id] { return gate(id, background); });
    CHECK(without_window.sleeps());
    CHECK(with_window.sleeps());
    apkrt_set_background(0);
    CHECK(apkrt_is_background() == 0);
    CHECK(without_window.result() == go_on);
    CHECK(with_window.result() == go_on);
    apkrt_window_remove(id);
}

TEST(in_the_background_a_removed_window_answers_gone_and_id_0_waits_on) {
    const auto id = apkrt_window_add(nullptr, 1080, 2340);
    apkrt_set_background(1);
    Waiter without_window([] { return gate(0, background); });
    Waiter with_window([id] { return gate(id, background); });
    Waiter presenting([id] { return gate(id, present); });
    CHECK(without_window.sleeps());
    CHECK(with_window.sleeps());
    CHECK(presenting.sleeps());
    CHECK(apkrt_window_remove(id) == 0);
    CHECK(with_window.result() == gone);
    CHECK(presenting.result() == gone);
    CHECK(without_window.sleeps());  // ID 0 ignores windows
    apkrt_set_background(0);
    CHECK(without_window.result() == go_on);
}

TEST(the_foreground_wait_of_host_code_returns_in_the_foreground) {
    apkrt_wait_foreground();  // at once
    apkrt_set_background(1);
    std::atomic<bool> returned{false};
    std::thread host([&] {
        apkrt_wait_foreground();
        returned.store(true);
    });
    std::this_thread::sleep_for(50ms);
    CHECK(!returned.load());
    apkrt_set_background(0);
    CHECK(within(2s, [&] { return returned.load(); }));
    host.join();
}

TEST(the_background_counts_the_waits_at_the_gate_and_the_longest) {
    std::uint32_t parked = 9;
    std::uint64_t longest = 9;
    apkrt_set_background(1);  // a new trip counts from nothing
    apkrt_background_gate(&parked, &longest);
    CHECK(parked == 0 && longest == 0);
    std::thread first([] { apkrt_wait_foreground(); });
    std::thread second([] {
        std::this_thread::sleep_for(30ms);
        apkrt_wait_foreground();
    });
    std::this_thread::sleep_for(80ms);
    apkrt_set_background(0);
    first.join();
    second.join();
    apkrt_wait_foreground();  // in the foreground: no wait, not counted
    apkrt_background_gate(&parked, &longest);
    CHECK(parked == 2);
    CHECK(longest >= 80'000'000 && longest < 2'000'000'000);
    apkrt_set_background(1);
    apkrt_background_gate(&parked, &longest);
    CHECK(parked == 0 && longest == 0);
    apkrt_set_background(0);
}

// --- Guest threads: signals at the gate ---------------------------------------------------------

std::atomic<int> handled[64];

/// The guest handler: counts.
void on_signal(int signal) {
    if (signal > 0 && signal < 64) handled[signal].fetch_add(1);
}

/// SIGUSR1 goes to on_signal without SA_RESTART, SIGUSR2 with it.
void install_handlers() {
    struct Action {
        std::uint64_t handler, flags, restorer, mask;
    };
    const Action plain{address(&on_signal), sa_restorer, address(&blocking_sigreturn), 0};
    const Action restarting{address(&on_signal), sa_restart | sa_restorer, address(&blocking_sigreturn), 0};
    CHECK(probe_syscall(nr_rt_sigaction, sigusr1, address(&plain), 0, 8) == 0);
    CHECK(probe_syscall(nr_rt_sigaction, sigusr2, address(&restarting), 0, 8) == 0);
}

constexpr std::size_t stack_size = 256 * 1024;

/// A clone child that runs `function(argument)` as guest code on a stack of its own, the way bionic's
/// pthread_create starts a thread.
class Child {
public:
    Child(void (*function)(void *), void *argument)
        : stack_(::mmap(nullptr, stack_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0)) {
        CHECK(stack_ != MAP_FAILED);
        tid_ = static_cast<std::int32_t>(probe_clone(pthread_flags, static_cast<char *>(stack_) + stack_size,
                                                     &tid_word_, nullptr, &tid_word_, function, argument));
        CHECK(tid_ > 0);
    }
    Child(const Child &) = delete;
    Child &operator=(const Child &) = delete;
    ~Child() {
        join();
        ::munmap(stack_, stack_size);
    }

    std::uint64_t tid() const { return static_cast<std::uint64_t>(tid_); }

    /// The host thread it runs on.
    pthread_t host() const {
        pthread_t found = nullptr;
        CHECK(within(2s, [&] {
            apkrt::threads::find(tid_, [&](apkrt::threads::GuestThread &thread) { found = thread.signals.host; });
            return found != nullptr;
        }));
        return found;
    }

    /// pthread_join's wait: until the kernel clears the thread ID word at the thread's exit.
    void join() {
        struct {
            std::int64_t seconds, nanoseconds;
        } const slice{0, 100'000'000};
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        for (int value; (value = __atomic_load_n(&tid_word_, __ATOMIC_ACQUIRE)) != 0;) {
            if (std::chrono::steady_clock::now() >= deadline) {
                std::fprintf(stderr, "window_test: a guest thread does not end, giving up\n");
                std::_Exit(1);
            }
            const long result = probe_syscall(nr_futex, address(&tid_word_), futex_wait,
                                              static_cast<std::uint64_t>(value), address(&slice));
            CHECK(result == 0 || result == -eagain || result == -etimedout);
        }
    }

private:
    void *stack_;
    int tid_word_ = 0;
    std::int32_t tid_ = 0;
};

/// The guest thread of the next tests: one call of the gate.
struct GuestGate {
    std::uint64_t id = 0;
    std::uint64_t kind = present;
    std::atomic<int> phase{0};  // 1: in the call, 2: past it
    std::atomic<long> result{-1};
};

void pass_the_gate(void *argument) {
    auto &guest = *static_cast<GuestGate *>(argument);
    guest.phase.store(1);
    guest.result.store(probe_site_syscall(APKRT_NR_WINDOW_GATE, guest.id, guest.kind));
    guest.phase.store(2);
}

TEST(a_signal_ends_the_wait_at_the_gate_with_eintr_after_its_handler_ran) {
    for (const auto kind : {present, background}) {
        static GuestGate guest;
        guest.phase.store(0);
        guest.kind = kind;
        guest.id = apkrt_window_add(nullptr, 1080, 2340);
        if (kind == background) apkrt_set_background(1);
        const int before = handled[sigusr1].load();
        {
            Child child(pass_the_gate, &guest);
            const auto host = child.host();
            CHECK(within(2s, [&] { return guest.phase.load() == 1 && sleeping(host); }));
            CHECK(probe_syscall(nr_tgkill, static_cast<std::uint64_t>(::getpid()), child.tid(), sigusr1) == 0);
            CHECK(within(2s, [&] { return guest.phase.load() == 2; }));
            if (guest.phase.load() != 2) apkrt_window_remove(guest.id);  // lets a thread that waits on go
        }
        CHECK(guest.result.load() == -eintr);
        CHECK(handled[sigusr1].load() == before + 1);
        apkrt_set_background(0);
        apkrt_window_remove(guest.id);
    }
}

TEST(an_sa_restart_handler_makes_the_runtime_wait_on_at_the_gate) {
    for (const auto kind : {present, background}) {
        static GuestGate guest;
        guest.phase.store(0);
        guest.kind = kind;
        guest.id = apkrt_window_add(nullptr, 1080, 2340);
        if (kind == background) apkrt_set_background(1);
        const int before = handled[sigusr2].load();
        {
            Child child(pass_the_gate, &guest);
            const auto host = child.host();
            CHECK(within(2s, [&] { return guest.phase.load() == 1 && sleeping(host); }));
            CHECK(probe_syscall(nr_tgkill, static_cast<std::uint64_t>(::getpid()), child.tid(), sigusr2) == 0);
            CHECK(within(2s, [&] { return handled[sigusr2].load() == before + 1; }));
            // The handler ran, and the thread waits again.
            CHECK(within(2s, [&] { return sleeping(host); }));
            std::this_thread::sleep_for(20ms);
            CHECK(guest.phase.load() == 1);
            if (kind == present) {
                CHECK(apkrt_window_set_gate(guest.id, 1) == 0);
            } else {
                apkrt_set_background(0);
            }
            CHECK(within(2s, [&] { return guest.phase.load() == 2; }));
            if (guest.phase.load() != 2) apkrt_window_remove(guest.id);
        }
        CHECK(guest.result.load() == go_on);
        apkrt_set_background(0);
        apkrt_window_remove(guest.id);
    }
}

TEST(the_host_s_calls_come_from_a_host_thread_while_guest_threads_wait) {
    static GuestGate presenting, submitting;
    presenting.id = apkrt_window_add(nullptr, 1080, 2340);
    presenting.kind = present;
    submitting.id = 0;
    submitting.kind = background;
    apkrt_set_background(1);
    {
        Child first(pass_the_gate, &presenting);
        Child second(pass_the_gate, &submitting);
        const auto first_host = first.host(), second_host = second.host();
        CHECK(within(2s, [&] { return presenting.phase.load() == 1 && sleeping(first_host); }));
        CHECK(within(2s, [&] { return submitting.phase.load() == 1 && sleeping(second_host); }));
        std::thread app([&] {
            apkrt_window_remove(presenting.id);
            apkrt_set_background(0);
        });
        app.join();
        CHECK(within(2s, [&] { return presenting.phase.load() == 2 && submitting.phase.load() == 2; }));
    }
    CHECK(presenting.result.load() == gone);
    CHECK(submitting.result.load() == go_on);
}

// --- "Scheduled" --------------------------------------------------------------------------------

/// A provider of "scheduled" that the tests steer.
struct Provider {
    std::atomic<int> answer{1};
    std::atomic<std::int64_t> sleep_ns{0};  // before it answers
    std::atomic<std::uint64_t> deadline{0};
    std::atomic<int> calls{0};
    std::atomic<pthread_t> thread{nullptr};
};

int provide(void *context, std::uint64_t deadline) {
    auto &provider = *static_cast<Provider *>(context);
    provider.deadline.store(deadline);
    provider.thread.store(::pthread_self());
    provider.calls.fetch_add(1);
    std::this_thread::sleep_for(std::chrono::nanoseconds(provider.sleep_ns.load()));
    return provider.answer.load();
}

Provider providers[2];

std::uint64_t uptime_ns() {
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

TEST(scheduled_without_providers_answers_in_time) {
    CHECK(apkrt_wait_scheduled(1.0) == 1);
}

TEST(every_provider_gets_the_one_deadline_on_the_calling_thread) {
    CHECK(apkrt_add_scheduled_provider(provide, &providers[0]) == 0);
    CHECK(apkrt_add_scheduled_provider(provide, &providers[1]) == 0);
    providers[0].sleep_ns.store(20'000'000);  // the second one still gets the first one's deadline
    const auto before = uptime_ns();
    CHECK(apkrt_wait_scheduled(1.0) == 1);
    const auto after = uptime_ns();
    for (auto &provider : providers) {
        CHECK(provider.calls.load() == 1);
        CHECK(provider.thread.load() == ::pthread_self());
        CHECK(provider.deadline.load() >= before + 1'000'000'000 && provider.deadline.load() <= after + 1'000'000'000);
    }
    CHECK(providers[0].deadline.load() == providers[1].deadline.load());
    providers[0].sleep_ns.store(0);
}

TEST(a_provider_that_misses_the_deadline_makes_the_call_say_so) {
    providers[0].answer.store(0);
    CHECK(apkrt_wait_scheduled(0.2) == 0);
    CHECK(providers[1].calls.load() == 2);  // the others are asked all the same
    providers[0].answer.store(1);
    // An answer after the deadline is too late, even a yes.
    providers[1].sleep_ns.store(300'000'000);
    CHECK(apkrt_wait_scheduled(0.1) == 0);
    providers[1].sleep_ns.store(0);
    CHECK(apkrt_wait_scheduled(1.0) == 1);
}

TEST(there_are_at_most_four_providers) {
    static Provider more[3];
    CHECK(apkrt_add_scheduled_provider(nullptr, &more[0]) == -1);
    CHECK(apkrt_add_scheduled_provider(provide, &more[0]) == 0);
    CHECK(apkrt_add_scheduled_provider(provide, &more[1]) == 0);
    CHECK(apkrt_add_scheduled_provider(provide, &more[2]) == -1);
    CHECK(apkrt_wait_scheduled(1.0) == 1);
    CHECK(more[0].calls.load() == 1 && more[1].calls.load() == 1 && more[2].calls.load() == 0);
}

TEST(scheduled_without_a_time_limit_gives_a_deadline_far_ahead) {
    const auto before = uptime_ns();
    CHECK(apkrt_wait_scheduled(INFINITY) == 1);
    CHECK(providers[0].deadline.load() > before + 3'600'000'000'000);  // more than an hour: nothing wrapped
}

}  // namespace

int main() {
    // The test's own thread is the program's first thread: the process lives as long as it does.
    static apkrt::threads::GuestThread first{};
    first.tid = ::getpid();
    apkrt::threads::add(first);
    // The runtime signal's handler, which the waits of host threads need before any clone installs it.
    apkrt::signals::install();
    install_handlers();
    return apk::testing::run_all();
}
