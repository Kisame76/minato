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
#include <dispatch/dispatch.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <iterator>
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
    double scale = 1;          // the render scale (ADR-0067 step 2): the guest and the low layer render at this fraction
    // The first-frame line's counts (apkrt_window_diagnostic): the guest's software locks and posts, answered and
    // failed, and the presents by apkrt_present_kind.
    std::uint64_t locks = 0, locks_failed = 0, posts = 0, posts_failed = 0;
    std::uint64_t by_kind[5] = {};
};

/// The render size of a full dimension at a scale, at least one pixel (ADR-0067 step 2).
std::int32_t scaled(std::int32_t full, double scale) {
    return std::max<std::int32_t>(1, static_cast<std::int32_t>(std::lround(full * scale)));
}

struct Provider {
    apkrt_scheduled_provider function;
    void *context;
};

std::mutex lock;
std::vector<Window> windows;
std::uint64_t next_id = 1;
std::vector<signals::ThreadSignals *> waiters;  // the threads at a gate
// Windows removed with apkrt_window_remove_untold whose app has not been told yet (apkrt_window_told): their
// present gates answer "drop", as Android's BufferQueue takes frames until the surface's destroy has returned.
std::vector<std::uint64_t> untold;
std::condition_variable foreground;            // for apkrt_wait_foreground, with `lock`
Provider providers[provider_limit];
int provider_count = 0;

std::atomic<bool> in_background{false};  // sequentially consistent, for the count-then-flag protocol

// The waits at the commit gate since the background last began (apkrt_background_gate), under `lock`.
std::uint32_t gate_parked = 0;
std::uint64_t gate_longest = 0;  // of the waits that ended, ns
std::uint32_t gate_running = 0;  // waits now
std::uint64_t gate_since = 0;    // the start of the oldest wait now

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
    if (id != 0 && window == nullptr) {
        // Removed but the app not told yet: a present is dropped (3), any other gate answers gone.
        const bool waiting = std::find(untold.begin(), untold.end(), id) != untold.end();
        return waiting && kind != APKRT_GATE_BACKGROUND ? 3 : 1;  // else gone, or never given
    }
    if (kind == APKRT_GATE_PRESENT) return window->open ? 0 : -1;
    if (kind == APKRT_GATE_PRESENT_TRY) return window->open ? 0 : 2;
    return in_background.load() ? -1 : 0;
}

/// The first-frame line of window `id` on stderr 3 s and 10 s after it came (MINATO_LOADER_LOG), or that it is gone:
/// tag "apkrt", as the trim's footprint line, so it reaches the device's session log and java_app_run's app-tag
/// comparison passes over it.
void log_first_frames(std::uint64_t id) {
    for (const int seconds : {3, 10}) {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, std::int64_t{seconds} * 1'000'000'000),
                       dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
                           char counts[320];
                           const bool there = apkrt_window_diagnostic(id, counts, sizeof counts) == 0;
                           char text[400];
                           const int length = std::snprintf(text, sizeof text, "I/apkrt   : window %llu at %d s: %s\n",
                                                            static_cast<unsigned long long>(id), seconds,
                                                            there ? counts : "removed");
                           if (length > 0) {
                               static_cast<void>(::write(STDERR_FILENO, text,
                                                         std::min<std::size_t>(static_cast<std::size_t>(length),
                                                                               sizeof text - 1)));
                           }
                       });
    }
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
        width = scaled(window->width, window->scale);
        height = scaled(window->height, window->scale);
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
    const int answer = with.lock(with.context, id, reinterpret_cast<apkrt_window_buffer *>(address));
    const std::lock_guard<std::mutex> hold(lock);
    if (Window *window = find(id)) ++(answer == 0 ? window->locks : window->locks_failed);
    return answer;
}

long post_buffer(std::uint64_t id) {
    apkrt_window_buffers with;
    {
        const std::lock_guard<std::mutex> hold(lock);
        with = buffers;
        if (with.post == nullptr) return -linux_errno::enosys;
        if (id == 0 || id >= next_id) return -linux_errno::einval;
    }
    const int answer = with.post(with.context, id);
    const std::lock_guard<std::mutex> hold(lock);
    if (Window *window = find(id)) ++(answer == 0 ? window->posts : window->posts_failed);
    return answer;
}

