// Guest signals (M2 step 5c, ARCHITECTURE.md 4.5, R9). The runtime keeps the Linux signal state of
// every guest thread itself: dispositions, masks, pending and queued signals, alternate stacks.
// Host signals serve three purposes only:
//   - SIGURG, the runtime signal: a thread sends it to itself to resume guest code in any register
//     state (clone, rt_sigreturn, delivery at syscall exit), and to other threads to make them look
//     at their pending guest signals (tgkill), interrupting a blocking host call on the way;
//   - hardware faults in guest code (SIGSEGV, SIGBUS, SIGILL, SIGTRAP, SIGFPE), translated into the
//     Linux signal, code and address;
//   - dying by a signal whose default action ends the process.
// A guest handler runs the way the Linux kernel runs it: with an rt_sigframe (siginfo, ucontext with
// all registers, FPSIMD and ESR records) on the guest's stack or alternate stack, and returns
// through its restorer and rt_sigreturn, which restores whatever the handler left in the ucontext.
#pragma once

#include "guest_context.h"
#include "syscall_frame.h"

#include <pthread.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace apkrt::signals {

constexpr int signal_count = 64;

constexpr std::uint64_t bit(int signal) {
    return std::uint64_t{1} << (signal - 1);
}

/// siginfo_t as Linux fills it on arm64 (include/uapi/asm-generic/siginfo.h).
struct LinuxSiginfo {
    std::int32_t signo;
    std::int32_t error;
    std::int32_t code;
    std::int32_t padding;
    union {
        struct {
            std::int32_t pid;  // kill, tgkill, sigqueue
            std::uint32_t uid;
            std::uint64_t value;  // sigqueue's si_value
        } sender;
        std::uint64_t address;  // faults
        std::uint8_t raw[112];
    } fields;
};
static_assert(sizeof(LinuxSiginfo) == 128);

/// struct sigaction as the Linux kernel takes it on arm64 (include/linux/signal_types.h).
struct LinuxSigaction {
    std::uint64_t handler;
    std::uint64_t flags;
    std::uint64_t restorer;
    std::uint64_t mask;
};

/// stack_t as Linux lays it out on arm64: size after flags, Darwin has it the other way round.
struct LinuxStack {
    std::uint64_t sp;
    std::int32_t flags;
    std::int32_t padding;
    std::uint64_t size;
};

// si_code values (include/uapi/asm-generic/siginfo.h).
constexpr std::int32_t si_user = 0;
constexpr std::int32_t si_queue = -1;
constexpr std::int32_t si_tkill = -6;
// stack_t flags.
constexpr std::int32_t ss_onstack = 1;
constexpr std::int32_t ss_disable = 2;
constexpr std::int32_t ss_autodisarm = std::int32_t(1u << 31);

/// The signal state of one guest thread. Its own host signal handler reads and changes it; other
/// threads only add signals, under `lock`.
struct ThreadSignals {
    std::atomic<std::uint64_t> mask{0};
    std::atomic<std::uint64_t> pending{0};  // signals waiting, standard and real-time
    // Whether the thread runs the runtime's code rather than guest code: signals wait until it goes
    // back to guest code. It changes exactly where the code changes sides (ADR-0025): the syscall
    // entry sets it, the syscall exit and the resume of a context clear it. A new thread starts in
    // the runtime.
    std::atomic<bool> in_runtime{true};
    LinuxSiginfo info[signal_count + 1] = {};  // what came with each pending standard signal
    struct Queued {
        int signal;
        LinuxSiginfo info;
    };
    static constexpr int queue_capacity = 64;  // real-time signals queue, in order
    Queued queue[queue_capacity] = {};
    int queued = 0;
    std::atomic_flag lock = ATOMIC_FLAG_INIT;
    LinuxStack alternate_stack{0, ss_disable, 0, 0};
    // Raised whenever a signal is sent to the thread or a futex wake takes it: every blocking syscall
    // of the thread that does not wait on descriptors sleeps on it (ADR-0026, syscalls.h wait_until).
    std::atomic<std::uint32_t> wake{0};
    // The thread's kqueue for its waits on descriptors (ADR-0027, decision 6; syscalls.h
    // wait_for_descriptors), made at its first such wait, with a user event that notify() triggers
    // along with the signal word; -1 before. Closed with the state, once the thread has ended.
    std::atomic<int> wait_queue{-1};
    // sigsuspend: the mask the handler's frame returns to, instead of the temporary one.
    bool frame_mask_set = false;
    std::uint64_t frame_mask = 0;
    pthread_t host{};

