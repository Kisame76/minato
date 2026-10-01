// Internal interface of the Linux syscall layer (ADR-0009, ARCHITECTURE.md 4.5): what the parts in
// sys_*.cpp share with the dispatcher in syscalls.cpp. Linux numbers, errno values and structures
// in, Darwin calls out.
#pragma once

#include "descriptors.h"
#include "signals.h"
#include "syscall_frame.h"

#include <sys/uio.h>
#include <time.h>

#include <cstdint>
#include <optional>

namespace apkrt::sys {

// Linux errno values (include/uapi/asm-generic/errno-base.h, errno.h).
namespace linux_errno {
constexpr long eperm = 1;
constexpr long enoent = 2;
constexpr long esrch = 3;
constexpr long eintr = 4;
constexpr long eio = 5;
constexpr long ebadf = 9;
constexpr long echild = 10;
constexpr long eagain = 11;
constexpr long enomem = 12;
constexpr long eacces = 13;
constexpr long efault = 14;
constexpr long ebusy = 16;
constexpr long eexist = 17;
constexpr long exdev = 18;
constexpr long enodev = 19;
constexpr long enotdir = 20;
constexpr long eisdir = 21;
constexpr long einval = 22;
constexpr long enotty = 25;
constexpr long espipe = 29;
constexpr long erofs = 30;
constexpr long epipe = 32;
constexpr long erange = 34;
constexpr long edeadlk = 35;
constexpr long enametoolong = 36;
constexpr long enolck = 37;
constexpr long enosys = 38;
constexpr long enotempty = 39;
constexpr long eloop = 40;
constexpr long enomsg = 42;
constexpr long eidrm = 43;
constexpr long enodata = 61;
constexpr long etime = 62;
constexpr long eproto = 71;
constexpr long ebadmsg = 74;
constexpr long eoverflow = 75;
constexpr long eilseq = 84;
constexpr long enotsock = 88;
constexpr long edestaddrreq = 89;
constexpr long emsgsize = 90;
constexpr long eprototype = 91;
constexpr long enoprotoopt = 92;
constexpr long eprotonosupport = 93;
constexpr long eopnotsupp = 95;
constexpr long eafnosupport = 97;
constexpr long eaddrinuse = 98;
constexpr long eaddrnotavail = 99;
constexpr long enetdown = 100;
constexpr long enetunreach = 101;
constexpr long econnaborted = 103;
constexpr long econnreset = 104;
constexpr long enobufs = 105;
constexpr long eisconn = 106;
constexpr long enotconn = 107;
constexpr long etimedout = 110;
constexpr long econnrefused = 111;
constexpr long ehostunreach = 113;
constexpr long ealready = 114;
constexpr long einprogress = 115;
constexpr long ecanceled = 125;
}  // namespace linux_errno

/// Darwin errno to Linux errno (F9).
long to_linux_errno(int error);

/// The Linux way to return from a syscall: the result, or -errno from the host's errno.
long result_or_error(long result);

/// Writes a message about something the layer does not offer yet to stderr, without allocating.
void report(const char *message);

/// Raises SIGPIPE for the calling thread, as Linux does for a write to a pipe or socket nobody reads.
void raise_sigpipe();

inline int as_int(std::uint64_t value) {
    return static_cast<int>(static_cast<std::int32_t>(value));  // Linux passes int arguments in the low half
}

template <typename T>
T *as_pointer(std::uint64_t value) {
    return reinterpret_cast<T *>(static_cast<std::uintptr_t>(value));
}

// --- Time (syscalls.cpp) ------------------------------------------------------------------------------

/// Linux's CLOCK_MONOTONIC, Darwin's CLOCK_UPTIME_RAW (F10), in nanoseconds: the clock of deadlines.
std::int64_t monotonic_now();

/// A valid time (neither field negative, tv_nsec below a second) in nanoseconds; INT64_MAX where it
/// does not fit, as Linux's ktime saturates.
std::int64_t nanoseconds_of(const timespec &time);

/// The deadline `nanoseconds` (not negative) from now; INT64_MAX where it does not fit.
std::int64_t deadline_after(std::int64_t nanoseconds);

// --- Waiting (sys_signals.cpp, ADR-0026) --------------------------------------------------------------
//
// Every blocking syscall that does not wait on descriptors (for those see below) waits in wait_until, on
// the calling thread's signal word (ThreadSignals::wake), which every signal sent to the thread and every
// futex wake of it raise. It reads the word before it checks, and sleeps only while the word still has
// that value: whatever raises it after the checks keeps the sleep from beginning, so no signal slips in
// between.

/// Why a wait ended: its own condition came true, the deadline passed, or a signal the thread takes is
/// pending, checked in this order. As on Linux, a wait whose time is up ends as timed out even when a
/// signal came as well; the signal is delivered at the end of the syscall all the same. ppoll and
/// pselect6 look for the signal before the time, as Linux's do_poll and do_select do (sys_events.cpp).
enum class WaitEnd { done, timeout, signal };

constexpr std::int64_t no_deadline = -1;

/// Sleeps while the thread's signal word is still `seen`, until `deadline` (monotonic_now's clock) at
/// the latest. May come back early.
void sleep_on_wake(signals::ThreadSignals &state, std::uint32_t seen, std::int64_t deadline);

/// Waits until `done()` holds, `deadline` passes, or a signal the thread does not block is pending.
/// Anything else that wakes the thread, the runtime signal for a blocked signal among them, only
/// leads to the next round.
template <typename Done>
WaitEnd wait_until(signals::ThreadSignals &state, Done done, std::int64_t deadline) {
    while (true) {
        const std::uint32_t seen = state.wake.load();
        if (done()) return WaitEnd::done;
        if (deadline != no_deadline && monotonic_now() >= deadline) return WaitEnd::timeout;
        if ((state.pending.load() & ~state.mask.load()) != 0) return WaitEnd::signal;
        sleep_on_wake(state, seen, deadline);
    }
}

// --- Waiting on descriptors (sys_signals.cpp, ADR-0027) -----------------------------------------------
//
// A wait for host descriptors sleeps in the calling thread's kqueue (signals.h, wait_queue_of), where
// notify() triggers a user event along with the signal word. The kqueue is there before the wait first
// checks for signals, and its event stays triggered until a sleep takes it: a signal sent after the
// check ends the sleep at once.

/// A host descriptor a wait watches: for reading (EVFILT_READ) or for writing (EVFILT_WRITE).
struct Watched {
    int host;
    std::int16_t filter;
    bool ready = false;           // the kqueue reported it at the last probe or sleep
    bool armed = false;           // its filter is in the thread's kqueue
    bool end = false;             // when ready: the other side is gone (EV_EOF)
    std::int64_t available = -1;  // when ready: the bytes to read or the room to write; -1 unknown
};

/// Asks the thread's kqueue at once which of `watched` are ready; makes the kqueue if need be.
void probe_descriptors(signals::ThreadSignals &state, Watched *watched, int count);

/// Sleeps in the thread's kqueue until one of `watched` is ready, notify() is called or `deadline`
/// (monotonic_now's clock) passes. May come back early.
void sleep_on_descriptors(signals::ThreadSignals &state, Watched *watched, int count, std::int64_t deadline);

/// Takes the filters of `watched` that a probe or a sleep left in the thread's kqueue out again.
void stop_watching(signals::ThreadSignals &state, Watched *watched, int count);

/// wait_until for descriptors: waits until `done()` holds, `deadline` passes, or a signal the thread
/// does not block is pending, in this order, and sleeps on `watched` in between. A wait whose `done()`
/// holds at once makes no kqueue, unless `done()` itself asks the kqueue (probe_descriptors).
template <typename Done>
WaitEnd wait_for_descriptors(signals::ThreadSignals &state, Watched *watched, int count, Done done,
                             std::int64_t deadline) {
    WaitEnd end;
    while (true) {
        if (done()) {
            end = WaitEnd::done;
            break;
        }
        if (deadline != no_deadline && monotonic_now() >= deadline) {
            end = WaitEnd::timeout;
            break;
        }
        signals::wait_queue_of(state);  // before the check: a signal sent after it triggers the user event
        if ((state.pending.load() & ~state.mask.load()) != 0) {
            end = WaitEnd::signal;
            break;
        }
        sleep_on_descriptors(state, watched, count, deadline);
    }
    stop_watching(state, watched, count);
    return end;
}

/// The signal mask that ppoll, pselect6, epoll_pwait and epoll_pwait2 take for their wait: in force from
/// the construction, when `address` holds one. At end(), the thread's own comes back, unless a signal
/// ended the wait: then, as in rt_sigsuspend, the temporary mask holds until the delivery at the end of
/// the syscall, and the handler's frame returns to the thread's own.
class WaitMask {
public:
    WaitMask(signals::ThreadSignals &state, std::uint64_t address);
    WaitMask(const WaitMask &) = delete;
    WaitMask &operator=(const WaitMask &) = delete;