long readback_window(std::uint64_t id, std::uint64_t address) {
    if (address == 0) return -linux_errno::efault;
    apkrt_window_readback_request request;
    if (!sys::memory::copy_checked(reinterpret_cast<std::uint64_t>(&request), address, sizeof request)) {
        return -linux_errno::efault;
    }
    if (request.format != 1) return -linux_errno::einval;  // PixelFormat.RGBA_8888, the only one for now
    if (request.width == 0 || request.height == 0) return -linux_errno::einval;
    const std::uint64_t stride = request.stride;
    if (stride < static_cast<std::uint64_t>(request.width) * 4) return -linux_errno::einval;
    const std::uint64_t bytes = static_cast<std::uint64_t>(request.height) * stride;
    if (bytes > APKRT_HARDWARE_BUFFER_MAX_BYTES) return -linux_errno::einval;
    apkrt_window_buffers with;
    {
        const std::lock_guard<std::mutex> hold(lock);
        with = buffers;
        if (with.readback == nullptr) return -linux_errno::enodata;  // no host read path (CPU-less, e.g. GL/Vulkan)
        // Only the guest's own windows, never another app's or iOS's screen (ADR-0043 amendment): an id that is not a
        // live window answers as a window with no posted buffer does, so the guest cannot tell the two apart.
        if (id == 0 || id >= next_id || find(id) == nullptr) return -linux_errno::enodata;
    }
    std::vector<std::uint8_t> staging;
    try {
        staging.resize(bytes);
    } catch (const std::bad_alloc &) {
        return -linux_errno::enomem;
    }
    // The destination the host fills is the runtime's own staging, never guest memory; the runtime copies it out below,
    // which also vouches for the guest address. The host checks the rectangle against the window's geometry.
    const apkrt_window_readback host{request.left,   request.top,    request.right,  request.bottom,
                                     request.width,  request.height, request.stride, staging.data()};
    const int answered = with.readback(with.context, id, &host);
    if (answered != 0) return answered;
    if (!sys::memory::copy_checked(request.pixels, reinterpret_cast<std::uint64_t>(staging.data()), bytes)) {
        return -linux_errno::efault;
    }
    return 0;
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
    if (::getenv("MINATO_LOADER_LOG") != nullptr) log_first_frames(next_id);
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

namespace {

int remove_window(uint64_t id, bool tell_later) {
    void *layer;
    apkrt_window_buffers with;
    {
        const std::lock_guard<std::mutex> hold(lock);
        Window *window = find(id);
        if (window == nullptr) return -1;
        layer = window->layer;
        windows.erase(windows.begin() + (window - windows.data()));
        if (tell_later) untold.push_back(id);
        notify_waiters();
        with = buffers;
    }
    apkrt::stall::surface_removed(id);  // stop watching it if it was an app surface (ignored otherwise)
    if (with.removed != nullptr) with.removed(with.context, id);
    if (layer != nullptr) CFRelease(layer);
    return 0;
}

}  // namespace

int apkrt_window_remove(uint64_t id) {
    return remove_window(id, false);
}

int apkrt_window_remove_untold(uint64_t id) {
    return remove_window(id, true);
}

void apkrt_window_told(uint64_t id) {
    const std::lock_guard<std::mutex> hold(lock);
    const auto found = std::find(untold.begin(), untold.end(), id);
    if (found == untold.end()) return;
    untold.erase(found);
    notify_waiters();
}

uint64_t apkrt_window_presents(uint64_t id) {
    const std::lock_guard<std::mutex> hold(lock);
    const Window *window = find(id);
    return window != nullptr ? window->presents : 0;
}

void apkrt_set_background(int on) {
    in_background.store(on != 0);
    const std::lock_guard<std::mutex> hold(lock);
    if (on != 0) {
        gate_parked = 0;
        gate_longest = 0;
    }
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

int apkrt_window_render_size(uint64_t id, int32_t *width, int32_t *height) {
    const std::lock_guard<std::mutex> hold(lock);
    const Window *window = find(id);
    if (window == nullptr) return -1;
    if (width != nullptr) *width = scaled(window->width, window->scale);
    if (height != nullptr) *height = scaled(window->height, window->scale);
    return 0;
}

int apkrt_window_set_render_scale(uint64_t id, double scale) {
    if (!(scale >= 0.25 && scale <= 1.0)) return -1;  // refuses NaN too (every comparison with it is false)
    const std::lock_guard<std::mutex> hold(lock);
    Window *window = find(id);
    if (window == nullptr) return -1;
    window->scale = scale;
    return 0;
}

void apkrt_window_presented(uint64_t id) {
    apkrt_window_presented_as(id, UINT32_MAX);
}

void apkrt_window_presented_as(uint64_t id, uint32_t kind) {
    apkrt::stall::presented(id, clock_gettime_nsec_np(CLOCK_MONOTONIC));  // the stall watchdog's last frame for id
    const std::lock_guard<std::mutex> hold(lock);
    if (Window *window = find(id)) {
        ++window->presents;
        if (kind < std::size(window->by_kind)) ++window->by_kind[kind];
    }
}

int apkrt_window_diagnostic(uint64_t id, char *line, size_t size) {
    const std::lock_guard<std::mutex> hold(lock);
    const Window *window = find(id);
    if (window == nullptr) return -1;
    if (line == nullptr || size == 0) return 0;
    const auto &kind = window->by_kind;
    std::snprintf(line, size,
                  "%dx%d, gate %s, software locks %llu (failed %llu), posts %llu (failed %llu), buffers shown %llu, "
                  "hidden as transparent %llu, EGL swaps %llu, Vulkan presents %llu, video frames %llu",
                  window->width, window->height, window->open ? "open" : "closed",
                  static_cast<unsigned long long>(window->locks), static_cast<unsigned long long>(window->locks_failed),
                  static_cast<unsigned long long>(window->posts), static_cast<unsigned long long>(window->posts_failed),
                  static_cast<unsigned long long>(kind[APKRT_PRESENT_BUFFER_SHOWN]),
                  static_cast<unsigned long long>(kind[APKRT_PRESENT_BUFFER_HIDDEN]),
                  static_cast<unsigned long long>(kind[APKRT_PRESENT_EGL]),
                  static_cast<unsigned long long>(kind[APKRT_PRESENT_VULKAN]),
                  static_cast<unsigned long long>(kind[APKRT_PRESENT_VIDEO]));
    return 0;
}

void apkrt_stall_surface_created(uint64_t id) {
    // The host calls this when it hands an app surface (a SurfaceView's surface, a NativeActivity's native window) to
    // the guest: the stall watchdog then expects a first frame on `id`. The decor and other windows are not surfaces.
    apkrt::stall::surface_created(id, clock_gettime_nsec_np(CLOCK_MONOTONIC));
}

void apkrt_stall_window_created(uint64_t id) {
    apkrt::stall::window_created(id, clock_gettime_nsec_np(CLOCK_MONOTONIC));
}

int apkrt_is_background(void) {
    return in_background.load() ? 1 : 0;
}

void apkrt_background_gate(uint32_t *parked, uint64_t *longest_ns) {
    const std::lock_guard<std::mutex> hold(lock);
    const std::uint64_t running = gate_running > 0 ? clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - gate_since : 0;
    if (parked != nullptr) *parked = gate_parked;
    if (longest_ns != nullptr) *longest_ns = std::max(gate_longest, running);
}

void apkrt_wait_foreground(void) {
    std::unique_lock<std::mutex> hold(lock);
    if (!in_background.load()) return;
    const std::uint64_t start = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    ++gate_parked;
    if (gate_running++ == 0) gate_since = start;
    foreground.wait(hold, [] { return !in_background.load(); });
    gate_longest = std::max(gate_longest, clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - start);
    --gate_running;
}

// graphics/host's background deferral (ADR-0029 addendum): the alternative to dropping the oldest deferred command
// buffer when ANGLE's FIFO is full. It parks the calling (committing) GL thread at the background gate until
// foreground and returns 1, so the buffer commits on resume and its work is not lost -- but ONLY once the process
// is backgrounded AND every surfaceDestroyed has returned (untold empty). apkrt_set_background(1) is called only
// after onStop (ADR-0029 decision 15); an untold window means a destroy callback is still running and may wait on
// this very thread (SDL), so parking before it is told would hang surfaceDestroyed. In every other state it
// returns 0 at once and never parks, so neither onStop nor surfaceDestroyed can stall. The caller (ANGLE, with its
// own submit lock released first) drops on 0 and retries the commit on 1. Shares apkrt_wait_foreground's gate and
// counters and releases `lock` while it waits, so it never blocks another gate call.
int apkrt_background_commit_park(void) {
    std::unique_lock<std::mutex> hold(lock);
    if (!in_background.load() || !untold.empty()) return 0;
    const std::uint64_t start = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    ++gate_parked;
    if (gate_running++ == 0) gate_since = start;
    foreground.wait(hold, [] { return !in_background.load(); });
    gate_longest = std::max(gate_longest, clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - start);
    --gate_running;
    return 1;
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
