#include "stall_watchdog.h"

#include "APKRuntime/apkrt.h"
#include "threads.h"

#include <algorithm>
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
    std::uint64_t last_progress_ns = 0;                // when `work` last grew; 0: no growing progress reported yet
    std::uint64_t progress_work = 0;                   // the most cumulative work reported toward the first frame
    std::uint64_t last_present_ns = 0;                 // 0: no frame presented on it yet
    std::uint64_t present_times[continuous_frames] = {};  // ring of the last `continuous_frames` present times
    std::uint64_t present_count = 0;
    bool renders = true;  // a render surface; false for a Java app's window, watched for its first frame only
};

// Never destroyed: the detached watchdog daemon may touch these during static destruction at a host test exit;
// a leaked heap mutex and vector stay valid while the daemon still ticks (the device uses _exit, no static dtors).
std::mutex &lock = *new std::mutex;
std::vector<Surface> &surfaces = *new std::vector<Surface>;
bool is_paused = false;
bool has_dumped = false;
std::uint64_t timeout_ns = 10'000'000'000ull;           // 10 s: no first frame and no growing progress this long
std::uint64_t max_first_frame_ns = 60'000'000'000ull;   // 60 s: hard cap, reported even while work grows
std::uint64_t (*work_source)(std::uint64_t id) = nullptr;  // pulled per waiting surface; null: created-time deadline

}  // namespace

void resumed(std::uint64_t /*now_ns*/) {
    const std::lock_guard<std::mutex> hold(lock);
    is_paused = false;  // the surfaces carry their own creation times; a resume only lifts the pause gate
}

void paused() {
    const std::lock_guard<std::mutex> hold(lock);
    is_paused = true;
}

namespace {

void watch(std::uint64_t id, std::uint64_t now_ns, bool renders) {
    const std::lock_guard<std::mutex> hold(lock);
    for (const Surface &surface : surfaces) {
        if (surface.id == id) return;  // already watched
    }
    Surface surface{id, now_ns};
    surface.renders = renders;
    surfaces.push_back(surface);
}

}  // namespace

void surface_created(std::uint64_t id, std::uint64_t now_ns) {
    watch(id, now_ns, true);
}

