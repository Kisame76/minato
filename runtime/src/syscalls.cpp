// The Linux syscall layer (ADR-0009, ARCHITECTURE.md 4.5): Linux AArch64 syscall numbers, argument
// conventions, errno values and structures in, Darwin calls out. Numbers are never passed through
// to Darwin. This file has the dispatcher and the small syscalls; memory, files, threads and signals
// have files of their own (sys_*.cpp, syscalls.h). At the end of each syscall, guest signals that
// became deliverable are delivered (signals.h). Everything unknown answers -ENOSYS and is reported
// once on stderr, so the gaps show up while programs are brought up.
#include "dl.h"
#include "guest_context.h"
#include "loader_syscalls.h"
#include "process_exit.h"
#include "signals.h"
#include "syscalls.h"
#include "threads.h"

#include <os/os_sync_wait_on_address.h>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>

namespace apkrt::sys {

// 1-34 agree except 11, which is EDEADLK on Darwin and EAGAIN on
/// Linux; above that almost nothing agrees.
long to_linux_errno(int error) {
    if (error >= 1 && error <= 34 && error != EDEADLK) return error;
    switch (error) {
        case EDEADLK: return linux_errno::edeadlk;
        case EAGAIN: return linux_errno::eagain;
        case ENAMETOOLONG: return linux_errno::enametoolong;
        case ENOLCK: return linux_errno::enolck;
        case ENOSYS: return linux_errno::enosys;
        case ENOTEMPTY: return linux_errno::enotempty;
        case ELOOP: return linux_errno::eloop;
        case ENOMSG: return linux_errno::enomsg;
        case EIDRM: return linux_errno::eidrm;
        case ENODATA: return linux_errno::enodata;
        case ETIME: return linux_errno::etime;
        case EPROTO: return linux_errno::eproto;
        case EBADMSG: return linux_errno::ebadmsg;
        case EOVERFLOW: return linux_errno::eoverflow;
        case EILSEQ: return linux_errno::eilseq;
        case ENOTSOCK: return linux_errno::enotsock;
        case EDESTADDRREQ: return linux_errno::edestaddrreq;
        case EMSGSIZE: return linux_errno::emsgsize;
        case EPROTOTYPE: return linux_errno::eprototype;
        case ENOPROTOOPT: return linux_errno::enoprotoopt;
        case EPROTONOSUPPORT: return linux_errno::eprotonosupport;
        case ENOTSUP:
        case EOPNOTSUPP: return linux_errno::eopnotsupp;
        case EAFNOSUPPORT: return linux_errno::eafnosupport;
        case EADDRINUSE: return linux_errno::eaddrinuse;
        case EADDRNOTAVAIL: return linux_errno::eaddrnotavail;
        case ENETDOWN: return linux_errno::enetdown;
        case ENETUNREACH: return linux_errno::enetunreach;
        case ECONNABORTED: return linux_errno::econnaborted;
        case ECONNRESET: return linux_errno::econnreset;
        case ENOBUFS: return linux_errno::enobufs;
        case EISCONN: return linux_errno::eisconn;
        case ENOTCONN: return linux_errno::enotconn;
        case ETIMEDOUT: return linux_errno::etimedout;
        case ECONNREFUSED: return linux_errno::econnrefused;
        case EHOSTUNREACH: return linux_errno::ehostunreach;
        case EALREADY: return linux_errno::ealready;
        case EINPROGRESS: return linux_errno::einprogress;
        case ECANCELED: return linux_errno::ecanceled;
        default: return linux_errno::eio;
    }
}

long result_or_error(long result) {
    return result >= 0 ? result : -to_linux_errno(errno);
}

void report(const char *message) {
    static_cast<void>(::write(STDERR_FILENO, message, std::char_traits<char>::length(message)));
}

long futex_wake(std::uint32_t *word, int count, bool shared) {
    const auto flags = shared ? OS_SYNC_WAKE_BY_ADDRESS_SHARED : OS_SYNC_WAKE_BY_ADDRESS_NONE;
    long woken = 0;
    while (woken < count) {
        if (::os_sync_wake_by_address_any(word, sizeof *word, flags) != 0) break;  // ENOENT: nobody left
        ++woken;
    }
    return woken;
}

}  // namespace apkrt::sys

