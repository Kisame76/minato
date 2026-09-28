// The Linux syscall layer (ADR-0009, ARCHITECTURE.md 4.5): Linux AArch64 syscall numbers, argument
// conventions, errno values and structures in, Darwin calls out. Numbers are never passed through
// to Darwin. This file has the dispatcher and the small syscalls; memory, files, threads, signals and
// futex have files of their own (sys_*.cpp, syscalls.h). At the end of each syscall, guest signals that
// became deliverable are delivered (signals.h). Everything unknown answers -ENOSYS and is reported
// once on stderr, so the gaps show up while programs are brought up.
#include "APKRuntime/guest_interface.h"
#include "channels.h"
#include "descriptors.h"
#include "dl.h"
#include "guest_context.h"
#include "host_calls.h"
#include "loader_syscalls.h"
#include "process_exit.h"
#include "session.h"
#include "signals.h"
#include "syscalls.h"
#include "threads.h"
#include "windows.h"

#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
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

void raise_sigpipe() {
    signals::LinuxSiginfo info{};
    info.signo = 13;  // SIGPIPE
    info.fields.sender.pid = ::getpid();
    info.fields.sender.uid = ::getuid();
    signals::send_to_thread(static_cast<std::int32_t>(sys_gettid()), 13, info);
}

std::int64_t monotonic_now() {
    return static_cast<std::int64_t>(clock_gettime_nsec_np(CLOCK_UPTIME_RAW));
}

std::int64_t nanoseconds_of(const timespec &time) {
    constexpr std::int64_t second = 1'000'000'000;
    if (time.tv_sec > (INT64_MAX - time.tv_nsec) / second) return INT64_MAX;
    return static_cast<std::int64_t>(time.tv_sec) * second + time.tv_nsec;
}

std::int64_t deadline_after(std::int64_t nanoseconds) {
    const std::int64_t now = monotonic_now();
    return nanoseconds > INT64_MAX - now ? INT64_MAX : now + nanoseconds;
}

}  // namespace apkrt::sys

