#include "stall_watchdog.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <thread>
#include <vector>

#include <dlfcn.h>
#include <mach/mach.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>

namespace apkrt::stall {
namespace {

// A "stopped presenting" stall counts only for a continuous renderer -- >= `continuous_frames` presents in the
// `continuous_window_ns` before the last one -- so a surface that drew a few frames and went idle (a paused game, a
// static screen) never triggers a dump. "No first frame" needs no such gate.
constexpr std::size_t continuous_frames = 30;
constexpr std::uint64_t continuous_window_ns = 2'000'000'000ull;  // 2 s

// One watched app surface: a SurfaceView's surface or a NativeActivity's native window.
struct Surface {
    std::uint64_t id;
    std::uint64_t created_ns;
    std::uint64_t last_present_ns = 0;                 // 0: no frame presented on it yet
    std::uint64_t present_times[continuous_frames] = {};  // ring of the last `continuous_frames` present times
    std::uint64_t present_count = 0;
};

std::mutex lock;
std::vector<Surface> surfaces;
bool is_paused = false;
bool has_dumped = false;
std::uint64_t timeout_ns = 10'000'000'000ull;  // 10 s

}  // namespace

void resumed(std::uint64_t /*now_ns*/) {
    const std::lock_guard<std::mutex> hold(lock);
    is_paused = false;  // the surfaces carry their own creation times; a resume only lifts the pause gate
}

void paused() {
    const std::lock_guard<std::mutex> hold(lock);
    is_paused = true;
}

void surface_created(std::uint64_t id, std::uint64_t now_ns) {
    const std::lock_guard<std::mutex> hold(lock);
    for (const Surface &surface : surfaces) {
        if (surface.id == id) return;  // already watched
    }
    surfaces.push_back(Surface{id, now_ns});
}

void presented(std::uint64_t id, std::uint64_t now_ns) {
    const std::lock_guard<std::mutex> hold(lock);
    for (Surface &surface : surfaces) {
        if (surface.id != id) continue;
        surface.last_present_ns = now_ns;
        surface.present_times[surface.present_count % continuous_frames] = now_ns;
        ++surface.present_count;
        return;
    }
    // `id` is not a watched app surface (the decor, a dialog): not a surface's first frame, so it is ignored.
}

void surface_removed(std::uint64_t id) {
    const std::lock_guard<std::mutex> hold(lock);
    for (auto it = surfaces.begin(); it != surfaces.end(); ++it) {
        if (it->id == id) {
            surfaces.erase(it);
            return;
        }
    }
}

std::string tick(std::uint64_t now_ns) {
    const std::lock_guard<std::mutex> hold(lock);
    if (is_paused || has_dumped) return {};
    for (const Surface &surface : surfaces) {
        if (surface.last_present_ns == 0) {
            // No first frame: measured from the surface's creation, whatever any other window has drawn.
            if (now_ns > surface.created_ns && now_ns - surface.created_ns > timeout_ns) {
                has_dumped = true;
                return "no first frame " + std::to_string((now_ns - surface.created_ns) / 1'000'000'000ull) +
                       "s after the app surface was created";
            }
            continue;
        }
        // Stopped presenting: fire only for a continuous renderer. A surface with fewer than `continuous_frames`
        // presents in the window before its last one is a quiet screen, not a stall -- do not fire or latch (it may
        // become a continuous renderer and stall later). present_times[present_count % continuous_frames] is the
        // oldest of the last `continuous_frames` presents once that many have happened.
        if (surface.present_count < continuous_frames ||
            surface.last_present_ns - surface.present_times[surface.present_count % continuous_frames] >
                continuous_window_ns) {
            continue;
        }
        if (now_ns > surface.last_present_ns && now_ns - surface.last_present_ns > timeout_ns) {
            has_dumped = true;
            return "stopped presenting for " +
                   std::to_string((now_ns - surface.last_present_ns) / 1'000'000'000ull) + "s";
        }
    }
    return {};
}

void set_timeout_ns(std::uint64_t ns) {
    const std::lock_guard<std::mutex> hold(lock);
    timeout_ns = ns;
}

void reset_for_test() {
    const std::lock_guard<std::mutex> hold(lock);
    surfaces.clear();
    is_paused = false;
    has_dumped = false;
    timeout_ns = 10'000'000'000ull;
}

namespace {

std::atomic<bool> thread_started{false};

// Appends `text` to a bounded buffer and keeps it NUL-terminated; never writes past `cap`.
void add(char *buf, std::size_t cap, std::size_t &at, const char *text) {
    while (*text != '\0' && at + 1 < cap) buf[at++] = *text++;
    buf[at] = '\0';
}

// Appends the symbol for a code address, "name+0xoff", else "0xpc (lib)", else "0xpc" -- as process_exit's
// exit_callers line does, through dladdr and the loader's Dl_info.
void add_symbol(char *buf, std::size_t cap, std::size_t &at, std::uint64_t pc) {
    char piece[256];
    Dl_info info{};
    if (pc != 0 && ::dladdr(reinterpret_cast<const void *>(pc), &info) != 0 && info.dli_sname != nullptr) {
        const auto base = reinterpret_cast<std::uint64_t>(info.dli_saddr);
        std::snprintf(piece, sizeof piece, "%s+0x%llx", info.dli_sname, static_cast<unsigned long long>(pc - base));
    } else if (info.dli_fname != nullptr) {
        const char *slash = std::strrchr(info.dli_fname, '/');
        std::snprintf(piece, sizeof piece, "0x%llx (%s)", static_cast<unsigned long long>(pc),
                      slash != nullptr ? slash + 1 : info.dli_fname);
    } else {
        std::snprintf(piece, sizeof piece, "0x%llx", static_cast<unsigned long long>(pc));
    }
    add(buf, cap, at, piece);
}

// One thread's line: its pc, then its frame-record chain, each return address symbolized. Reads are fault-guarded
// (vm_read_overwrite), so a chain into an unreadable page just ends. The thread is not suspended: a hung thread (the
// case we dump for) has a still stack; a live one may give a short or odd chain, never a fault.
void dump_thread(int index, std::uint64_t pc, std::uint64_t fp) {
    char line[2048];
    std::size_t at = 0;
    char head[64];
    std::snprintf(head, sizeof head, "E/apkrt   : thread %d: ", index);
    add(line, sizeof line, at, head);
    add_symbol(line, sizeof line, at, pc);
    std::uint64_t frame = fp;
    for (int depth = 0; depth < 32 && frame != 0 && frame % 8 == 0; ++depth) {
        std::uint64_t record[2] = {0, 0};  // {caller's frame, return address}
        vm_size_t read = 0;
        if (::vm_read_overwrite(::mach_task_self(), static_cast<vm_address_t>(frame), sizeof record,
                                reinterpret_cast<vm_address_t>(record), &read) != KERN_SUCCESS ||
            read != sizeof record || record[1] == 0) {
            break;
        }
        add(line, sizeof line, at, " <- ");
        add_symbol(line, sizeof line, at, record[1] - 1);  // the call instruction, before its return address
        frame = record[0];
    }
    add(line, sizeof line, at, "\n");
    static_cast<void>(::write(STDERR_FILENO, line, at));
}

void write_line(const char *text) { static_cast<void>(::write(STDERR_FILENO, text, std::strlen(text))); }

// A catcher handles SIGQUIT only in an ART process (its signal catcher); a bare-bionic native app -- or a unit test --
// leaves it SIG_DFL, where raising it would terminate. So raise it only when it is handled; the native dump above
// already covers every thread regardless.
bool sigquit_is_handled() {
    struct sigaction current {};
    return ::sigaction(SIGQUIT, nullptr, &current) == 0 && current.sa_handler != SIG_DFL &&
           current.sa_handler != SIG_IGN;
}

// Dumps every thread of the process (native ones too), each symbolized, then, when SIGQUIT is handled, raises it so
// ART adds the managed stacks -- the ANR watchdog's path. Once per session (tick latches).
void dump_all_threads(const std::string &reason) {
    const std::string head = "E/apkrt   : stall: " + reason + " -- dumping all threads\n";
    write_line(head.c_str());
    thread_act_array_t threads = nullptr;
    mach_msg_type_number_t count = 0;
    if (::task_threads(::mach_task_self(), &threads, &count) != KERN_SUCCESS) return;
    for (mach_msg_type_number_t i = 0; i < count; ++i) {
        arm_thread_state64_t state{};
        mach_msg_type_number_t words = ARM_THREAD_STATE64_COUNT;
        if (::thread_get_state(threads[i], ARM_THREAD_STATE64, reinterpret_cast<thread_state_t>(&state), &words) ==
            KERN_SUCCESS) {
            dump_thread(static_cast<int>(i), __darwin_arm_thread_state64_get_pc(state),
                        __darwin_arm_thread_state64_get_fp(state));
        }
        ::mach_port_deallocate(::mach_task_self(), threads[i]);
    }
    ::vm_deallocate(::mach_task_self(), reinterpret_cast<vm_address_t>(threads),
                    count * sizeof(thread_act_t));
    if (sigquit_is_handled()) ::kill(::getpid(), SIGQUIT);
}

}  // namespace

void start_thread() {
    if (thread_started.exchange(true)) return;
    std::thread([] {
        // A runtime helper thread must not catch a signal meant for a guest thread -- above all the SIGQUIT the ANR
        // watchdog sends to ART's signal catcher, which this would otherwise steal. Block them all here.
        sigset_t all;
        sigfillset(&all);
        ::pthread_sigmask(SIG_SETMASK, &all, nullptr);
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            const std::string reason = tick(clock_gettime_nsec_np(CLOCK_MONOTONIC));
            if (!reason.empty()) dump_all_threads(reason);
        }
    }).detach();
}

}  // namespace apkrt::stall