namespace {

using namespace apkrt::sys;

// Linux AArch64 syscall numbers (include/uapi/asm-generic/unistd.h).
namespace nr {
constexpr std::uint64_t getcwd = 17;
constexpr std::uint64_t dup = 23;
constexpr std::uint64_t dup3 = 24;
constexpr std::uint64_t fcntl = 25;
constexpr std::uint64_t ioctl = 29;
constexpr std::uint64_t mkdirat = 34;
constexpr std::uint64_t unlinkat = 35;
constexpr std::uint64_t symlinkat = 36;
constexpr std::uint64_t linkat = 37;
constexpr std::uint64_t statfs = 43;
constexpr std::uint64_t fstatfs = 44;
constexpr std::uint64_t truncate = 45;
constexpr std::uint64_t ftruncate = 46;
constexpr std::uint64_t faccessat = 48;
constexpr std::uint64_t chdir = 49;
constexpr std::uint64_t fchdir = 50;
constexpr std::uint64_t fchmod = 52;
constexpr std::uint64_t fchmodat = 53;
constexpr std::uint64_t fchownat = 54;
constexpr std::uint64_t fchown = 55;
constexpr std::uint64_t openat = 56;
constexpr std::uint64_t close = 57;
constexpr std::uint64_t pipe2 = 59;
constexpr std::uint64_t getdents64 = 61;
constexpr std::uint64_t lseek = 62;
constexpr std::uint64_t read = 63;
constexpr std::uint64_t write = 64;
constexpr std::uint64_t readv = 65;
constexpr std::uint64_t writev = 66;
constexpr std::uint64_t pread64 = 67;
constexpr std::uint64_t pwrite64 = 68;
constexpr std::uint64_t preadv = 69;
constexpr std::uint64_t pwritev = 70;
constexpr std::uint64_t readlinkat = 78;
constexpr std::uint64_t newfstatat = 79;
constexpr std::uint64_t fstat = 80;
constexpr std::uint64_t sync = 81;
constexpr std::uint64_t fsync = 82;
constexpr std::uint64_t fdatasync = 83;
constexpr std::uint64_t utimensat = 88;
constexpr std::uint64_t exit = 93;
constexpr std::uint64_t exit_group = 94;
constexpr std::uint64_t set_tid_address = 96;
constexpr std::uint64_t nanosleep = 101;
constexpr std::uint64_t futex = 98;
constexpr std::uint64_t clock_gettime = 113;
constexpr std::uint64_t clock_getres = 114;
constexpr std::uint64_t clock_nanosleep = 115;
constexpr std::uint64_t sched_getscheduler = 120;
constexpr std::uint64_t sched_getparam = 121;
constexpr std::uint64_t sched_getaffinity = 123;
constexpr std::uint64_t sched_yield = 124;
constexpr std::uint64_t kill = 129;
constexpr std::uint64_t tkill = 130;
constexpr std::uint64_t tgkill = 131;
constexpr std::uint64_t sigaltstack = 132;
constexpr std::uint64_t rt_sigsuspend = 133;
constexpr std::uint64_t rt_sigaction = 134;
constexpr std::uint64_t rt_sigprocmask = 135;
constexpr std::uint64_t rt_sigpending = 136;
constexpr std::uint64_t rt_sigtimedwait = 137;
constexpr std::uint64_t rt_sigqueueinfo = 138;
constexpr std::uint64_t rt_sigreturn = 139;
constexpr std::uint64_t umask = 166;
constexpr std::uint64_t prctl = 167;
constexpr std::uint64_t gettimeofday = 169;
constexpr std::uint64_t getpid = 172;
constexpr std::uint64_t getppid = 173;
constexpr std::uint64_t getuid = 174;
constexpr std::uint64_t geteuid = 175;
constexpr std::uint64_t getgid = 176;
constexpr std::uint64_t getegid = 177;
constexpr std::uint64_t gettid = 178;
constexpr std::uint64_t munmap = 215;
constexpr std::uint64_t clone = 220;
constexpr std::uint64_t mmap = 222;
constexpr std::uint64_t mprotect = 226;
constexpr std::uint64_t madvise = 233;
constexpr std::uint64_t rt_tgsigqueueinfo = 240;
constexpr std::uint64_t renameat2 = 276;
constexpr std::uint64_t getrandom = 278;
constexpr std::uint64_t faccessat2 = 439;
}  // namespace nr

/// Reports each unknown syscall number once, on stderr, without allocating.
void report_unsupported(std::uint64_t number) {
    static std::array<std::atomic<bool>, 512> reported{};
    if (number < reported.size() && reported[number].exchange(true)) return;
    char message[80];
    const int length = std::snprintf(message, sizeof message, "apkrt: Linux syscall %llu is not supported yet\n",
                                     static_cast<unsigned long long>(number));
    if (length > 0) static_cast<void>(::write(STDERR_FILENO, message, static_cast<std::size_t>(length)));
}

// --- Scheduling --------------------------------------------------------------------------------------

/// Guest threads run like ordinary Linux threads: SCHED_OTHER with priority 0.
long sys_sched_getscheduler(int pid) {
    if (pid != 0 && pid != ::getpid()) return -linux_errno::esrch;
    return 0;  // SCHED_OTHER
}

long sys_sched_getparam(int pid, std::uint64_t parameters) {
    if (pid != 0 && pid != ::getpid()) return -linux_errno::esrch;
    *as_pointer<int>(parameters) = 0;  // struct sched_param { int sched_priority; }
    return 0;
}

long sys_sched_getaffinity(int pid, std::uint64_t size, std::uint64_t mask) {
    // Linux writes the kernel's CPU mask, 8 bytes up to 64 CPUs, and returns its size.
    constexpr std::uint64_t kernel_mask_size = 8;
    if (pid != 0 && pid != ::getpid()) return -linux_errno::esrch;
    if (size < kernel_mask_size) return -linux_errno::einval;
    int cpus = 1;
    std::size_t length = sizeof cpus;
    if (::sysctlbyname("hw.activecpu", &cpus, &length, nullptr, 0) != 0 || cpus < 1) cpus = 1;
    *as_pointer<std::uint64_t>(mask) = cpus >= 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << cpus) - 1;
    return static_cast<long>(kernel_mask_size);
}

