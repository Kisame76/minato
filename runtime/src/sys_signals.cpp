// Signal syscalls of the Linux syscall layer (ADR-0009, ARCHITECTURE.md 4.5, R9) on the emulated
// signal state of signals.h: masks, dispositions, alternate stacks, sending and waiting. No guest
// disposition or mask reaches a host signal.
#include "signals.h"
#include "syscalls.h"
#include "threads.h"

#include <os/os_sync_wait_on_address.h>
#include <time.h>
#include <unistd.h>

#include <cstring>

namespace apkrt::sys {
namespace {

using signals::bit;
using signals::LinuxSigaction;
using signals::LinuxSiginfo;
using signals::LinuxStack;

constexpr int linux_sig_block = 0;
constexpr int linux_sig_unblock = 1;
constexpr int linux_sig_setmask = 2;
constexpr std::uint64_t linux_sigset_size = 8;
constexpr int sigkill = 9, sigstop = 19;
// SIGKILL and SIGSTOP cannot be blocked; Linux drops them from every mask silently.
constexpr std::uint64_t unblockable = bit(sigkill) | bit(sigstop);
constexpr std::uint64_t linux_minsigstksz = 5120;  // arm64

bool valid_signal(int signal) {
    return signal >= 1 && signal <= signals::signal_count;
}

LinuxSiginfo sender_info(int signal, std::int32_t code) {
    LinuxSiginfo info{};
    info.signo = signal;
    info.code = code;
    info.fields.sender.pid = ::getpid();
    info.fields.sender.uid = ::getuid();
    return info;
}

/// Waits until the thread's wake count moves on from `seen` or `timeout_ns` passes (never with -1).
/// Returns false on the timeout.
bool wait_for_signal(signals::ThreadSignals &state, std::uint32_t seen, std::int64_t timeout_ns) {
    if (timeout_ns < 0) {
        ::os_sync_wait_on_address(&state.wake, seen, sizeof seen, OS_SYNC_WAIT_ON_ADDRESS_NONE);
        return true;
    }
    if (timeout_ns == 0) return false;
    const int result = ::os_sync_wait_on_address_with_timeout(&state.wake, seen, sizeof seen,
                                                              OS_SYNC_WAIT_ON_ADDRESS_NONE, OS_CLOCK_MACH_ABSOLUTE_TIME,
                                                              static_cast<std::uint64_t>(timeout_ns));
    return !(result < 0 && errno == ETIMEDOUT);
}

}  // namespace

long sys_rt_sigprocmask(int how, std::uint64_t new_address, std::uint64_t old_address, std::uint64_t size) {
    if (size != linux_sigset_size) return -linux_errno::einval;
    auto &state = signals::current();
    const std::uint64_t old_mask = state.mask.load();
    if (new_address != 0) {
        const auto requested = *as_pointer<const std::uint64_t>(new_address);
        std::uint64_t mask;
        switch (how) {
            case linux_sig_block: mask = old_mask | requested; break;
            case linux_sig_unblock: mask = old_mask & ~requested; break;
            case linux_sig_setmask: mask = requested; break;
            default: return -linux_errno::einval;
        }
        state.mask.store(mask & ~unblockable);  // what it unblocks arrives at the end of this syscall
    }
    if (old_address != 0) *as_pointer<std::uint64_t>(old_address) = old_mask;
    return 0;
}

long sys_rt_sigaction(int signal, std::uint64_t new_address, std::uint64_t old_address, std::uint64_t size) {
    if (size != linux_sigset_size || !valid_signal(signal)) return -linux_errno::einval;
    const LinuxSigaction previous = signals::action(signal);
    if (new_address != 0) {
        if (signal == sigkill || signal == sigstop) return -linux_errno::einval;
        auto wanted = *as_pointer<const LinuxSigaction>(new_address);
        wanted.mask &= ~unblockable;
        signals::set_action(signal, wanted);
    }
    if (old_address != 0) *as_pointer<LinuxSigaction>(old_address) = previous;
    return 0;
}

long sys_sigaltstack(std::uint64_t new_address, std::uint64_t old_address) {
    auto &state = signals::current();
    const LinuxStack previous = state.alternate_stack;
    if (new_address != 0) {
        const auto wanted = *as_pointer<const LinuxStack>(new_address);
        const auto mode = wanted.flags & ~signals::ss_autodisarm;
        if (mode == signals::ss_disable) {
            state.alternate_stack = {0, signals::ss_disable, 0, 0};
        } else if (mode == 0 || mode == signals::ss_onstack) {
            if (wanted.size < linux_minsigstksz) return -linux_errno::enomem;
            state.alternate_stack = {wanted.sp, wanted.flags & signals::ss_autodisarm, 0, wanted.size};
        } else {
            return -linux_errno::einval;
        }
    }
    if (old_address != 0) *as_pointer<LinuxStack>(old_address) = previous;
    return 0;
}

long sys_rt_sigpending(std::uint64_t address, std::uint64_t size) {
    if (size != linux_sigset_size) return -linux_errno::einval;
    const auto &state = signals::current();
    // Linux reports the pending signals that are blocked; the others are delivered already.
    *as_pointer<std::uint64_t>(address) = signals::pending(state) & state.mask.load();
    return 0;
}

long sys_rt_sigsuspend(std::uint64_t address, std::uint64_t size) {
    if (size != linux_sigset_size) return -linux_errno::einval;
    auto &state = signals::current();
    const auto temporary = *as_pointer<const std::uint64_t>(address) & ~unblockable;
    // The handler's frame keeps the mask from before, which rt_sigreturn restores.
    state.frame_mask = state.mask.load();
    state.frame_mask_set = true;
    state.mask.store(temporary);
    while (true) {
        const auto seen = state.wake.load();
        if ((signals::pending(state) & ~temporary) != 0) break;
        wait_for_signal(state, seen, -1);
    }
    return -linux_errno::eintr;  // delivered at the end of this syscall
}

long sys_rt_sigtimedwait(std::uint64_t set_address, std::uint64_t info_address, std::uint64_t timeout_address,
                         std::uint64_t size) {
    if (size != linux_sigset_size) return -linux_errno::einval;
    auto &state = signals::current();
    const auto wanted = *as_pointer<const std::uint64_t>(set_address) & ~unblockable;
    std::int64_t deadline = -1;
    if (timeout_address != 0) {
        const auto &timeout = *as_pointer<const timespec>(timeout_address);
        if (timeout.tv_sec < 0 || timeout.tv_nsec < 0 || timeout.tv_nsec >= 1'000'000'000) return -linux_errno::einval;
        deadline = static_cast<std::int64_t>(clock_gettime_nsec_np(CLOCK_UPTIME_RAW)) +
                   static_cast<std::int64_t>(timeout.tv_sec) * 1'000'000'000 + timeout.tv_nsec;
    }
    while (true) {
        const auto seen = state.wake.load();
        int signal;
        LinuxSiginfo info;
        if (signals::take(state, wanted, signal, info)) {
            if (info_address != 0) std::memcpy(as_pointer<void>(info_address), &info, sizeof info);
            return signal;
        }
        // Another signal the thread takes now interrupts the wait.
        if ((signals::pending(state) & ~state.mask.load()) != 0) return -linux_errno::eintr;
        std::int64_t left = -1;
        if (deadline >= 0) {
            left = deadline - static_cast<std::int64_t>(clock_gettime_nsec_np(CLOCK_UPTIME_RAW));
            if (left <= 0) return -linux_errno::eagain;
        }
        wait_for_signal(state, seen, left);
    }
}

long sys_kill(int pid, int signal) {
    // Other processes do not exist for a guest (F11); 0 and -1 mean its own process group, and
    // every process it may signal: its own.
    if (pid != ::getpid() && pid != 0 && pid != -1 && pid != -::getpid()) return -linux_errno::esrch;
    if (signal != 0 && !valid_signal(signal)) return -linux_errno::einval;
    return signals::send_to_process(signal, sender_info(signal, signals::si_user));
}

long sys_tgkill(int process, int tid, int signal) {
    if (tid <= 0 || (process != -1 && process <= 0)) return -linux_errno::einval;
    if (process != -1 && process != ::getpid()) return -linux_errno::esrch;
    if (signal != 0 && !valid_signal(signal)) return -linux_errno::einval;
    return signals::send_to_thread(tid, signal, sender_info(signal, signals::si_tkill));
}

long sys_rt_sigqueueinfo(int process, int tid, int signal, std::uint64_t info_address) {
    if (process != ::getpid()) return -linux_errno::esrch;
    if (!valid_signal(signal)) return -linux_errno::einval;
    auto info = *as_pointer<const LinuxSiginfo>(info_address);
    info.signo = signal;
    return tid == 0 ? signals::send_to_process(signal, info) : signals::send_to_thread(tid, signal, info);
}

}  // namespace apkrt::sys
