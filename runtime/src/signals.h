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
#include <cstdint>

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
    // Whether the thread runs the runtime's code rather than guest code: signals wait until it
    // goes back to guest code. A new thread starts in the runtime.
    std::atomic<bool> in_runtime{true};
    // Raised whenever a signal is sent to the thread, for sigsuspend and sigtimedwait to wait on.
    std::atomic<std::uint32_t> wake{0};
    // sigsuspend: the mask the handler's frame returns to, instead of the temporary one.
    bool frame_mask_set = false;
    std::uint64_t frame_mask = 0;
    pthread_t host{};
};

/// The signal state of the calling thread: its guest thread's, or a state of its own for a host
/// thread that makes guest syscalls (tests).
ThreadSignals &current();

/// Installs the runtime's host signal handlers, once for the process; before guest code runs.
void install();

/// The address signal handlers without SA_RESTORER return to (libc.so's __apkrt_sigreturn).
void set_default_restorer(std::uint64_t address);

/// Continues the calling guest thread in `context`, and first delivers the guest signals it can
/// take, as the Linux kernel does on its way back to user space. Never returns.
[[noreturn]] void resume(const GuestContext &context);

/// The disposition of a signal, and setting it (sys_signals.cpp checks the arguments).
LinuxSigaction action(int signal);
void set_action(int signal, const LinuxSigaction &action);

/// Sends a signal to the guest thread `tid`: -ESRCH when there is none. Signal 0 only checks.
long send_to_thread(std::int32_t tid, int signal, const LinuxSiginfo &info);

/// Sends a signal to the process: to the calling thread unless it blocks it, else to a thread
/// that does not.
long send_to_process(int signal, const LinuxSiginfo &info);

/// Wakes a thread waiting for signals (sigsuspend, sigtimedwait) after its state changed.
void notify(ThreadSignals &state);

/// Takes the lowest pending signal among `allowed` (a set of signals), for sigtimedwait; false
/// when there is none.
bool take(ThreadSignals &state, std::uint64_t allowed, int &signal, LinuxSiginfo &info);

/// The pending signals of the calling thread.
std::uint64_t pending(const ThreadSignals &state);

/// At the end of every syscall of a guest thread, called with the frame as the syscall leaves it
/// (x[0] the result): delivers the signals that became deliverable, restarting the syscall after
/// the handler when it was interrupted and the handler has SA_RESTART. Returns when there is
/// nothing to deliver.
void at_syscall_exit(const apkrt_syscall_frame &frame, std::uint64_t first_argument, std::uint64_t number);

/// A signal frame's ucontext for rt_sigreturn: restores the mask and the alternate stack of the
/// calling thread and returns the registers to resume. False when `address` holds no frame the
/// syscall layer can read.
bool read_frame(std::uint64_t address, GuestContext &context);

}  // namespace apkrt::signals