// --- Time -------------------------------------------------------------------------------------------

/// Linux clocks on Darwin clocks (F10): Linux CLOCK_MONOTONIC stops while the device sleeps, like
/// Darwin's CLOCK_UPTIME_RAW; Linux CLOCK_BOOTTIME goes on, like Darwin's CLOCK_MONOTONIC. The coarse
/// clocks read the precise ones. Nullopt for clock ids Linux does not have or we do not offer yet
/// (the negative ids of per-process and per-thread CPU clocks).
std::optional<clockid_t> to_darwin_clock(int linux_clock) {
    switch (linux_clock) {
        case 0:   // CLOCK_REALTIME
        case 5:   // CLOCK_REALTIME_COARSE
        case 8:   // CLOCK_REALTIME_ALARM
        case 11:  // CLOCK_TAI: its offset to UTC stays 0 until someone sets it
            return CLOCK_REALTIME;
        case 1:  // CLOCK_MONOTONIC
        case 4:  // CLOCK_MONOTONIC_RAW
        case 6:  // CLOCK_MONOTONIC_COARSE
            return CLOCK_UPTIME_RAW;
        case 7:  // CLOCK_BOOTTIME
        case 9:  // CLOCK_BOOTTIME_ALARM
            return CLOCK_MONOTONIC;
        case 2: return CLOCK_PROCESS_CPUTIME_ID;
        case 3: return CLOCK_THREAD_CPUTIME_ID;
        default: return std::nullopt;
    }
}

std::int64_t nanoseconds_of(const timespec &time) {
    return static_cast<std::int64_t>(time.tv_sec) * 1'000'000'000 + time.tv_nsec;
}

/// struct timespec has the same layout on both systems: two 64-bit fields.
long sys_clock_gettime(int linux_clock, std::uint64_t time, bool resolution) {
    const auto clock = to_darwin_clock(linux_clock);
    if (!clock) return -linux_errno::einval;
    if (time == 0) return resolution ? 0 : -linux_errno::efault;  // clock_getres may skip the result
    auto *result = as_pointer<timespec>(time);
    return result_or_error(resolution ? ::clock_getres(*clock, result) : ::clock_gettime(*clock, result));
}

/// struct timeval on Linux arm64: two 64-bit fields. Darwin's tv_usec is 32-bit, so it is copied.
long sys_gettimeofday(std::uint64_t time, std::uint64_t zone) {
    if (time != 0) {
        timeval host;
        ::gettimeofday(&host, nullptr);
        const std::int64_t value[2] = {host.tv_sec, host.tv_usec};
        std::memcpy(as_pointer<void>(time), value, sizeof value);
    }
    // struct timezone: Linux keeps what settimeofday set, and nothing sets it on Android.
    if (zone != 0) std::memset(as_pointer<void>(zone), 0, 2 * sizeof(std::int32_t));
    return 0;
}

