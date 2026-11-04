// The window registry (ADR-0028, decisions 6 and 7): the host's windows, their gates, the background state,
// "scheduled" and the windows' buffers for drawing by the CPU (ADR-0043, decision 3). The host changes them under
// `lock` and then notifies every thread that waits at a gate, whose wait (ADR-0026) reads its signal word before
// it looks at the registry: so no change slips in between.
#include "windows.h"

#include "APKRuntime/apkrt.h"
#include "process_exit.h"
#include "signals.h"
#include "stall_watchdog.h"
#include "syscalls.h"

#include <CoreFoundation/CoreFoundation.h>
#include <time.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <new>
#include <vector>

namespace apkrt::windows {
namespace {

namespace linux_errno = sys::linux_errno;

constexpr int provider_limit = 4;

struct Window {
    std::uint64_t id;
    void *layer;  // retained, or NULL (the tests on the Mac)
    std::int32_t width, height;
    bool open;  // the gate; a removed window is not here
    std::uint64_t presents;
    std::uint32_t frame_rate;  // a float's bits, 0 for no preference (ANativeWindow_setFrameRate)
};

struct Provider {
    apkrt_scheduled_provider function;
    void *context;
};

std::mutex lock;
std::vector<Window> windows;
std::uint64_t next_id = 1;
std::vector<signals::ThreadSignals *> waiters;  // the threads at a gate
std::condition_variable foreground;            // for apkrt_wait_foreground, with `lock`
Provider providers[provider_limit];
int provider_count = 0;

std::atomic<bool> in_background{false};  // sequentially consistent, for the count-then-flag protocol

apkrt_window_buffers buffers{};  // the windows' buffers (ADR-0043, decision 3); lock is NULL without them

Window *find(std::uint64_t id) {
    const auto found = std::find_if(windows.begin(), windows.end(), [id](const Window &window) { return window.id == id; });
    return found != windows.end() ? &*found : nullptr;
}

/// Ends the round of every wait at a gate, under `lock`. The gate sleeps only on the signal word, so the
/// thread's kqueue is left alone, whose next wait would take a round in vain.
void notify_waiters() {
    for (signals::ThreadSignals *state : waiters) signals::raise_signal_word(*state);
}

/// The gate's answer now, under `lock`: 0, 1 or (present_try) 2, or -1 to wait.
long answer(std::uint64_t id, std::uint64_t kind) {
    const Window *window = id != 0 ? find(id) : nullptr;
    if (id != 0 && window == nullptr) return 1;  // gone, or never given
    if (kind == APKRT_GATE_PRESENT) return window->open ? 0 : -1;
    if (kind == APKRT_GATE_PRESENT_TRY) return window->open ? 0 : 2;
    return in_background.load() ? -1 : 0;
}

}  // namespace

long gate(std::uint64_t id, std::uint64_t kind) {
    if (kind != APKRT_GATE_PRESENT && kind != APKRT_GATE_BACKGROUND && kind != APKRT_GATE_PRESENT_TRY) {
        return -linux_errno::einval;
    }
    if (kind != APKRT_GATE_BACKGROUND && id == 0) return -linux_errno::einval;
    auto &state = signals::current();
    long result;
    {
        const std::lock_guard<std::mutex> hold(lock);
        result = answer(id, kind);
        if (result >= 0) return result;
        waiters.push_back(&state);
    }
    const auto end = sys::wait_until(
        state,
        [&] {
            const std::lock_guard<std::mutex> hold(lock);
            result = answer(id, kind);
            return result >= 0;
        },
        sys::no_deadline);
    {
        const std::lock_guard<std::mutex> hold(lock);
        waiters.erase(std::find(waiters.begin(), waiters.end(), &state));
    }
    // A signal ends the wait; with SA_RESTART the syscall's exit makes the call again.
    return end == sys::WaitEnd::done ? result : -linux_errno::eintr;
}

long size(std::uint64_t id, std::uint64_t address) {
    if (address == 0) return -linux_errno::efault;
    std::int32_t width, height;
    {
        const std::lock_guard<std::mutex> hold(lock);
        const Window *window = find(id);
        if (window == nullptr) return -linux_errno::einval;
        width = window->width;
        height = window->height;
    }
    auto *const size = reinterpret_cast<std::int32_t *>(address);
    size[0] = width;
    size[1] = height;
    return 0;
}

long lock_buffer(std::uint64_t id, std::uint64_t address) {
    apkrt_window_buffers with;
    {
        const std::lock_guard<std::mutex> hold(lock);
        with = buffers;
        if (with.lock == nullptr) return -linux_errno::enosys;
        if (address == 0) return -linux_errno::efault;
        if (id == 0 || id >= next_id) return -linux_errno::einval;
        if (find(id) == nullptr) return -linux_errno::enodev;
    }
    return with.lock(with.context, id, reinterpret_cast<apkrt_window_buffer *>(address));
}

long post_buffer(std::uint64_t id) {
    apkrt_window_buffers with;
    {
        const std::lock_guard<std::mutex> hold(lock);
        with = buffers;
        if (with.post == nullptr) return -linux_errno::enosys;
        if (id == 0 || id >= next_id) return -linux_errno::einval;
    }
    return with.post(with.context, id);
}

void set_frame_rate(std::uint64_t id, std::uint32_t bits, void (*report)(std::uint32_t bits)) {
    const std::lock_guard<std::mutex> hold(lock);
    Window *window = find(id);
    if (window == nullptr || window->frame_rate == bits) return;
    window->frame_rate = bits;
    report(bits);
}

void trim_buffers() {
    apkrt_window_buffers with;
    {
        const std::lock_guard<std::mutex> hold(lock);
        with = buffers;
    }
    if (with.trim != nullptr) with.trim(with.context);
}

}  // namespace apkrt::windows