    void end(WaitEnd end);

private:
    signals::ThreadSignals &state_;
    const std::uint64_t own_;
    bool applied_ = false;
};

// --- futex (sys_futex.cpp, ADR-0026) ------------------------------------------------------------------

/// `timeout_address` is the count to move for FUTEX_REQUEUE, `address2` the word it moves to.
long sys_futex(std::uint64_t address, int operation, std::uint32_t value, std::uint64_t timeout_address,
               std::uint64_t address2, std::uint32_t bitset);

/// Wakes up to `count` waiters on `word` and returns how many it woke, as FUTEX_WAKE does. Private and
/// shared futexes are the same: every waiter is in this one process.
long futex_wake(std::uint32_t *word, int count, bool shared);

// --- Memory (sys_memory.cpp) -------------------------------------------------------------------------

long sys_mmap(std::uint64_t address, std::uint64_t length, int protection, int flags, int fd, std::uint64_t offset);
/// munmap; a range the calling guest thread still runs on (its own stack, unmapped right before
/// exit) is unmapped once the thread has left it.
long sys_munmap(const apkrt_syscall_frame &frame, std::uint64_t address, std::uint64_t length);
long sys_mprotect(std::uint64_t address, std::uint64_t length, int protection);
long sys_madvise(std::uint64_t address, std::uint64_t length, int advice);

// --- Signals (sys_signals.cpp) -----------------------------------------------------------------------

long sys_rt_sigprocmask(int how, std::uint64_t new_address, std::uint64_t old_address, std::uint64_t size);
long sys_rt_sigaction(int signal, std::uint64_t new_address, std::uint64_t old_address, std::uint64_t size);
long sys_sigaltstack(std::uint64_t new_address, std::uint64_t old_address);
long sys_rt_sigpending(std::uint64_t address, std::uint64_t size);
long sys_rt_sigsuspend(std::uint64_t address, std::uint64_t size);
long sys_rt_sigtimedwait(std::uint64_t set_address, std::uint64_t info_address, std::uint64_t timeout_address,
                         std::uint64_t size);
long sys_kill(int pid, int signal);
/// tgkill; tkill is tgkill with process -1.
long sys_tgkill(int process, int tid, int signal);
/// rt_sigqueueinfo (tid 0: to the process) and rt_tgsigqueueinfo.
long sys_rt_sigqueueinfo(int process, int tid, int signal, std::uint64_t info_address);

// --- Threads (sys_threads.cpp) -----------------------------------------------------------------------

long sys_clone(const apkrt_syscall_frame &frame, std::uint64_t flags, std::uint64_t stack, std::uint64_t parent_tid,
               std::uint64_t tls, std::uint64_t child_tid);
long sys_set_tid_address(std::uint64_t address);
long sys_gettid();

// --- Files (sys_files.cpp) ---------------------------------------------------------------------------

long sys_fcntl(int fd, int command, std::uint64_t argument);
long sys_fstat(int fd, std::uint64_t buffer);
long sys_openat(int directory, std::uint64_t path, int flags, int mode);
long sys_close(int fd);
long sys_faccessat(int directory, std::uint64_t path, int mode, int flags);
long sys_newfstatat(int directory, std::uint64_t path, std::uint64_t buffer, int flags);
long sys_mkdirat(int directory, std::uint64_t path, int mode);
long sys_unlinkat(int directory, std::uint64_t path, int flags);
long sys_renameat2(int old_directory, std::uint64_t old_path, int new_directory, std::uint64_t new_path,
                   unsigned flags);
long sys_linkat(int old_directory, std::uint64_t old_path, int new_directory, std::uint64_t new_path, int flags);
long sys_symlinkat(std::uint64_t target, int directory, std::uint64_t path);
long sys_readlinkat(int directory, std::uint64_t path, std::uint64_t buffer, std::int64_t size);
long sys_fchmodat(int directory, std::uint64_t path, int mode);
long sys_fchownat(int directory, std::uint64_t path, std::uint32_t owner, std::uint32_t group, int flags);
long sys_truncate(std::uint64_t path, std::int64_t length);
long sys_utimensat(int directory, std::uint64_t path, std::uint64_t times, int flags);
long sys_getcwd(std::uint64_t buffer, std::uint64_t size);
long sys_chdir(std::uint64_t path);
long sys_fchdir(int fd);
long sys_lseek(int fd, std::int64_t offset, int whence);
long sys_getdents64(int fd, std::uint64_t buffer, std::uint64_t size);
long sys_dup(int fd);
long sys_dup3(int fd, int target, int flags);
long sys_pipe2(std::uint64_t fds, int flags);
long sys_read(int fd, std::uint64_t buffer, std::uint64_t size);
long sys_write(int fd, std::uint64_t buffer, std::uint64_t size);
long sys_readv(int fd, std::uint64_t vectors, int count);
long sys_writev(int fd, std::uint64_t vectors, int count);
long sys_pread64(int fd, std::uint64_t buffer, std::uint64_t size, std::int64_t offset);
long sys_pwrite64(int fd, std::uint64_t buffer, std::uint64_t size, std::int64_t offset);
long sys_preadv(int fd, std::uint64_t vectors, int count, std::int64_t offset);
long sys_pwritev(int fd, std::uint64_t vectors, int count, std::int64_t offset);
long sys_fchmod(int fd, int mode);
long sys_fchown(int fd, std::uint32_t owner, std::uint32_t group);
long sys_ftruncate(int fd, std::int64_t length);
/// fsync and fdatasync: Darwin's fsync is what Linux's fdatasync promises and more.
long sys_fsync(int fd);
long sys_ioctl(int fd, std::uint32_t request, std::uint64_t argument);
long sys_statfs(std::uint64_t path, std::uint64_t buffer);
long sys_fstatfs(int fd, std::uint64_t buffer);

// --- eventfd, epoll, ppoll and pselect6 (sys_events.cpp, ADR-0027) -----------------------------------

long sys_eventfd2(unsigned initial, int flags);

/// read, write, readv and writev of an eventfd (Description::Kind::eventfd), with at least one vector.
long transfer_count(descriptors::Description &counter, bool writing, const iovec *vectors, int count);

long sys_epoll_create1(int flags);
long sys_epoll_ctl(int epoll, int operation, int fd, std::uint64_t event);
/// epoll_pwait: `timeout` in milliseconds, negative for none.
long sys_epoll_pwait(int epoll, std::uint64_t events, int maximum, int timeout, std::uint64_t mask,
                     std::uint64_t mask_size);
/// epoll_pwait2: `timeout` a timespec, 0 for none.
long sys_epoll_pwait2(int epoll, std::uint64_t events, int maximum, std::uint64_t timeout, std::uint64_t mask,
                      std::uint64_t mask_size);
long sys_ppoll(std::uint64_t fds, std::uint64_t count, std::uint64_t timeout, std::uint64_t mask,
               std::uint64_t mask_size);
/// pselect6: `mask` the address of a pair of the mask's address and its size.
long sys_pselect6(int count, std::uint64_t read_set, std::uint64_t write_set, std::uint64_t except_set,
                  std::uint64_t timeout, std::uint64_t mask);

}  // namespace apkrt::sys