bool valid(const timespec &time) {
    return time.tv_sec >= 0 && time.tv_nsec >= 0 && time.tv_nsec < 1'000'000'000;
}

/// Sleeps for `duration`; on a signal, writes what is left to `remaining` (if not 0) and answers EINTR.
long sleep_for(timespec duration, std::uint64_t remaining) {
    timespec left{};
    if (::nanosleep(&duration, &left) == 0) return 0;
    const int error = errno;
    if (error == EINTR && remaining != 0) *as_pointer<timespec>(remaining) = left;
    return -to_linux_errno(error);
}

long sys_nanosleep(std::uint64_t request, std::uint64_t remaining) {
    const auto duration = *as_pointer<const timespec>(request);
    if (!valid(duration)) return -linux_errno::einval;
    return sleep_for(duration, remaining);
}

/// Darwin has no clock_nanosleep: a relative sleep is nanosleep, an absolute one sleeps for the
/// difference to the clock's current time, again after a signal, as Linux would.
long sys_clock_nanosleep(int linux_clock, int flags, std::uint64_t request, std::uint64_t remaining) {
    constexpr int timer_abstime = 1;
    const auto clock = to_darwin_clock(linux_clock);
    if (!clock || *clock == CLOCK_THREAD_CPUTIME_ID) return -linux_errno::einval;
    const auto time = *as_pointer<const timespec>(request);
    if (!valid(time)) return -linux_errno::einval;
    if ((flags & timer_abstime) == 0) return sleep_for(time, remaining);
    const std::int64_t target = nanoseconds_of(time);
    while (true) {
        const auto now = static_cast<std::int64_t>(clock_gettime_nsec_np(*clock));
        if (now >= target) return 0;
        const std::int64_t left = target - now;
        const timespec duration{static_cast<time_t>(left / 1'000'000'000), static_cast<long>(left % 1'000'000'000)};
        if (const long result = sleep_for(duration, 0); result != 0) return result;  // no remaining time for TIMER_ABSTIME
    }
}

// --- futex (ADR-0009: os_sync_wait_on_address) ---------------------------------------------------------

namespace linux_futex {
constexpr int wait = 0;
constexpr int wake = 1;
constexpr int wait_bitset = 9;
constexpr int wake_bitset = 10;
constexpr int private_flag = 128;
constexpr int clock_realtime = 256;
constexpr std::uint32_t bitset_match_any = 0xffff'ffff;
}  // namespace linux_futex

long futex_wait(std::uint32_t *word, std::uint32_t expected, std::optional<std::int64_t> timeout_ns, bool shared) {
    // Linux compares first and answers EAGAIN on a mismatch; os_sync would just return.
    if (__atomic_load_n(word, __ATOMIC_SEQ_CST) != expected) return -linux_errno::eagain;
    const auto flags = shared ? OS_SYNC_WAIT_ON_ADDRESS_SHARED : OS_SYNC_WAIT_ON_ADDRESS_NONE;
    int result;
    if (!timeout_ns) {
        result = ::os_sync_wait_on_address(word, expected, sizeof *word, flags);
    } else if (*timeout_ns <= 0) {
        return -linux_errno::etimedout;
    } else {
        result = ::os_sync_wait_on_address_with_timeout(word, expected, sizeof *word, flags,
                                                         OS_CLOCK_MACH_ABSOLUTE_TIME,
                                                         static_cast<std::uint64_t>(*timeout_ns));
    }
    if (result >= 0) return 0;
    return errno == ETIMEDOUT ? -linux_errno::etimedout : -to_linux_errno(errno);
}


