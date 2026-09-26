// Signal syscalls of the Linux syscall layer (ADR-0009, ARCHITECTURE.md 4.5, R9) on the emulated
// signal state of signals.h: masks, dispositions, alternate stacks, sending and waiting, and the sleep
// of every blocking syscall on the thread's signal word (ADR-0026) or in its kqueue (ADR-0027). No
// guest disposition or mask reaches a host signal.
#include "signals.h"
#include "syscalls.h"
#include "threads.h"

#include <os/os_sync_wait_on_address.h>
#include <sys/event.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <vector>

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

}  // namespace

void sleep_on_wake(signals::ThreadSignals &state, std::uint32_t seen, std::int64_t deadline) {
    if (deadline == no_deadline) {
        ::os_sync_wait_on_address(&state.wake, seen, sizeof seen, OS_SYNC_WAIT_ON_ADDRESS_NONE);
        return;
    }
    const std::int64_t left = deadline - monotonic_now();
    if (left <= 0) return;
    // A day at most per sleep, far within what the kernel converts to its ticks; wait_until sleeps again.
    constexpr std::int64_t day = 86'400'000'000'000;
    ::os_sync_wait_on_address_with_timeout(&state.wake, seen, sizeof seen, OS_SYNC_WAIT_ON_ADDRESS_NONE,
                                           OS_CLOCK_MACH_ABSOLUTE_TIME, static_cast<std::uint64_t>(std::min(left, day)));
}

namespace {

/// Puts the filters of `watched` into the thread's kqueue, one-shot, and takes what the kqueue reports
/// within `timeout` (none: until something comes): the descriptors that are ready, with the bytes to
/// read or the room to write, and notify()'s event. False when the kqueue cannot tell: the thread has
/// none, or the kqueue fails or refuses a descriptor. What it cannot tell about counts as ready, for the
/// call to find out.
bool collect(signals::ThreadSignals &state, Watched *watched, int count, const timespec *timeout) {
    const auto cannot_tell = [&] {
        for (int index = 0; index < count; ++index) {
            watched[index].ready = true;
            watched[index].armed = false;
            watched[index].end = false;
            watched[index].available = -1;
        }
        return false;
    };
    const int queue = signals::wait_queue_of(state);
    if (queue < 0) return cannot_tell();
    std::vector<struct kevent> changes(static_cast<std::size_t>(count));
    std::vector<struct kevent> events(static_cast<std::size_t>(count) + 1);
    for (int index = 0; index < count; ++index) {
        Watched &entry = watched[index];
        EV_SET(&changes[static_cast<std::size_t>(index)], entry.host, entry.filter, EV_ADD | EV_ONESHOT, 0, 0, &entry);
        entry.ready = false;
        entry.end = false;
        entry.armed = true;
    }
    const int got = ::kevent(queue, changes.data(), count, events.data(), count + 1, timeout);
    if (got < 0 && errno != EINTR) return cannot_tell();  // SIGURG: the wait looks again
    bool watching = true;
    for (int index = 0; index < got; ++index) {
        const struct kevent &event = events[static_cast<std::size_t>(index)];
        if (event.filter == EVFILT_USER) continue;  // notify(): the wait looks again
        auto &entry = *static_cast<Watched *>(event.udata);
        entry.armed = false;  // a one-shot filter goes as it fires, and a refused one never came in
        entry.ready = true;
        // With EV_EOF the data is still what there is to read, or 0 room.
        entry.end = (event.flags & EV_EOF) != 0;
        entry.available = (event.flags & EV_ERROR) != 0 ? -1 : static_cast<std::int64_t>(event.data);
        if ((event.flags & EV_ERROR) != 0) watching = false;
    }
    return watching;
}

}  // namespace

void probe_descriptors(signals::ThreadSignals &state, Watched *watched, int count) {
    const timespec now{0, 0};
    collect(state, watched, count, &now);
}

void sleep_on_descriptors(signals::ThreadSignals &state, Watched *watched, int count, std::int64_t deadline) {
    const std::uint32_t seen = state.wake.load();
    const bool watching = [&] {
        if (deadline == no_deadline) return collect(state, watched, count, nullptr);
        const std::int64_t left = std::max<std::int64_t>(deadline - monotonic_now(), 0);
        const timespec timeout{left / 1'000'000'000, left % 1'000'000'000};
        return collect(state, watched, count, &timeout);
    }();
    if (watching) return;
    // The kqueue watches every kind of descriptor that can block (ADR-0027, decision 5), and every
    // thread gets one unless the host runs out: should either fail, the wait looks again every
    // millisecond, and a signal still ends the sleep on the word.
    const std::int64_t soon = monotonic_now() + 1'000'000;
    sleep_on_wake(state, seen, deadline == no_deadline ? soon : std::min(deadline, soon));
}

void stop_watching(signals::ThreadSignals &state, Watched *watched, int count) {
    const int queue = state.wait_queue.load();
    if (queue < 0) return;
    std::vector<struct kevent> changes;
    for (int index = 0; index < count; ++index) {
        Watched &entry = watched[index];
        if (!entry.armed) continue;
        struct kevent change;
        EV_SET(&change, entry.host, entry.filter, EV_DELETE | EV_RECEIPT, 0, 0, nullptr);
        changes.push_back(change);
        entry.armed = false;
    }
    if (changes.empty()) return;
    // With EV_RECEIPT every change is made and answered, and nothing else is taken from the kqueue.
    std::vector<struct kevent> receipts(changes.size());
    ::kevent(queue, changes.data(), static_cast<int>(changes.size()), receipts.data(), static_cast<int>(receipts.size()),
             nullptr);
}

WaitMask::WaitMask(signals::ThreadSignals &state, std::uint64_t address) : state_(state), own_(state.mask.load()) {
    if (address == 0) return;
    state.mask.store(*as_pointer<const std::uint64_t>(address) & ~unblockable);
    applied_ = true;
}

void WaitMask::end(WaitEnd end) {
    if (!applied_) return;
    applied_ = false;
    if (end == WaitEnd::signal) {
        state_.frame_mask = own_;
        state_.frame_mask_set = true;
        return;
    }
    state_.mask.store(own_);
}

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
    wait_until(state, [] { return false; }, no_deadline);  // only a signal ends it
    return -linux_errno::eintr;  // delivered at the end of this syscall
}

long sys_rt_sigtimedwait(std::uint64_t set_address, std::uint64_t info_address, std::uint64_t timeout_address,
                         std::uint64_t size) {
    if (size != linux_sigset_size) return -linux_errno::einval;
    auto &state = signals::current();
    const auto wanted = *as_pointer<const std::uint64_t>(set_address) & ~unblockable;
    std::int64_t deadline = no_deadline;
    if (timeout_address != 0) {
        const auto &timeout = *as_pointer<const timespec>(timeout_address);
        if (timeout.tv_sec < 0 || timeout.tv_nsec < 0 || timeout.tv_nsec >= 1'000'000'000) return -linux_errno::einval;
        deadline = deadline_after(nanoseconds_of(timeout));
    }
    int signal = 0;
    LinuxSiginfo info{};
    switch (wait_until(state, [&] { return signals::take(state, wanted, signal, info); }, deadline)) {
        case WaitEnd::done:
            if (info_address != 0) std::memcpy(as_pointer<void>(info_address), &info, sizeof info);
            return signal;
        case WaitEnd::signal: return -linux_errno::eintr;  // another signal the thread takes now
        case WaitEnd::timeout: return -linux_errno::eagain;
    }
    return -linux_errno::eagain;
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