using namespace apkrt::windows;

extern "C" {

uint64_t apkrt_window_add(void *layer, int32_t width, int32_t height) {
    const std::lock_guard<std::mutex> hold(lock);
    try {
        windows.push_back({next_id, layer, width, height, false, 0, 0});
    } catch (const std::bad_alloc &) {
        return 0;
    }
    if (layer != nullptr) CFRetain(layer);
    return next_id++;
}

int apkrt_window_set_gate(uint64_t id, int open) {
    const std::lock_guard<std::mutex> hold(lock);
    Window *window = find(id);
    if (window == nullptr) return -1;
    window->open = open != 0;
    notify_waiters();
    return 0;
}

int apkrt_window_set_size(uint64_t id, int32_t width, int32_t height) {
    if (width <= 0 || height <= 0) return -1;
    const std::lock_guard<std::mutex> hold(lock);
    Window *window = find(id);
    if (window == nullptr) return -1;
    window->width = width;
    window->height = height;
    return 0;
}

int apkrt_window_remove(uint64_t id) {
    void *layer;
    apkrt_window_buffers with;
    {
        const std::lock_guard<std::mutex> hold(lock);
        Window *window = find(id);
        if (window == nullptr) return -1;
        layer = window->layer;
        windows.erase(windows.begin() + (window - windows.data()));
        notify_waiters();
        with = buffers;
    }
    if (with.removed != nullptr) with.removed(with.context, id);
    if (layer != nullptr) CFRelease(layer);
    return 0;
}

uint64_t apkrt_window_presents(uint64_t id) {
    const std::lock_guard<std::mutex> hold(lock);
    const Window *window = find(id);
    return window != nullptr ? window->presents : 0;
}

void apkrt_set_background(int on) {
    in_background.store(on != 0);
    const std::lock_guard<std::mutex> hold(lock);
    notify_waiters();
    if (on == 0) foreground.notify_all();
}

int apkrt_wait_scheduled(double seconds) {
    // None below 0 or for NaN, at most 10^9 s, so that the deadline cannot wrap.
    const double limited = seconds > 0 ? std::min(seconds, 1e9) : 0;
    const auto deadline = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + static_cast<std::uint64_t>(limited * 1e9);
    Provider asked[provider_limit];
    int count;
    {
        const std::lock_guard<std::mutex> hold(lock);
        count = provider_count;
        std::copy(providers, providers + count, asked);
    }
    bool in_time = true;
    for (int index = 0; index < count; ++index) {
        const bool scheduled = asked[index].function(asked[index].context, deadline) != 0;
        in_time = in_time && scheduled && clock_gettime_nsec_np(CLOCK_UPTIME_RAW) <= deadline;
    }
    return in_time ? 1 : 0;
}

void *apkrt_window_layer(uint64_t id) {
    const std::lock_guard<std::mutex> hold(lock);
    const Window *window = find(id);
    if (window == nullptr || window->layer == nullptr) return nullptr;
    CFRetain(window->layer);
    return window->layer;
}

int apkrt_window_size(uint64_t id, int32_t *width, int32_t *height) {
    const std::lock_guard<std::mutex> hold(lock);
    const Window *window = find(id);
    if (window == nullptr) return -1;
    if (width != nullptr) *width = window->width;
    if (height != nullptr) *height = window->height;
    return 0;
}

void apkrt_window_presented(uint64_t id) {
    apkrt::stall::presented(clock_gettime_nsec_np(CLOCK_MONOTONIC));  // the stall watchdog's last-present time
    const std::lock_guard<std::mutex> hold(lock);
    if (Window *window = find(id)) ++window->presents;
}

int apkrt_is_background(void) {
    return in_background.load() ? 1 : 0;
}

void apkrt_wait_foreground(void) {
    std::unique_lock<std::mutex> hold(lock);
    foreground.wait(hold, [] { return !in_background.load(); });
}

int apkrt_set_window_buffers(const apkrt_window_buffers *set) {
    if (set != nullptr && (set->lock == nullptr || set->post == nullptr || set->removed == nullptr)) return -1;
    const std::lock_guard<std::mutex> hold(lock);
    buffers = set != nullptr ? *set : apkrt_window_buffers{};
    return 0;
}

int apkrt_add_scheduled_provider(apkrt_scheduled_provider provider, void *context) {
    if (provider == nullptr) return -1;
    const std::lock_guard<std::mutex> hold(lock);
    if (provider_count == provider_limit) return -1;
    providers[provider_count++] = {provider, context};
    return 0;
}

}  // extern "C"