long sys_futex(std::uint64_t address, int operation, std::uint32_t value, std::uint64_t timeout_address,
               std::uint32_t bitset) {
    if (address % 4 != 0) return -linux_errno::einval;
    auto *word = as_pointer<std::uint32_t>(address);
    const bool shared = (operation & linux_futex::private_flag) == 0;
    const int command = operation & ~(linux_futex::private_flag | linux_futex::clock_realtime);

    switch (command) {
        case linux_futex::wait:
        case linux_futex::wait_bitset: {
            if (command == linux_futex::wait_bitset && bitset != linux_futex::bitset_match_any) break;
            std::optional<std::int64_t> timeout;
            if (timeout_address != 0) {
                const auto &time = *as_pointer<const timespec>(timeout_address);
                if (time.tv_sec < 0 || time.tv_nsec < 0 || time.tv_nsec >= 1'000'000'000) return -linux_errno::einval;
                if (command == linux_futex::wait) {
                    timeout = nanoseconds_of(time);  // relative
                } else {
                    // Absolute, on CLOCK_MONOTONIC or with FUTEX_CLOCK_REALTIME on CLOCK_REALTIME.
                    const auto clock = (operation & linux_futex::clock_realtime) ? CLOCK_REALTIME : CLOCK_UPTIME_RAW;
                    timeout = nanoseconds_of(time) - static_cast<std::int64_t>(clock_gettime_nsec_np(clock));
                }
            }
            return futex_wait(word, value, timeout, shared);
        }
        case linux_futex::wake:
        case linux_futex::wake_bitset:
            if (command == linux_futex::wake_bitset && bitset != linux_futex::bitset_match_any) break;
            return futex_wake(word, static_cast<int>(std::min<std::uint32_t>(value, INT32_MAX)), shared);
        default: break;
    }
    // Requeue, wake-op, priority inheritance and bitsets other than "any" follow when a program needs them.
    char message[80];
    const int length = std::snprintf(message, sizeof message, "apkrt: futex operation %#x is not supported yet\n",
                                     static_cast<unsigned>(operation));
    if (length > 0) static_cast<void>(::write(STDERR_FILENO, message, static_cast<std::size_t>(length)));
    return -linux_errno::enosys;
}

// --- Process ------------------------------------------------------------------------------------------

// prctl options (include/uapi/linux/prctl.h).
constexpr int pr_set_name = 15;
constexpr int pr_get_name = 16;
constexpr int pr_set_tagged_addr_ctrl = 55;
constexpr int pr_get_tagged_addr_ctrl = 56;
constexpr int pr_set_vma = 0x5356'4d41;
constexpr std::size_t task_name_size = 16;  // TASK_COMM_LEN

long sys_prctl(int option, std::uint64_t argument) {
    const auto subcommand = argument;
    switch (option) {
        case pr_set_name: {
            char name[task_name_size] = {};
            std::snprintf(name, sizeof name, "%s", as_pointer<const char>(argument));
            return ::pthread_setname_np(name) == 0 ? 0 : -linux_errno::einval;
        }
        case pr_get_name:
            return ::pthread_getname_np(::pthread_self(), as_pointer<char>(argument), task_name_size) == 0
                       ? 0
                       : -linux_errno::einval;
        case pr_set_tagged_addr_ctrl:
        case pr_get_tagged_addr_ctrl:
            // The tagged address ABI: with it, bionic would put tags into the top byte of heap
            // pointers. Kernels without it answer EINVAL, and bionic then leaves pointers untagged.
            return -linux_errno::einval;
        case pr_set_vma:
            // PR_SET_VMA_ANON_NAME: names for anonymous memory in /proc/self/maps, which Android's
            // kernels have (CONFIG_ANON_VMA_NAME). Nothing shows them here, but the call succeeds,
            // as on Android; bionic names every thread stack, and errno must stay as it was.
            return subcommand == 0 ? 0 : -linux_errno::einval;
        default: {
            char message[80];
            const int length =
                std::snprintf(message, sizeof message, "apkrt: prctl option %d is not supported yet\n", option);
            if (length > 0) static_cast<void>(::write(STDERR_FILENO, message, static_cast<std::size_t>(length)));
            return -linux_errno::einval;
        }
    }
}

// getrandom flags (include/uapi/linux/random.h): GRND_NONBLOCK, GRND_RANDOM, GRND_INSECURE.
constexpr unsigned grnd_known = 0x1 | 0x2 | 0x4;

long sys_getrandom(std::uint64_t buffer, std::uint64_t length, unsigned flags) {
    if (flags & ~grnd_known) return -linux_errno::einval;
    // The kernel's generator is always seeded on Darwin, so no flag can make this block.
    arc4random_buf(as_pointer<void>(buffer), length);
    return static_cast<long>(length);
}

bool is_write(std::uint64_t number) {
    return number == nr::write || number == nr::writev || number == nr::pwrite64 || number == nr::pwritev;
}

long dispatch(const apkrt_syscall_frame &frame) {
    const auto *x = frame.x;
    switch (x[8]) {
        case nr::write:
            return result_or_error(::write(as_int(x[0]), as_pointer<const void>(x[1]), x[2]));

        case nr::writev:
            // struct iovec has the same layout on Linux and Darwin: a pointer and a size_t.
            return result_or_error(::writev(as_int(x[0]), as_pointer<const iovec>(x[1]), as_int(x[2])));

        case nr::exit:
            apkrt::threads::exit_current(as_int(x[0]));
        case nr::exit_group:
            // ADR-0019: one program per process, loaded code cannot be unloaded.
            apkrt::process::exit(as_int(x[0]));
        case nr::clone:
            return sys_clone(frame, x[0], x[1], x[2], x[3], x[4]);
        case nr::set_tid_address:
            return sys_set_tid_address(x[0]);
        case nr::gettid:
            return sys_gettid();
        case nr::getpid:
            return ::getpid();
        case nr::sched_yield:
            ::sched_yield();
            return 0;

        case nr::getppid: return ::getppid();
        case nr::getuid: return ::getuid();
        case nr::geteuid: return ::geteuid();
        case nr::getgid: return ::getgid();
        case nr::getegid: return ::getegid();

        case nr::mmap:
            return sys_mmap(x[0], x[1], as_int(x[2]), as_int(x[3]), as_int(x[4]), x[5]);
        case nr::munmap:
            return sys_munmap(frame, x[0], x[1]);
        case nr::mprotect:
            return sys_mprotect(x[0], x[1], as_int(x[2]));
        case nr::madvise:
            return sys_madvise(x[0], x[1], as_int(x[2]));

        case nr::prctl:
            return sys_prctl(as_int(x[0]), x[1]);
        case nr::getrandom:
            return sys_getrandom(x[0], x[1], static_cast<unsigned>(x[2]));

        case nr::read:
            return result_or_error(::read(as_int(x[0]), as_pointer<void>(x[1]), x[2]));
        case nr::readv:
            return result_or_error(::readv(as_int(x[0]), as_pointer<const iovec>(x[1]), as_int(x[2])));
        case nr::pread64:
            return result_or_error(::pread(as_int(x[0]), as_pointer<void>(x[1]), x[2], static_cast<off_t>(x[3])));
        case nr::pwrite64:
            return result_or_error(::pwrite(as_int(x[0]), as_pointer<const void>(x[1]), x[2], static_cast<off_t>(x[3])));
        case nr::preadv:
            return result_or_error(::preadv(as_int(x[0]), as_pointer<const iovec>(x[1]), as_int(x[2]),
                                            static_cast<off_t>(x[3])));
        case nr::pwritev:
            return result_or_error(::pwritev(as_int(x[0]), as_pointer<const iovec>(x[1]), as_int(x[2]),
                                             static_cast<off_t>(x[3])));
        case nr::lseek:
            return sys_lseek(as_int(x[0]), static_cast<std::int64_t>(x[1]), as_int(x[2]));
        case nr::openat:
            return sys_openat(as_int(x[0]), x[1], as_int(x[2]), as_int(x[3]));
        case nr::close:
            return sys_close(as_int(x[0]));
        case nr::dup:
            return result_or_error(::dup(as_int(x[0])));
        case nr::dup3:
            return sys_dup3(as_int(x[0]), as_int(x[1]), as_int(x[2]));
        case nr::pipe2:
            return sys_pipe2(x[0], as_int(x[1]));
        case nr::fcntl:
            return sys_fcntl(as_int(x[0]), as_int(x[1]), x[2]);
        case nr::ioctl:
            return sys_ioctl(as_int(x[0]), static_cast<std::uint32_t>(x[1]), x[2]);
        case nr::faccessat:
            return sys_faccessat(as_int(x[0]), x[1], as_int(x[2]), 0);
        case nr::faccessat2:
            return sys_faccessat(as_int(x[0]), x[1], as_int(x[2]), as_int(x[3]));
        case nr::newfstatat:
            return sys_newfstatat(as_int(x[0]), x[1], x[2], as_int(x[3]));
        case nr::fstat:
            return sys_fstat(as_int(x[0]), x[1]);
        case nr::statfs:
            return sys_statfs(x[0], x[1]);
        case nr::fstatfs:
            return sys_fstatfs(as_int(x[0]), x[1]);
        case nr::getdents64:
            return sys_getdents64(as_int(x[0]), x[1], x[2]);
        case nr::mkdirat:
            return sys_mkdirat(as_int(x[0]), x[1], as_int(x[2]));
        case nr::unlinkat:
            return sys_unlinkat(as_int(x[0]), x[1], as_int(x[2]));
        case nr::renameat2:
            return sys_renameat2(as_int(x[0]), x[1], as_int(x[2]), x[3], static_cast<unsigned>(x[4]));
        case nr::linkat:
            return sys_linkat(as_int(x[0]), x[1], as_int(x[2]), x[3], as_int(x[4]));
        case nr::symlinkat:
            return sys_symlinkat(x[0], as_int(x[1]), x[2]);
        case nr::readlinkat:
            return sys_readlinkat(as_int(x[0]), x[1], x[2], static_cast<std::int64_t>(x[3]));
        case nr::fchmodat:
            return sys_fchmodat(as_int(x[0]), x[1], as_int(x[2]));
        case nr::fchmod:
            return result_or_error(::fchmod(as_int(x[0]), static_cast<mode_t>(x[1] & 07777)));
        case nr::fchownat:
            return sys_fchownat(as_int(x[0]), x[1], static_cast<std::uint32_t>(x[2]), static_cast<std::uint32_t>(x[3]),
                                as_int(x[4]));
        case nr::fchown:
            return result_or_error(::fchown(as_int(x[0]), static_cast<uid_t>(x[1]), static_cast<gid_t>(x[2])));
        case nr::truncate:
            return sys_truncate(x[0], static_cast<std::int64_t>(x[1]));
        case nr::ftruncate:
            return result_or_error(::ftruncate(as_int(x[0]), static_cast<off_t>(x[1])));
        case nr::fsync:
        case nr::fdatasync:
            return result_or_error(::fsync(as_int(x[0])));
        case nr::sync:
            ::sync();
            return 0;
        case nr::utimensat:
            return sys_utimensat(as_int(x[0]), x[1], x[2], as_int(x[3]));
        case nr::getcwd:
            return sys_getcwd(x[0], x[1]);
        case nr::umask:
            return ::umask(static_cast<mode_t>(x[0] & 0777));
        case nr::chdir:
            return sys_chdir(x[0]);
        case nr::fchdir:
            return sys_fchdir(as_int(x[0]));
        case nr::futex:
            return sys_futex(x[0], as_int(x[1]), static_cast<std::uint32_t>(x[2]), x[3],
                             static_cast<std::uint32_t>(x[5]));
        case nr::clock_gettime:
            return sys_clock_gettime(as_int(x[0]), x[1], false);
        case nr::clock_getres:
            return sys_clock_gettime(as_int(x[0]), x[1], true);
        case nr::clock_nanosleep:
            return sys_clock_nanosleep(as_int(x[0]), as_int(x[1]), x[2], x[3]);
        case nr::nanosleep:
            return sys_nanosleep(x[0], x[1]);
        case nr::gettimeofday:
            return sys_gettimeofday(x[0], x[1]);
        case nr::sched_getaffinity:
            return sys_sched_getaffinity(as_int(x[0]), x[1], x[2]);
        case nr::sched_getscheduler:
            return sys_sched_getscheduler(as_int(x[0]));
        case nr::sched_getparam:
            return sys_sched_getparam(as_int(x[0]), x[1]);
        case nr::sigaltstack:
            return sys_sigaltstack(x[0], x[1]);

        case nr::rt_sigprocmask:
            return sys_rt_sigprocmask(as_int(x[0]), x[1], x[2], x[3]);
        case nr::rt_sigaction:
            return sys_rt_sigaction(as_int(x[0]), x[1], x[2], x[3]);
        case nr::kill:
            return sys_kill(as_int(x[0]), as_int(x[1]));
        case nr::tkill:
            return sys_tgkill(-1, as_int(x[0]), as_int(x[1]));
        case nr::tgkill:
            return sys_tgkill(as_int(x[0]), as_int(x[1]), as_int(x[2]));
        case nr::rt_sigqueueinfo:
            return sys_rt_sigqueueinfo(as_int(x[0]), 0, as_int(x[1]), x[2]);
        case nr::rt_tgsigqueueinfo:
            return sys_rt_sigqueueinfo(as_int(x[0]), as_int(x[1]), as_int(x[2]), x[3]);
        case nr::rt_sigpending:
            return sys_rt_sigpending(x[0], x[1]);
        case nr::rt_sigsuspend:
            return sys_rt_sigsuspend(x[0], x[1]);
        case nr::rt_sigtimedwait:
            return sys_rt_sigtimedwait(x[0], x[1], x[2], x[3]);
        case nr::rt_sigreturn: {
            // The handler returned through its restorer: sp is where the kernel put the frame.
            const auto at_syscall = apkrt::context_at_syscall(frame);
            apkrt::GuestContext restored{};
            if (!at_syscall || !apkrt::signals::read_frame(at_syscall->sp, restored)) {
                report("apkrt: rt_sigreturn without a signal frame\n");
                apkrt::process::exit(128 + 11);  // Linux sends SIGSEGV, which ends the process
            }
            apkrt::signals::resume(restored);
        }

        case APKRT_NR_DLOPEN:
        case APKRT_NR_DLSYM:
        case APKRT_NR_DLCLOSE:
        case APKRT_NR_DLERROR:
        case APKRT_NR_DLADDR:
            return apkrt::dl::loader_syscall(frame);  // ld-android.so's calls into the loader (ADR-0025)

        default:
            report_unsupported(x[8]);
            return -linux_errno::enosys;
    }
}

/// APKRT_TRACE_SYSCALLS=1 prints every syscall with its arguments and result on stderr, like strace.
bool tracing() {
    static const bool enabled = [] {
        const char *value = std::getenv("APKRT_TRACE_SYSCALLS");
        return value != nullptr && value[0] == '1';
    }();
    return enabled;
}

/// One line per syscall: `result` is null before the call (exit_group never comes back).
void trace(const apkrt_syscall_frame &frame, const long *result) {
    char line[200];
    int length = std::snprintf(line, sizeof line, "apkrt: syscall %llu (%#llx, %#llx, %#llx, %#llx, %#llx, %#llx)",
                               static_cast<unsigned long long>(frame.x[8]), static_cast<unsigned long long>(frame.x[0]),
                               static_cast<unsigned long long>(frame.x[1]), static_cast<unsigned long long>(frame.x[2]),
                               static_cast<unsigned long long>(frame.x[3]), static_cast<unsigned long long>(frame.x[4]),
                               static_cast<unsigned long long>(frame.x[5]));
    if (length <= 0 || static_cast<std::size_t>(length) >= sizeof line) return;
    const int tail = result != nullptr
                         ? std::snprintf(line + length, sizeof line - static_cast<std::size_t>(length), " = %ld\n", *result)
                         : std::snprintf(line + length, sizeof line - static_cast<std::size_t>(length), " ...\n");
    if (tail > 0) length += tail;
    static_cast<void>(::write(STDERR_FILENO, line, std::min(static_cast<std::size_t>(length), sizeof line - 1)));
}

}  // namespace