namespace {

using namespace apkrt::sys;

// Linux AArch64 syscall numbers (include/uapi/asm-generic/unistd.h).
namespace nr {
constexpr std::uint64_t getcwd = 17;
constexpr std::uint64_t eventfd2 = 19;
constexpr std::uint64_t epoll_create1 = 20;
constexpr std::uint64_t epoll_ctl = 21;
constexpr std::uint64_t epoll_pwait = 22;
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
constexpr std::uint64_t pselect6 = 72;
constexpr std::uint64_t ppoll = 73;
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
constexpr std::uint64_t getrlimit = 163;
constexpr std::uint64_t setrlimit = 164;
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
constexpr std::uint64_t socket = 198;
constexpr std::uint64_t munmap = 215;
constexpr std::uint64_t clone = 220;
constexpr std::uint64_t mmap = 222;
constexpr std::uint64_t mprotect = 226;
constexpr std::uint64_t madvise = 233;
constexpr std::uint64_t rt_tgsigqueueinfo = 240;
constexpr std::uint64_t prlimit64 = 261;
constexpr std::uint64_t renameat2 = 276;
constexpr std::uint64_t getrandom = 278;
constexpr std::uint64_t faccessat2 = 439;
constexpr std::uint64_t epoll_pwait2 = 441;
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

/// Sleeps until `deadline` (monotonic_now's clock) in wait_until, which a signal ends: then EINTR,
/// with what is left written to `remaining` (if not 0).
long sleep_until(std::int64_t deadline, std::uint64_t remaining) {
    if (wait_until(apkrt::signals::current(), [] { return false; }, deadline) != WaitEnd::signal) return 0;
    if (remaining != 0) {
        const std::int64_t left = std::max<std::int64_t>(deadline - monotonic_now(), 0);
        *as_pointer<timespec>(remaining) = {static_cast<time_t>(left / 1'000'000'000),
                                            static_cast<long>(left % 1'000'000'000)};
    }
    return -linux_errno::eintr;
}

long sys_nanosleep(std::uint64_t request, std::uint64_t remaining) {
    const auto duration = *as_pointer<const timespec>(request);
    if (!valid(duration)) return -linux_errno::einval;
    return sleep_until(deadline_after(nanoseconds_of(duration)), remaining);
}

/// Darwin has no clock_nanosleep: a relative sleep is nanosleep, an absolute one sleeps for the
/// difference to the clock's current time, again after each wait, as Linux would.
long sys_clock_nanosleep(int linux_clock, int flags, std::uint64_t request, std::uint64_t remaining) {
    constexpr int timer_abstime = 1;
    const auto clock = to_darwin_clock(linux_clock);
    if (!clock || *clock == CLOCK_THREAD_CPUTIME_ID) return -linux_errno::einval;
    const auto time = *as_pointer<const timespec>(request);
    if (!valid(time)) return -linux_errno::einval;
    if ((flags & timer_abstime) == 0) return sleep_until(deadline_after(nanoseconds_of(time)), remaining);
    const std::int64_t target = nanoseconds_of(time);
    while (true) {
        const auto now = static_cast<std::int64_t>(clock_gettime_nsec_np(*clock));
        if (now >= target) return 0;
        // No remaining time for TIMER_ABSTIME.
        if (const long result = sleep_until(deadline_after(target - now), 0); result != 0) return result;
    }
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

/// The guest's RLIMIT_STACK, at first Android's: 8 MiB soft and no hard limit (RLIM_INFINITY)
/// (ADR-0035). The main thread's stack stays 8 MiB whatever the guest sets (program.cpp), where Linux
/// would let a higher soft limit grow it.
std::mutex stack_limit_mutex;
apkrt::descriptors::Limits stack_limit = {8 << 20, ~std::uint64_t{0}};  // guarded by stack_limit_mutex

/// prlimit64, and getrlimit and setrlimit, which are prlimit64 of the calling process. struct rlimit
/// and struct rlimit64 are two 64-bit words on arm64. RLIMIT_NOFILE is the descriptor table's
/// (ADR-0027) and RLIMIT_STACK Android's (ADR-0035); the other resources are not supported yet.
long sys_prlimit64(int pid, int resource, std::uint64_t new_address, std::uint64_t old_address) {
    constexpr int rlimit_stack = 3, rlimit_nofile = 7;
    using apkrt::descriptors::Limits;
    if (pid != 0 && pid != ::getpid()) return -linux_errno::esrch;
    if (resource != rlimit_stack && resource != rlimit_nofile) {
        char message[80];
        std::snprintf(message, sizeof message, "apkrt: resource limit %d is not supported yet\n", resource);
        report(message);
        return -linux_errno::enosys;
    }
    std::optional<Limits> requested;
    if (new_address != 0) requested = *as_pointer<const Limits>(new_address);
    Limits previous{};
    if (resource == rlimit_stack) {
        // setrlimit's rules, as descriptors::set_limits has them for RLIMIT_NOFILE.
        const std::lock_guard lock(stack_limit_mutex);
        previous = stack_limit;
        if (requested) {
            if (requested->soft > requested->hard) return -linux_errno::einval;
            if (requested->hard > stack_limit.hard) return -linux_errno::eperm;
            stack_limit = *requested;
        }
    } else {
        previous = apkrt::descriptors::limits();
        if (requested) {
            if (const long result = apkrt::descriptors::set_limits(*requested); result != 0) return result;
        }
    }
    if (old_address != 0) *as_pointer<Limits>(old_address) = previous;
    return 0;
}

// getrandom flags (include/uapi/linux/random.h): GRND_NONBLOCK, GRND_RANDOM, GRND_INSECURE.
constexpr unsigned grnd_known = 0x1 | 0x2 | 0x4;

long sys_getrandom(std::uint64_t buffer, std::uint64_t length, unsigned flags) {
    if (flags & ~grnd_known) return -linux_errno::einval;
    // The kernel's generator is always seeded on Darwin, so no flag can make this block.
    arc4random_buf(as_pointer<void>(buffer), length);
    return static_cast<long>(length);
}

// --- Sockets ----------------------------------------------------------------------------------------

/// No sockets yet. bionic's async_safe_log opens logd's socket with exactly this call before every
/// message and writes to stderr when it fails. There is no logd here, so the call is refused quietly,
/// as by a kernel without AF_UNIX: the report would reach the process's stderr past the guest's own
/// (ADR-0035). Every other socket call is reported.
long sys_socket(int domain, int type, int protocol) {
    constexpr int af_unix = 1, sock_dgram = 2, sock_nonblock = 04000, sock_cloexec = 02000000;
    if (domain == af_unix && type == (sock_dgram | sock_cloexec | sock_nonblock) && protocol == 0) {
        return -linux_errno::eafnosupport;
    }
    report_unsupported(nr::socket);
    return -linux_errno::enosys;
}

bool is_write(std::uint64_t number) {
    return number == nr::write || number == nr::writev || number == nr::pwrite64 || number == nr::pwritev;
}

long dispatch(const apkrt_syscall_frame &frame) {
    const auto *x = frame.x;
    switch (x[8]) {
        case nr::write:
            return sys_write(as_int(x[0]), x[1], x[2]);
        case nr::writev:
            return sys_writev(as_int(x[0]), x[1], as_int(x[2]));

        case nr::exit:
            apkrt::threads::exit_current(as_int(x[0]));
        case nr::exit_group:
            // ADR-0019: one program per process, loaded code cannot be unloaded; ADR-0028: with a
            // session, the process lives on after the program.
            apkrt::process::end(as_int(x[0]));
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
        case nr::getrlimit:
            return sys_prlimit64(0, as_int(x[0]), 0, x[1]);
        case nr::setrlimit:
            return sys_prlimit64(0, as_int(x[0]), x[1], 0);
        case nr::prlimit64:
            return sys_prlimit64(as_int(x[0]), as_int(x[1]), x[2], x[3]);
        case nr::getrandom:
            return sys_getrandom(x[0], x[1], static_cast<unsigned>(x[2]));
        case nr::socket:
            return sys_socket(as_int(x[0]), as_int(x[1]), as_int(x[2]));

        case nr::read:
            return sys_read(as_int(x[0]), x[1], x[2]);
        case nr::readv:
            return sys_readv(as_int(x[0]), x[1], as_int(x[2]));
        case nr::pread64:
            return sys_pread64(as_int(x[0]), x[1], x[2], static_cast<std::int64_t>(x[3]));
        case nr::pwrite64:
            return sys_pwrite64(as_int(x[0]), x[1], x[2], static_cast<std::int64_t>(x[3]));
        case nr::preadv:
            return sys_preadv(as_int(x[0]), x[1], as_int(x[2]), static_cast<std::int64_t>(x[3]));
        case nr::pwritev:
            return sys_pwritev(as_int(x[0]), x[1], as_int(x[2]), static_cast<std::int64_t>(x[3]));
        case nr::lseek:
            return sys_lseek(as_int(x[0]), static_cast<std::int64_t>(x[1]), as_int(x[2]));
        case nr::openat:
            return sys_openat(as_int(x[0]), x[1], as_int(x[2]), as_int(x[3]));
        case nr::close:
            return sys_close(as_int(x[0]));
        case nr::dup:
            return sys_dup(as_int(x[0]));
        case nr::dup3:
            return sys_dup3(as_int(x[0]), as_int(x[1]), as_int(x[2]));
        case nr::pipe2:
            return sys_pipe2(x[0], as_int(x[1]));
        case nr::eventfd2:
            return sys_eventfd2(static_cast<unsigned>(x[0]), as_int(x[1]));
        case nr::epoll_create1:
            return sys_epoll_create1(as_int(x[0]));
        case nr::epoll_ctl:
            return sys_epoll_ctl(as_int(x[0]), as_int(x[1]), as_int(x[2]), x[3]);
        case nr::epoll_pwait:
            return sys_epoll_pwait(as_int(x[0]), x[1], as_int(x[2]), as_int(x[3]), x[4], x[5]);
        case nr::epoll_pwait2:
            return sys_epoll_pwait2(as_int(x[0]), x[1], as_int(x[2]), x[3], x[4], x[5]);
        case nr::ppoll:
            return sys_ppoll(x[0], static_cast<std::uint32_t>(x[1]), x[2], x[3], x[4]);
        case nr::pselect6:
            return sys_pselect6(as_int(x[0]), x[1], x[2], x[3], x[4], x[5]);
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
            return sys_fchmod(as_int(x[0]), as_int(x[1]));
        case nr::fchownat:
            return sys_fchownat(as_int(x[0]), x[1], static_cast<std::uint32_t>(x[2]), static_cast<std::uint32_t>(x[3]),
                                as_int(x[4]));
        case nr::fchown:
            return sys_fchown(as_int(x[0]), static_cast<std::uint32_t>(x[1]), static_cast<std::uint32_t>(x[2]));
        case nr::truncate:
            return sys_truncate(x[0], static_cast<std::int64_t>(x[1]));
        case nr::ftruncate:
            return sys_ftruncate(as_int(x[0]), static_cast<std::int64_t>(x[1]));
        case nr::fsync:
        case nr::fdatasync:
            return sys_fsync(as_int(x[0]));
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
        case APKRT_NR_BIND_HOST_TABLE:
            return apkrt::host_calls::bind_host_table(x[0], x[1], x[2], x[3]);  // a front lib (ADR-0030)
        // The session and the window's gate (ADR-0028, decision 11).
        case APKRT_NR_SESSION_CONNECT:
            return apkrt::session::connect(x[0]);
        case APKRT_NR_SESSION_ANSWER:
            return apkrt::session::answer(x[0]);
        case APKRT_NR_SESSION_REQUEST:
            return apkrt::session::request(x[0], x[1], x[2]);
        case APKRT_NR_WINDOW_GATE:
            return apkrt::windows::gate(x[0], x[1]);
        // The vsync channel (ADR-0036, decision 1).
        case APKRT_NR_VSYNC_CONNECT:
            return apkrt::channels::vsync_connect();
        case APKRT_NR_VSYNC_REQUEST:
            return apkrt::channels::vsync_request(as_int(x[0]));
        case APKRT_NR_VSYNC_READ:
            return apkrt::channels::vsync_read(as_int(x[0]), x[1]);
        case APKRT_NR_INPUT_CONNECT:
            return apkrt::channels::input_connect(x[0]);
        case APKRT_NR_INPUT_READ:
            return apkrt::channels::input_read(as_int(x[0]), x[1]);

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
    // A write to a pipe or socket nobody reads: Linux raises SIGPIPE along with EPIPE (and along with
    // the count of a write that got some bytes out first, sys_files.cpp).
    if (result == -linux_errno::epipe && is_write(frame->x[8])) raise_sigpipe();
    frame->x[0] = static_cast<std::uint64_t>(result);
    errno = saved_errno;
    return apkrt::signals::at_syscall_exit();
}

extern "C" void apkrt_syscall_deliver(apkrt_syscall_frame *frame) {
    apkrt::signals::deliver_at_syscall_exit(*frame);
}
