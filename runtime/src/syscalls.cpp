// The Linux syscall layer (ADR-0009, ARCHITECTURE.md 4.5): Linux AArch64 syscall numbers, argument
// conventions, errno values and structures in, Darwin calls out. Numbers are never passed through
// to Darwin. This file has the dispatcher and the small syscalls; memory, files, threads, signals and
// futex have files of their own (sys_*.cpp, syscalls.h). At the end of each syscall, guest signals that
// became deliverable are delivered (signals.h). Everything unknown answers -ENOSYS and is reported
// once on stderr, so the gaps show up while programs are brought up.
#include "APKRuntime/guest_interface.h"
#include "audio.h"
#include "channels.h"
#include "descriptors.h"
#include "dl.h"
#include "guest_context.h"
#include "hardware_buffers.h"
#include "video_decoders.h"
#include "media_players.h"
#include "sensor_syscalls.h"
#include "location_syscalls.h"
#include "audio_codec.h"
#include "audio_decode.h"
#include "battery.h"
#include "network.h"
#include "host_calls.h"
#include "identity.h"
#include "loader_syscalls.h"
#include "memfd.h"
#include "proc.h"
#include "process_exit.h"
#include "session.h"
#include "signals.h"
#include "sockets.h"
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
        case ENOTCAPABLE: return linux_errno::eacces;  // a link or ".." out of its mount (ADR-0052)
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
        case ESOCKTNOSUPPORT: return linux_errno::esocktnosupport;
        case EPFNOSUPPORT: return linux_errno::epfnosupport;
        case ENOTSUP:
        case EOPNOTSUPP: return linux_errno::eopnotsupp;
        case EAFNOSUPPORT: return linux_errno::eafnosupport;
        case EADDRINUSE: return linux_errno::eaddrinuse;
        case EADDRNOTAVAIL: return linux_errno::eaddrnotavail;
        case ENETDOWN: return linux_errno::enetdown;
        case ENETUNREACH: return linux_errno::enetunreach;
        case ENETRESET: return linux_errno::enetreset;
        case ECONNABORTED: return linux_errno::econnaborted;
        case ECONNRESET: return linux_errno::econnreset;
        case ENOBUFS: return linux_errno::enobufs;
        case EISCONN: return linux_errno::eisconn;
        case ENOTCONN: return linux_errno::enotconn;
        case ESHUTDOWN: return linux_errno::eshutdown;
        case ETOOMANYREFS: return linux_errno::etoomanyrefs;
        case ETIMEDOUT: return linux_errno::etimedout;
        case ECONNREFUSED: return linux_errno::econnrefused;
        case EHOSTDOWN: return linux_errno::ehostdown;
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
    info.fields.sender.uid = app_uid;
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
constexpr std::uint64_t fallocate = 47;
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
constexpr std::uint64_t sendfile = 71;
constexpr std::uint64_t pselect6 = 72;
constexpr std::uint64_t ppoll = 73;
constexpr std::uint64_t readlinkat = 78;
constexpr std::uint64_t newfstatat = 79;
constexpr std::uint64_t fstat = 80;
constexpr std::uint64_t sync = 81;
constexpr std::uint64_t fsync = 82;
constexpr std::uint64_t fdatasync = 83;
constexpr std::uint64_t utimensat = 88;
constexpr std::uint64_t capget = 90;
constexpr std::uint64_t capset = 91;
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
constexpr std::uint64_t sched_setaffinity = 122;
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
constexpr std::uint64_t setpriority = 140;
constexpr std::uint64_t getpriority = 141;
constexpr std::uint64_t getresuid = 148;
constexpr std::uint64_t getresgid = 150;
constexpr std::uint64_t uname = 160;
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
constexpr std::uint64_t sysinfo = 179;
constexpr std::uint64_t socket = 198;
constexpr std::uint64_t socketpair = 199;
constexpr std::uint64_t bind = 200;
constexpr std::uint64_t listen = 201;
constexpr std::uint64_t accept = 202;
constexpr std::uint64_t connect = 203;
constexpr std::uint64_t getsockname = 204;
constexpr std::uint64_t getpeername = 205;
constexpr std::uint64_t sendto = 206;
constexpr std::uint64_t recvfrom = 207;
constexpr std::uint64_t setsockopt = 208;
constexpr std::uint64_t getsockopt = 209;
constexpr std::uint64_t shutdown = 210;
constexpr std::uint64_t sendmsg = 211;
constexpr std::uint64_t recvmsg = 212;
constexpr std::uint64_t munmap = 215;
constexpr std::uint64_t mremap = 216;
constexpr std::uint64_t clone = 220;
constexpr std::uint64_t mmap = 222;
constexpr std::uint64_t mprotect = 226;
constexpr std::uint64_t msync = 227;
constexpr std::uint64_t madvise = 233;
constexpr std::uint64_t rt_tgsigqueueinfo = 240;
constexpr std::uint64_t accept4 = 242;
constexpr std::uint64_t recvmmsg = 243;
constexpr std::uint64_t prlimit64 = 261;
constexpr std::uint64_t sendmmsg = 269;
constexpr std::uint64_t process_vm_readv = 270;
constexpr std::uint64_t renameat2 = 276;
constexpr std::uint64_t getrandom = 278;
constexpr std::uint64_t memfd_create = 279;
constexpr std::uint64_t userfaultfd = 282;
constexpr std::uint64_t membarrier = 283;
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