    ThreadSignals() = default;
    ~ThreadSignals();
    ThreadSignals(const ThreadSignals &) = delete;
    ThreadSignals &operator=(const ThreadSignals &) = delete;
};
static_assert(offsetof(ThreadSignals, mask) == 0 && offsetof(ThreadSignals, pending) == 8 &&
                  offsetof(ThreadSignals, in_runtime) == 16 && sizeof(std::atomic<bool>) == 1,
              "layout used by syscall_entry.S");

/// The signal state of the calling thread: its guest thread's, or a state of its own for a host
/// thread that makes guest syscalls (tests).
ThreadSignals &current();

/// Installs the runtime's host signal handlers, once for the process; before guest code runs.
void install();

/// The address signal handlers without SA_RESTORER return to (libc.so's __apkrt_sigreturn).
void set_default_restorer(std::uint64_t address);

/// Continues the calling guest thread in `context`, and first delivers the guest signals it can
/// take, as the Linux kernel does on its way back to user space. The thread stays in the runtime
/// until the runtime signal's handler switches to the context. Never returns.
[[noreturn]] void resume(const GuestContext &context);

/// Starts guest code at `entry` the way Linux starts an executable: the stack pointer at
/// `stack_pointer`, every other register zero, and the thread in guest code from the first word, with
/// what became pending while the runtime worked delivered first (ADR-0025). Never returns.
[[noreturn]] void enter_guest(std::uint64_t entry, std::uint64_t stack_pointer);

/// The disposition of a signal, and setting it (sys_signals.cpp checks the arguments).
LinuxSigaction action(int signal);
void set_action(int signal, const LinuxSigaction &action);

/// Sends a signal to the guest thread `tid`: -ESRCH when there is none. Signal 0 only checks.
long send_to_thread(std::int32_t tid, int signal, const LinuxSiginfo &info);

/// Sends a signal to the process: to the calling thread unless it blocks it, else to a thread
/// that does not.
long send_to_process(int signal, const LinuxSiginfo &info);

/// Raises the thread's signal word and triggers the user event in its kqueue: a blocking syscall of
/// the thread wakes up, whether it sleeps on the word or in the kqueue.
void notify(ThreadSignals &state);

/// Raises only the thread's signal word, for a futex wake: it ends a futex wait, which sleeps on the
/// word, and leaves the kqueue alone.
void raise_signal_word(ThreadSignals &state);

/// The calling thread's kqueue for its waits on descriptors (ThreadSignals::wait_queue), made at the
/// first call; -1 when the host cannot make one.
int wait_queue_of(ThreadSignals &state);

/// Takes the lowest pending signal among `allowed` (a set of signals), for sigtimedwait; false
/// when there is none.
bool take(ThreadSignals &state, std::uint64_t allowed, int &signal, LinuxSiginfo &info);

/// The pending signals of the calling thread.
std::uint64_t pending(const ThreadSignals &state);

/// At the end of every syscall: the state of the calling guest thread, whose in_runtime the exit of
/// syscall_entry.S clears before it looks for signals to deliver (ADR-0025); null for a host thread.
ThreadSignals *at_syscall_exit();

/// The exit's slow path, when it found a deliverable signal, with the frame as the syscall leaves it
/// (x[0] the result, x[31] its first argument, which the dispatcher keeps there): delivers the
/// signals, restarting the syscall after the handler when it was interrupted and the handler has
/// SA_RESTART.
[[noreturn]] void deliver_at_syscall_exit(const apkrt_syscall_frame &frame);

/// Where a runtime signal from another thread found a guest thread, by its pc and in_runtime
/// (ADR-0025).
enum class Stopped {
    in_runtime,     // runtime code, or the syscall entry before it set in_runtime: signals wait
    in_guest_code,  // the signal context is the guest's
    returning,      // the syscall exit after it cleared in_runtime: counts as returned
    leaving_loader,  // a loader stub after its call returned: counts as back at the caller
};
Stopped where_stopped(std::uint64_t pc, bool in_runtime);

/// For a thread stopped `returning`: the guest's state after the syscall, as if the exit had run to
/// its end, with the slow path's SA_RESTART rule for the `deliverable` signals.
GuestContext after_syscall_exit(const ucontext_t &uc, std::uint64_t deliverable);

/// For a thread stopped `leaving_loader`: the caller's state after its call, as if the stub had run
/// to its end.
GuestContext after_loader_call(const ucontext_t &uc);

/// A signal frame's ucontext for rt_sigreturn: restores the mask, the alternate stack and the
/// guest's x18 (Darwin TSD slot 11, ADR-0020) of the calling thread and returns the registers to
/// resume. False when `address` holds no frame the syscall layer can read.
bool read_frame(std::uint64_t address, GuestContext &context);

}  // namespace apkrt::signals

/// Called by apkrt_call_guest in syscall_entry.S on a guest thread: resumes `context`, the guest
/// function's entry.
extern "C" [[noreturn]] void apkrt_call_guest_enter(const apkrt::GuestContext *context);