extern "C" void *apkrt_syscall_dispatch(apkrt_syscall_frame *frame) {
    // errno belongs to the host: keep the host's value, whatever the syscall did to it.
    const int saved_errno = errno;
    // The entry has marked the thread as in the runtime (signals.h); guest signals wait for the exit,
    // which may restart the syscall with its first argument.
    frame->x[31] = frame->x[0];
    if (tracing()) trace(*frame, nullptr);
    const long result = dispatch(*frame);
    if (tracing()) trace(*frame, &result);
    // A write to a pipe or socket nobody reads: Linux raises SIGPIPE along with EPIPE.
    if (result == -linux_errno::epipe && is_write(frame->x[8])) {
        apkrt::signals::LinuxSiginfo info{};
        info.signo = 13;  // SIGPIPE
        info.fields.sender.pid = ::getpid();
        info.fields.sender.uid = ::getuid();
        apkrt::signals::send_to_thread(static_cast<std::int32_t>(sys_gettid()), 13, info);
    }
    frame->x[0] = static_cast<std::uint64_t>(result);
    errno = saved_errno;
    return apkrt::signals::at_syscall_exit();
}

extern "C" void apkrt_syscall_deliver(apkrt_syscall_frame *frame) {
    apkrt::signals::deliver_at_syscall_exit(*frame);
}