void window_created(std::uint64_t id, std::uint64_t now_ns) {
    watch(id, now_ns, false);
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

void set_work_source(std::uint64_t (*source)(std::uint64_t id)) {
    const std::lock_guard<std::mutex> hold(lock);
    work_source = source;
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
    for (Surface &surface : surfaces) {
        if (surface.last_present_ns == 0) {
            // No first frame. Progress-aware with a hard cap. Pull the host's cumulative work toward this surface's
            // first frame and slide the soft deadline only when it grows: a render loop that reruns the same uploads
            // without a new result (the B1 stall) reports no growth, so its deadline does not slide and it is caught; a
            // null source, a Vulkan surface or a window with no live EGL context (the decor) reports nothing, keeping
            // the created-time deadline and the Unity 6 guard. The soft deadline is then measured from the later of the
            // surface's creation and the last time its work grew, so a slow but advancing first load (a shader warm-up,
            // asset extraction) is not taken for a stall; the hard cap bounds even a surface whose work keeps growing,
            // so a load that advances forever or far too slowly cannot dodge the watchdog.
            if (work_source != nullptr) {
                const std::uint64_t work = work_source(surface.id);
                if (work > surface.progress_work) {
                    surface.progress_work = work;
                    surface.last_progress_ns = now_ns;
                }
            }
            const std::uint64_t since = std::max(surface.created_ns, surface.last_progress_ns);
            const bool over_cap = now_ns > surface.created_ns && now_ns - surface.created_ns > max_first_frame_ns;
            const bool stalled = now_ns > since && now_ns - since > timeout_ns;
            if (over_cap || stalled) {
                has_dumped = true;
                if (over_cap) {
                    return "no first frame " + std::to_string((now_ns - surface.created_ns) / 1'000'000'000ull) +
                           "s after the app surface was created (hard cap, despite progress)";
                }
                const char *mark = surface.last_progress_ns != 0 ? "s after the app surface last grew its work"
                                                                 : "s after the app surface was created";
                return "no first frame " + std::to_string((now_ns - since) / 1'000'000'000ull) + mark;
            }
            continue;
        }
        // Stopped presenting: fire only for a render surface that is a continuous renderer; a Java app's window is
        // watched for its first frame only. A surface with fewer than `continuous_frames` presents in the window before
        // its last one is a quiet screen, not a stall -- do not fire or latch (it may become a continuous renderer and
        // stall later). present_times[present_count % continuous_frames] is the
        // oldest of the last `continuous_frames` presents once that many have happened.
        if (!surface.renders || surface.present_count < continuous_frames ||
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

void set_max_first_frame_ns(std::uint64_t ns) {
    const std::lock_guard<std::mutex> hold(lock);
    max_first_frame_ns = ns;
}

void reset_for_test() {
    const std::lock_guard<std::mutex> hold(lock);
    surfaces.clear();
    is_paused = false;
    has_dumped = false;
    timeout_ns = 10'000'000'000ull;
    max_first_frame_ns = 60'000'000'000ull;
    work_source = nullptr;
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

// One thread's line: its index, name tag, run state, pc and frame-record chain, each return address symbolized. The
// tag is the thread's name -- the guest and the framework name their threads (prctl(PR_SET_NAME), which the syscall
// layer maps to pthread_setname_np), as do Minato's own helper threads -- so the hung thread is named (the render
// thread, the periodic audio thread), not only numbered; a nameless thread keeps the bare index. Reads are
// fault-guarded (vm_read_overwrite), so a chain into an unreadable page just ends. The thread is not suspended: a hung
// thread (the case we dump for) has a still stack; a live one may give a short or odd chain, never a fault.
void dump_thread(int index, const char *name, const char *state, std::uint64_t pc, std::uint64_t fp) {
    char line[2048];
    std::size_t at = 0;
    char head[128];
    if (name != nullptr && name[0] != '\0') {
        std::snprintf(head, sizeof head, "E/apkrt   : thread %d [%s] %s: ", index, name, state);
    } else {
        std::snprintf(head, sizeof head, "E/apkrt   : thread %d %s: ", index, state);
    }
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
    // Name the gate the threads wait at (the background commit gate, apkrt_wait_foreground), so the report reads as a
    // report and not only as a stack of stacks: the reason, whether the process is backgrounded, and how many threads
    // and for how long are parked at the gate. The per-thread lines below add each thread's name and run state.
    std::uint32_t parked = 0;
    std::uint64_t longest_ns = 0;
    apkrt_background_gate(&parked, &longest_ns);
    char head[256];
    std::snprintf(head, sizeof head,
                  "E/apkrt   : stall: %s; background %s, %u parked at the background commit gate (longest %llu ms) "
                  "-- dumping all threads\n",
                  reason.c_str(), apkrt_is_background() != 0 ? "yes" : "no", parked,
                  static_cast<unsigned long long>(longest_ns / 1'000'000));
    write_line(head);
    thread_act_array_t threads = nullptr;
    mach_msg_type_number_t count = 0;
    if (::task_threads(::mach_task_self(), &threads, &count) != KERN_SUCCESS) return;
    // The app's threads first (the guest's, which run guest code on a host thread of the runtime's), then Minato's own
    // under a line of their own (the session log's tap, the display link, audio, the watchdogs), so a report reads
    // the app's threads without Minato's helpers mixed in.
    std::vector<pthread_t> guest;
    threads::for_each([&guest](threads::GuestThread &thread) {
        if (const pthread_t host = thread.signals.host.load(); host != pthread_t{}) guest.push_back(host);
    });
    for (const bool of_guest : {true, false}) {
        if (!of_guest) write_line("E/apkrt   : stall: Minato's own threads, not the app's:\n");
        for (mach_msg_type_number_t i = 0; i < count; ++i) {
            const pthread_t pt = ::pthread_from_mach_thread_np(threads[i]);
            const bool is_guest = pt != nullptr && std::find(guest.begin(), guest.end(), pt) != guest.end();
            if (is_guest != of_guest) continue;
            arm_thread_state64_t state{};
            mach_msg_type_number_t words = ARM_THREAD_STATE64_COUNT;
            if (::thread_get_state(threads[i], ARM_THREAD_STATE64, reinterpret_cast<thread_state_t>(&state),
                                   &words) != KERN_SUCCESS) {
                continue;
            }
            // The thread's name, via its pthread: prctl(PR_SET_NAME) from the guest lands here as
            // pthread_setname_np, and Minato names its own threads the same way. A thread with no name is bare.
            char name[64] = {};
            if (pt != nullptr) ::pthread_getname_np(pt, name, sizeof name);
            // The thread's run state (waiting at a gate, running, ...), so the report names where it is, not only its
            // stack: a hung thread the dump fires for is "waiting".
            thread_basic_info_data_t basic{};
            mach_msg_type_number_t basic_count = THREAD_BASIC_INFO_COUNT;
            const char *run = "?";
            if (::thread_info(threads[i], THREAD_BASIC_INFO, reinterpret_cast<thread_info_t>(&basic),
                              &basic_count) == KERN_SUCCESS) {
                switch (basic.run_state) {
                case TH_STATE_RUNNING: run = "running"; break;
                case TH_STATE_STOPPED: run = "stopped"; break;
                case TH_STATE_WAITING: run = "waiting"; break;
                case TH_STATE_UNINTERRUPTIBLE: run = "uninterruptible"; break;
                case TH_STATE_HALTED: run = "halted"; break;
                }
            }
            dump_thread(static_cast<int>(i), name, run, __darwin_arm_thread_state64_get_pc(state),
                        __darwin_arm_thread_state64_get_fp(state));
        }
    }
    for (mach_msg_type_number_t i = 0; i < count; ++i) ::mach_port_deallocate(::mach_task_self(), threads[i]);
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

void dump_threads_now(const std::string &reason) { dump_all_threads(reason); }

}  // namespace apkrt::stall