/// The device's active CPUs as Linux's CPU mask: one bit each, up to 64.
std::uint64_t active_cpu_mask() {
    int cpus = 1;
    std::size_t length = sizeof cpus;
    if (::sysctlbyname("hw.activecpu", &cpus, &length, nullptr, 0) != 0 || cpus < 1) cpus = 1;
    return cpus >= 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << cpus) - 1;
}

long sys_sched_getaffinity(int pid, std::uint64_t size, std::uint64_t mask) {
    // Linux writes the kernel's CPU mask, 8 bytes up to 64 CPUs, and returns its size.
    constexpr std::uint64_t kernel_mask_size = 8;
    if (pid != 0 && pid != ::getpid()) return -linux_errno::esrch;
    if (size < kernel_mask_size) return -linux_errno::einval;
    *as_pointer<std::uint64_t>(mask) = active_cpu_mask();
    return static_cast<long>(kernel_mask_size);
}

/// Whether `id` is 0, the process or one of its guest threads: what a call that takes a thread ID
/// accepts.
bool names_own_thread(int id) {
    if (id == 0 || id == ::getpid()) return true;
    bool found = false;
    apkrt::threads::find(id, [&](apkrt::threads::GuestThread &) { found = true; });
    return found;
}

/// Unity pins its threads to cores and sets their priorities (ADR-0033, decision 8). Both succeed
/// as on Android, and guest threads run where and as the host schedules them: the cores are kept
/// nowhere, a nice value only for getpriority.
long sys_sched_setaffinity(int pid, std::uint64_t size, std::uint64_t mask) {
    if (!names_own_thread(pid)) return -linux_errno::esrch;
    std::uint64_t wanted = 0;
    std::memcpy(&wanted, as_pointer<const void>(mask), std::min<std::uint64_t>(size, sizeof wanted));
    return (wanted & active_cpu_mask()) != 0 ? 0 : -linux_errno::einval;  // no CPU it could run on
}

/// The first thread's nice value where no guest thread has the process's ID: in tests, and for host
/// threads, which answer as the first thread (sys_gettid).
std::atomic<int> first_thread_nice{0};

/// Calls `use` with the nice value of every thread that setpriority's `which` and `who` name, while
/// none can end: PRIO_PROCESS a thread (0 the caller, the process's ID the first thread), PRIO_PGRP
/// and PRIO_USER (0, or the process's own group or the app's uid) all of them. 0, or Linux's errno.
template <typename Use>
long with_nice_values(int which, int who, Use use) {
    constexpr int prio_process = 0, prio_pgrp = 1, prio_user = 2;
    if (which < prio_process || which > prio_user) return -linux_errno::einval;
    const int pid = ::getpid();
    if (which == prio_process) {
        const int tid = who == 0 ? static_cast<int>(sys_gettid()) : who;
        bool found = false;
        apkrt::threads::find(tid, [&](apkrt::threads::GuestThread &thread) {
            found = true;
            use(thread.nice);
        });
        if (!found && tid == pid) {
            found = true;
            use(first_thread_nice);
        }
        return found ? 0 : -linux_errno::esrch;
    }
    const int own = which == prio_pgrp ? pid : static_cast<int>(::apkrt::app_uid);
    if (who != 0 && who != own) return -linux_errno::esrch;
    bool first = false;
    apkrt::threads::for_each([&](apkrt::threads::GuestThread &thread) {
        first = first || thread.tid == pid;
        use(thread.nice);
    });
    if (!first) use(first_thread_nice);
    return 0;
}

/// Linux's per-thread nice values (setpriority(2)), clamped to -20..19. Android's init gives every
/// process RLIMIT_NICE 40, so an app may set any of them, below its own as well.
long sys_setpriority(int which, int who, int nice) {
    const int value = std::clamp(nice, -20, 19);
    return with_nice_values(which, who, [&](std::atomic<int> &each) { each.store(value); });
}

/// The nice value, as the kernel answers it: 20 - nice, so that no answer is negative (bionic's
/// getpriority turns it back); for several threads, the highest priority among them. ART's thread
/// dumps and Process.getThreadPriority ask for it.
long sys_getpriority(int which, int who) {
    int lowest = 19;
    const long checked =
        with_nice_values(which, who, [&](std::atomic<int> &each) { lowest = std::min(lowest, each.load()); });
    return checked < 0 ? checked : 20 - lowest;
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

/// uname: Linux and Android 15's kernel, with the strings of the SDK's Android 15 emulator image
/// (kernel-ranchu of android-35). ART's read-barrier build checks for 5.13 or newer, and apps read it.
/// The emulator's release ends in -4k for its 4 KB pages; the guest's pages are 16 KB, as getpagesize
/// says, and a GKI kernel with 16 KB pages ends in -16k [inferred].
long sys_uname(std::uint64_t buffer) {
    struct {
        char sysname[65], nodename[65], release[65], version[65], machine[65], domainname[65];
    } names{};
    constexpr auto put = [](char (&field)[65], std::string_view text) { text.copy(field, sizeof field - 1); };
    put(names.sysname, "Linux");
    put(names.nodename, "localhost");
    put(names.release, "6.6.30-android15-8-gdd9c02ccfe27-ab11987101-16k");
    put(names.version, "#1 SMP PREEMPT Tue Jun 18 20:50:32 UTC 2024");
    put(names.machine, "aarch64");
    put(names.domainname, "(none)");
    std::memcpy(as_pointer<void>(buffer), &names, sizeof names);
    return 0;
}

// Capabilities (include/uapi/linux/capability.h): a header, then one set (version 1) or two of 32 bits.
constexpr std::uint32_t capability_version_1 = 0x1998'0330;
constexpr std::uint32_t capability_version_2 = 0x2007'1026;
constexpr std::uint32_t capability_version_3 = 0x2008'0522;
struct CapabilityHeader {
    std::uint32_t version;
    std::int32_t pid;
};
struct CapabilitySet {
    std::uint32_t effective, permitted, inheritable;
};

/// How many sets `header`'s version carries, as cap_validate_magic finds it; for a version Linux does
/// not know, 0, and the kernel's own version goes into the header for the next try.
std::size_t capability_sets(CapabilityHeader &header) {
    switch (header.version) {
        case capability_version_1: return 1;
        case capability_version_2:
        case capability_version_3: return 2;
        default:
            header.version = capability_version_3;
            return 0;
    }
}

/// capget and capset as for an app, which has no capabilities (ADR-0033, decision 8): libcap asks.
long sys_capget(std::uint64_t header_address, std::uint64_t sets_address) {
    auto &header = *as_pointer<CapabilityHeader>(header_address);
    const std::size_t sets = capability_sets(header);
    if (sets_address == 0) return 0;  // asks for the version, whichever it gave
    if (sets == 0 || header.pid < 0) return -linux_errno::einval;
    if (!names_own_thread(header.pid)) return -linux_errno::esrch;
    std::memset(as_pointer<void>(sets_address), 0, sets * sizeof(CapabilitySet));
    return 0;
}

long sys_capset(std::uint64_t header_address, std::uint64_t sets_address) {
    auto &header = *as_pointer<CapabilityHeader>(header_address);
    const std::size_t sets = capability_sets(header);
    if (sets == 0) return -linux_errno::einval;
    if (header.pid != 0 && header.pid != sys_gettid()) return -linux_errno::eperm;  // only the caller's own
    const auto *wanted = as_pointer<const CapabilitySet>(sets_address);
    for (std::size_t index = 0; index < sets; ++index) {
        if (wanted[index].effective != 0 || wanted[index].permitted != 0 || wanted[index].inheritable != 0) {
            return -linux_errno::eperm;  // more than it has
        }
    }
    return 0;
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

bool is_write(std::uint64_t number) {
    return number == nr::write || number == nr::writev || number == nr::pwrite64 || number == nr::pwritev;
}

/// The guest's stack holds this frame at every syscall, where Linux takes none of it: UBSan's checks give the cases a
/// frame of 16 KiB at -O0, more than ART's gap of 8 KiB for a stack overflow (107-int-math2 in the sanitizer build).
/// The cases only pass the arguments on; the functions they call keep their checks.
__attribute__((no_sanitize("undefined"))) long dispatch(const apkrt_syscall_frame &frame) {
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
            apkrt::process::note_exit(as_int(x[0]), x[29]);
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
        // The app uid, also as the gid (ADR-0039 decision 3).
        case nr::getuid:
        case nr::geteuid:
        case nr::getgid:
        case nr::getegid:
            return apkrt::app_uid;
        case nr::getresuid:
        case nr::getresgid:
            if (x[0] == 0 || x[1] == 0 || x[2] == 0) return -linux_errno::efault;
            for (int index = 0; index < 3; ++index) *reinterpret_cast<std::uint32_t *>(x[index]) = apkrt::app_uid;
            return 0;

        case nr::mmap:
            return sys_mmap(x[0], x[1], as_int(x[2]), as_int(x[3]), as_int(x[4]), x[5]);
        case nr::munmap:
            return sys_munmap(frame, x[0], x[1]);
        case nr::mremap:
            return sys_mremap(frame, x[0], x[1], x[2], as_int(x[3]), x[4]);
        case nr::mprotect:
            return sys_mprotect(x[0], x[1], as_int(x[2]));
        case nr::msync:
            return sys_msync(x[0], x[1], as_int(x[2]));
        case nr::madvise:
            return sys_madvise(x[0], x[1], as_int(x[2]));
        case nr::process_vm_readv:
            return sys_process_vm_readv(as_int(x[0]), x[1], x[2], x[3], x[4], x[5]);

        case nr::prctl:
            return sys_prctl(as_int(x[0]), x[1]);
        case nr::uname:
            return sys_uname(x[0]);
        case nr::sysinfo:
            return ::apkrt::proc::sysinfo(x[0]);  // Darwin has a struct proc
        case nr::capget:
            return sys_capget(x[0], x[1]);
        case nr::capset:
            return sys_capset(x[0], x[1]);
        case nr::getrlimit:
            return sys_prlimit64(0, as_int(x[0]), 0, x[1]);
        case nr::setrlimit:
            return sys_prlimit64(0, as_int(x[0]), x[1], 0);
        case nr::prlimit64:
            return sys_prlimit64(as_int(x[0]), as_int(x[1]), x[2], x[3]);
        case nr::getrandom:
            return sys_getrandom(x[0], x[1], static_cast<unsigned>(x[2]));
        case nr::memfd_create:
            return apkrt::memfd::create(x[0], static_cast<unsigned>(x[1]));
        // As a kernel built without them answers, without a report (ADR-0033 decision 8): ART asks for both at
        // every start, userfaultfd for its CMC collector, which our build leaves out, and membarrier for its fault
        // handler, which then only warns.
        case nr::userfaultfd:
        case nr::membarrier:
            return -linux_errno::enosys;
        // Sockets (ADR-0054, sys_sockets.cpp).
        case nr::socket:
            return sys_socket(as_int(x[0]), as_int(x[1]), as_int(x[2]));
        case nr::socketpair:
            return sys_socketpair(as_int(x[0]), as_int(x[1]), as_int(x[2]), x[3]);
        case nr::bind:
            return sys_bind(as_int(x[0]), x[1], static_cast<std::uint32_t>(x[2]));
        case nr::listen:
            return sys_listen(as_int(x[0]), as_int(x[1]));
        case nr::accept:
            return sys_accept4(as_int(x[0]), x[1], x[2], 0);
        case nr::accept4:
            return sys_accept4(as_int(x[0]), x[1], x[2], as_int(x[3]));
        case nr::connect:
            return sys_connect(as_int(x[0]), x[1], static_cast<std::uint32_t>(x[2]));
        case nr::getsockname:
            return sys_getsockname(as_int(x[0]), x[1], x[2]);
        case nr::getpeername:
            return sys_getpeername(as_int(x[0]), x[1], x[2]);
        case nr::sendto:
            return sys_sendto(as_int(x[0]), x[1], x[2], as_int(x[3]), x[4], static_cast<std::uint32_t>(x[5]));
        case nr::recvfrom:
            return sys_recvfrom(as_int(x[0]), x[1], x[2], as_int(x[3]), x[4], x[5]);
        case nr::setsockopt:
            return sys_setsockopt(as_int(x[0]), as_int(x[1]), as_int(x[2]), x[3], static_cast<std::uint32_t>(x[4]));
        case nr::getsockopt:
            return sys_getsockopt(as_int(x[0]), as_int(x[1]), as_int(x[2]), x[3], x[4]);
        case nr::shutdown:
            return sys_shutdown(as_int(x[0]), as_int(x[1]));
        case nr::sendmsg:
            return sys_sendmsg(as_int(x[0]), x[1], as_int(x[2]));
        case nr::recvmsg:
            return sys_recvmsg(as_int(x[0]), x[1], as_int(x[2]));
        case nr::sendmmsg:
            return sys_sendmmsg(as_int(x[0]), x[1], static_cast<std::uint32_t>(x[2]), as_int(x[3]));
        case nr::recvmmsg:
            return sys_recvmmsg(as_int(x[0]), x[1], static_cast<std::uint32_t>(x[2]), as_int(x[3]), x[4]);

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
        case nr::sendfile:
            return sys_sendfile(as_int(x[0]), as_int(x[1]), x[2], x[3]);
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
            return sys_readlinkat(as_int(x[0]), x[1], x[2], as_int(x[3]));  // int bufsiz
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
        case nr::fallocate:
            return sys_fallocate(as_int(x[0]), as_int(x[1]), static_cast<std::int64_t>(x[2]),
                                 static_cast<std::int64_t>(x[3]));
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
            return sys_futex(x[0], as_int(x[1]), static_cast<std::uint32_t>(x[2]), x[3], x[4],
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
        case nr::sched_setaffinity:
            return sys_sched_setaffinity(as_int(x[0]), x[1], x[2]);
        case nr::setpriority:
            return sys_setpriority(as_int(x[0]), as_int(x[1]), as_int(x[2]));
        case nr::getpriority:
            return sys_getpriority(as_int(x[0]), as_int(x[1]));
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
        case APKRT_NR_DLOPEN_IN:
        case APKRT_NR_CREATE_NAMESPACE:
        case APKRT_NR_LINK_NAMESPACES:
        case APKRT_NR_EXPORTED_NAMESPACE:
        case APKRT_NR_DLOPEN_NAMESPACE:
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
        case APKRT_NR_WINDOW_SIZE:
            return apkrt::windows::size(x[0], x[1]);  // ADR-0038 c)
        case APKRT_NR_WINDOW_LOCK:
            return apkrt::windows::lock_buffer(x[0], x[1]);  // ADR-0043, decision 3
        case APKRT_NR_WINDOW_POST:
            return apkrt::windows::post_buffer(x[0]);
        case APKRT_NR_HARDWARE_BUFFER:
            return apkrt::hardware_buffers::call(x[0], x[1]);  // ADR-0058 a)
        case APKRT_NR_VIDEO_DECODER:
            return apkrt::video_decoders::call(x[0], x[1]);  // ADR-0060 e2b3
        case APKRT_NR_MEDIA_PLAYER:
            return apkrt::media_players::call(x[0], x[1]);  // ADR-0060 e2e1
        case APKRT_NR_AUDIO_DECODE:
            return apkrt::audio_decode::call(x[0]);  // the SoundPool plan's step 3
        case APKRT_NR_AUDIO_CODEC:
            return apkrt::audio_codec::call(x[0], x[1]);  // ADR-0060 e2f
        case APKRT_NR_SENSOR:
            return apkrt::sensors::call(x[0], x[1]);  // ADR-0061
        case APKRT_NR_LOCATION:
            return apkrt::location::call(x[0], x[1], x[2]);  // ADR-0063
        case APKRT_NR_BATTERY:
            return apkrt::battery::call(x[0]);  // the battery transport (App gap-1, 2026-10-29)
        case APKRT_NR_NETWORK:
            return apkrt::network::call(x[0]);  // the network transport (ADR-0054 step d))
        case APKRT_NR_SESSION_CONFIGURATION:
            return apkrt::session::configuration(x[0]);  // ADR-0039, config-test
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
        // The audio output streams (ADR-0036, decisions 5 and 6).
        case APKRT_NR_AUDIO_OPEN:
            return apkrt::audio::open(x[0]);
        case APKRT_NR_AUDIO_CLOSE:
            return apkrt::audio::close(x[0]);
        case APKRT_NR_AUDIO_CONTROL:
            return apkrt::audio::control(x[0], x[1], static_cast<std::int64_t>(x[2]));
        case APKRT_NR_AUDIO_WRITE:
            return apkrt::audio::write(x[0], x[1], static_cast<std::int64_t>(x[2]), static_cast<std::int64_t>(x[3]));
        case APKRT_NR_AUDIO_STATUS:
            return apkrt::audio::status(x[0], x[1]);
        case APKRT_NR_AUDIO_WAIT:
            return apkrt::audio::wait(x[0], static_cast<std::uint32_t>(x[1]), static_cast<std::int64_t>(x[2]));
        case APKRT_NR_AUDIO_JOIN:
            return apkrt::audio::join(x[0], x[1]);

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
    // the count of a write that got some bytes out first, sys_files.cpp), but not for a SOCK_SEQPACKET socket.
    if (result == -linux_errno::epipe && is_write(frame->x[8]) &&
        apkrt::sockets::raises_sigpipe(static_cast<int>(static_cast<std::int32_t>(frame->x[31])))) {
        raise_sigpipe();
    }
    frame->x[0] = static_cast<std::uint64_t>(result);
    errno = saved_errno;
    return apkrt::signals::at_syscall_exit();
}

extern "C" void apkrt_syscall_deliver(apkrt_syscall_frame *frame) {
    apkrt::signals::deliver_at_syscall_exit(*frame);
}
