// Checks the Linux semantics of the syscall layer (ADR-0009): each call goes through the dispatcher
// with Linux numbers, flags and structures, exactly as a guest's svc veneer delivers them.
#include "syscall_frame.h"
#include "vfs.h"

#include "check.h"

#include <fcntl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <set>
#include <string>

namespace {

// Linux AArch64 syscall numbers and constants (asm-generic headers).
constexpr std::uint64_t nr_munmap = 215;
constexpr std::uint64_t nr_mmap = 222;
constexpr std::uint64_t nr_mprotect = 226;
constexpr std::uint64_t nr_madvise = 233;
constexpr std::uint64_t nr_rt_sigaction = 134;
constexpr std::uint64_t nr_rt_sigprocmask = 135;
constexpr std::uint64_t nr_getrandom = 278;
constexpr std::uint64_t nr_fcntl = 25;
constexpr std::uint64_t nr_sched_getscheduler = 120;
constexpr std::uint64_t nr_sched_getparam = 121;
constexpr std::uint64_t nr_sigaltstack = 132;
constexpr std::uint64_t nr_openat = 56;
constexpr std::uint64_t nr_faccessat = 48;
constexpr std::uint64_t nr_faccessat2 = 439;
constexpr std::uint64_t nr_newfstatat = 79;
constexpr std::uint64_t nr_fstat = 80;
constexpr std::uint64_t nr_futex = 98;
constexpr std::uint64_t nr_clock_gettime = 113;
constexpr std::uint64_t nr_sched_getaffinity = 123;
constexpr std::uint64_t nr_prctl = 167;

constexpr std::uint64_t prot_read = 1, prot_write = 2, prot_exec = 4;
constexpr std::uint64_t map_private = 0x02, map_fixed = 0x10, map_anonymous = 0x20, map_stack = 0x20000;
constexpr std::uint64_t map_fixed_noreplace = 0x10'0000, map_hugetlb = 0x40000;
constexpr std::uint64_t madv_dontneed = 4, madv_wipeonfork = 18, madv_hwpoison = 100;
constexpr std::int64_t ebadf = 9, eacces = 13, eexist = 17, einval = 22, esrch = 3, enoent = 2, eagain = 11,
                         etimedout = 110;

constexpr std::uint64_t page = 16 * 1024;

/// Room for a Linux struct termios (60 bytes on arm64).
void *buffer_for_termios() {
    static char buffer[64];
    return buffer;
}

std::int64_t call(std::uint64_t number, std::initializer_list<std::uint64_t> arguments) {
    apkrt_syscall_frame frame{};
    std::size_t index = 0;
    for (const auto argument : arguments) frame.x[index++] = argument;
    frame.x[8] = number;
    apkrt_syscall_dispatch(&frame);
    return static_cast<std::int64_t>(frame.x[0]);
}

std::uint64_t map_anonymous_pages(std::size_t count) {
    return static_cast<std::uint64_t>(call(nr_mmap, {0, count * page, prot_read | prot_write,
                                                     map_private | map_anonymous, ~std::uint64_t{0}, 0}));
}

TEST(mmap_maps_anonymous_memory_the_linux_way) {
    const auto address = static_cast<std::uint64_t>(
        call(nr_mmap, {0, 2 * page, prot_read | prot_write, map_private | map_anonymous | map_stack, ~0ull, 0}));
    CHECK(address % page == 0 && address > 0x1'0000'0000);  // an address, not -errno
    auto *bytes = reinterpret_cast<volatile std::uint8_t *>(address);
    CHECK(bytes[0] == 0 && bytes[2 * page - 1] == 0);
    bytes[page] = 7;
    CHECK(bytes[page] == 7);
    CHECK(call(nr_mprotect, {address, page, prot_read}) == 0);
    CHECK(call(nr_munmap, {address, 2 * page}) == 0);
}

TEST(mmap_never_hands_out_executable_memory) {
    CHECK(call(nr_mmap, {0, page, prot_read | prot_exec, map_private | map_anonymous, ~0ull, 0}) == -eacces);
    const auto address = map_anonymous_pages(1);
    CHECK(call(nr_mprotect, {address, page, prot_read | prot_exec}) == -eacces);
    CHECK(call(nr_munmap, {address, page}) == 0);
}

TEST(mmap_refuses_flags_it_does_not_know) {
    CHECK(call(nr_mmap, {0, page, prot_read, map_private | map_anonymous | map_hugetlb, ~0ull, 0}) == -einval);
    CHECK(call(nr_mmap, {0, page, prot_read, map_anonymous, ~0ull, 0}) == -einval);  // neither private nor shared
    CHECK(call(nr_mmap, {0, page, prot_read | 0x10 /* PROT_BTI */, map_private | map_anonymous, ~0ull, 0}) == -einval);
}

TEST(mmap_fixed_noreplace_does_not_replace) {
    const auto taken = map_anonymous_pages(1);
    CHECK(call(nr_mmap, {taken, page, prot_read, map_private | map_anonymous | map_fixed_noreplace, ~0ull, 0}) ==
          -eexist);
    CHECK(*reinterpret_cast<volatile std::uint8_t *>(taken) == 0);  // still mapped, still ours
    CHECK(call(nr_munmap, {taken, page}) == 0);

    // A free address is used exactly.
    CHECK(call(nr_mmap, {taken, page, prot_read, map_private | map_anonymous | map_fixed_noreplace, ~0ull, 0}) ==
          static_cast<std::int64_t>(taken));
    CHECK(call(nr_mmap, {taken, page, prot_read | prot_write, map_private | map_anonymous | map_fixed, ~0ull, 0}) ==
          static_cast<std::int64_t>(taken));
    CHECK(call(nr_munmap, {taken, page}) == 0);
}

TEST(madvise_dontneed_reads_back_as_zero) {
    // Linux guarantees zero pages after MADV_DONTNEED on private anonymous memory; allocators rely
    // on it. Darwin's MADV_DONTNEED keeps the contents.
    const auto address = map_anonymous_pages(2);
    std::memset(reinterpret_cast<void *>(address), 0x5a, 2 * page);
    CHECK(call(nr_madvise, {address, 2 * page, madv_dontneed}) == 0);
    const auto *bytes = reinterpret_cast<const std::uint8_t *>(address);
    CHECK(bytes[0] == 0 && bytes[page] == 0 && bytes[2 * page - 1] == 0);
    CHECK(call(nr_munmap, {address, 2 * page}) == 0);
}

TEST(madvise_accepts_fork_advice_and_refuses_unknown_advice) {
    const auto address = map_anonymous_pages(1);
    CHECK(call(nr_madvise, {address, page, madv_wipeonfork}) == 0);  // iOS has no fork (F11)
    CHECK(call(nr_madvise, {address, page, madv_hwpoison}) == -einval);
    CHECK(call(nr_munmap, {address, page}) == 0);
}

TEST(rt_sigprocmask_keeps_real_time_signals_in_the_guest_mask) {
    const std::uint64_t sigusr1 = 1ull << (10 - 1);
    const std::uint64_t sigrtmin_plus_3 = 1ull << (35 - 1);  // one of bionic's own
    const std::uint64_t sigkill = 1ull << (9 - 1);
    std::uint64_t old_mask = 0;
    std::uint64_t mask = sigusr1 | sigrtmin_plus_3 | sigkill;
    CHECK(call(nr_rt_sigprocmask, {0 /* SIG_BLOCK */, reinterpret_cast<std::uint64_t>(&mask),
                                   reinterpret_cast<std::uint64_t>(&old_mask), 8}) == 0);
    std::uint64_t now = 0;
    CHECK(call(nr_rt_sigprocmask, {0, 0, reinterpret_cast<std::uint64_t>(&now), 8}) == 0);
    CHECK((now & sigusr1) != 0);
    CHECK((now & sigrtmin_plus_3) != 0);
    CHECK((now & sigkill) == 0);  // cannot be blocked
    CHECK(call(nr_rt_sigprocmask, {2 /* SIG_SETMASK */, reinterpret_cast<std::uint64_t>(&old_mask), 0, 8}) == 0);
    CHECK(call(nr_rt_sigprocmask, {0, 0, reinterpret_cast<std::uint64_t>(&now), 8}) == 0);
    CHECK(now == old_mask);
    CHECK(call(nr_rt_sigprocmask, {0, 0, reinterpret_cast<std::uint64_t>(&now), 4}) == -einval);
}

TEST(rt_sigaction_records_dispositions_of_signals_darwin_lacks) {
    struct {
        std::uint64_t handler, flags, restorer, mask;
    } ignore{1 /* SIG_IGN */, 0, 0, 0}, previous{};
    CHECK(call(nr_rt_sigaction, {36, reinterpret_cast<std::uint64_t>(&ignore), 0, 8}) == 0);
    CHECK(call(nr_rt_sigaction, {36, 0, reinterpret_cast<std::uint64_t>(&previous), 8}) == 0);
    CHECK(previous.handler == 1);
    CHECK(call(nr_rt_sigaction, {9 /* SIGKILL */, reinterpret_cast<std::uint64_t>(&ignore), 0, 8}) == -einval);
    CHECK(call(nr_rt_sigaction, {65, 0, reinterpret_cast<std::uint64_t>(&previous), 8}) == -einval);
}

TEST(getrandom_fills_the_whole_buffer) {
    std::uint8_t buffer[64] = {};
    CHECK(call(nr_getrandom, {reinterpret_cast<std::uint64_t>(buffer), sizeof buffer, 1 /* GRND_NONBLOCK */}) == 64);
    int zeros = 0;
    for (const auto byte : buffer) zeros += byte == 0;
    CHECK(zeros < 16);
    CHECK(call(nr_getrandom, {reinterpret_cast<std::uint64_t>(buffer), sizeof buffer, 0x80}) == -einval);
}

TEST(fcntl_speaks_linux_flags) {
    int fds[2];
    CHECK(::pipe(fds) == 0);
    constexpr std::uint64_t f_dupfd = 0, f_getfd = 1, f_setfd = 2, f_getfl = 3, f_setfl = 4, f_dupfd_cloexec = 1030;
    constexpr std::int64_t linux_o_wronly = 01, linux_o_nonblock = 04000;
    const auto write_end = static_cast<std::uint64_t>(fds[1]);

    CHECK(call(nr_fcntl, {write_end, f_getfl}) == linux_o_wronly);
    CHECK(call(nr_fcntl, {write_end, f_setfl, static_cast<std::uint64_t>(linux_o_nonblock)}) == 0);
    CHECK(call(nr_fcntl, {write_end, f_getfl}) == (linux_o_wronly | linux_o_nonblock));
    CHECK((::fcntl(fds[1], F_GETFL) & O_NONBLOCK) != 0);  // really set on the host

    CHECK(call(nr_fcntl, {write_end, f_getfd}) == 0);
    CHECK(call(nr_fcntl, {write_end, f_setfd, 1 /* FD_CLOEXEC */}) == 0);
    CHECK(call(nr_fcntl, {write_end, f_getfd}) == 1);

    const auto copy = call(nr_fcntl, {write_end, f_dupfd_cloexec, 100});
    CHECK(copy >= 100);
    CHECK(call(nr_fcntl, {static_cast<std::uint64_t>(copy), f_getfd}) == 1);
    const auto plain_copy = call(nr_fcntl, {write_end, f_dupfd, 0});
    CHECK(plain_copy >= 0 && call(nr_fcntl, {static_cast<std::uint64_t>(plain_copy), f_getfd}) == 0);

    // bionic's start-up checks stdin, stdout and stderr like this and accepts only EBADF as failure.
    CHECK(call(nr_fcntl, {999, f_getfl}) == -ebadf);

    ::close(fds[0]);
    ::close(fds[1]);
    ::close(static_cast<int>(copy));
    ::close(static_cast<int>(plain_copy));
}

TEST(sched_queries_answer_like_an_ordinary_linux_thread) {
    CHECK(call(nr_sched_getscheduler, {0}) == 0);  // SCHED_OTHER
    CHECK(call(nr_sched_getscheduler, {static_cast<std::uint64_t>(::getpid())}) == 0);
    CHECK(call(nr_sched_getscheduler, {999'999}) == -esrch);
    int priority = -1;
    CHECK(call(nr_sched_getparam, {0, reinterpret_cast<std::uint64_t>(&priority)}) == 0);
    CHECK(priority == 0);
}

TEST(sigaltstack_keeps_the_guest_stack_in_linux_layout) {
    // Linux arm64 stack_t: ss_sp, ss_flags (int, padded), ss_size. Darwin orders size before flags.
    struct LinuxStack {
        std::uint64_t sp;
        std::int32_t flags;
        std::int32_t padding;
        std::uint64_t size;
    };
    constexpr std::int32_t ss_disable = 2;
    static std::uint8_t area[16 * 1024];

    LinuxStack old{};
    CHECK(call(nr_sigaltstack, {0, reinterpret_cast<std::uint64_t>(&old)}) == 0);
    CHECK(old.flags == ss_disable);  // a new thread has none

    const LinuxStack wanted{reinterpret_cast<std::uint64_t>(area), 0, 0, sizeof area};
    CHECK(call(nr_sigaltstack, {reinterpret_cast<std::uint64_t>(&wanted), 0}) == 0);
    LinuxStack now{};
    CHECK(call(nr_sigaltstack, {0, reinterpret_cast<std::uint64_t>(&now)}) == 0);
    CHECK(now.sp == wanted.sp && now.size == wanted.size && now.flags == 0);

    const LinuxStack too_small{reinterpret_cast<std::uint64_t>(area), 0, 0, 1024};
    CHECK(call(nr_sigaltstack, {reinterpret_cast<std::uint64_t>(&too_small), 0}) == -12);  // ENOMEM

    const LinuxStack off{0, ss_disable, 0, 0};
    CHECK(call(nr_sigaltstack, {reinterpret_cast<std::uint64_t>(&off), 0}) == 0);
    CHECK(call(nr_sigaltstack, {0, reinterpret_cast<std::uint64_t>(&now)}) == 0);
    CHECK(now.flags == ss_disable);
}

/// struct stat as the Linux kernel fills it on arm64 (include/uapi/asm-generic/stat.h), 128 bytes.
struct LinuxStat {
    std::uint64_t dev;
    std::uint64_t ino;
    std::uint32_t mode;
    std::uint32_t nlink;
    std::uint32_t uid;
    std::uint32_t gid;
    std::uint64_t rdev;
    std::uint64_t pad1;
    std::int64_t size;
    std::int32_t blksize;
    std::int32_t pad2;
    std::int64_t blocks;
    std::int64_t atime, atime_nsec, mtime, mtime_nsec, ctime, ctime_nsec;
    std::uint32_t unused4, unused5;
};
static_assert(sizeof(LinuxStat) == 128);

TEST(fstat_fills_the_linux_struct_stat) {
    char path[] = "/tmp/apkrt-fstat-XXXXXX";
    const int fd = ::mkstemp(path);
    CHECK(fd >= 0);
    CHECK(::write(fd, "twelve bytes", 12) == 12);
    LinuxStat linux_stat;
    std::memset(&linux_stat, 0xee, sizeof linux_stat);
    CHECK(call(nr_fstat, {static_cast<std::uint64_t>(fd), reinterpret_cast<std::uint64_t>(&linux_stat)}) == 0);
    struct stat host;
    CHECK(::fstat(fd, &host) == 0);
    CHECK(linux_stat.size == 12);
    CHECK((linux_stat.mode & 0170000) == 0100000);  // S_IFREG, same bits on both systems
    CHECK((linux_stat.mode & 0777) == (host.st_mode & 0777));
    CHECK(linux_stat.ino == host.st_ino);
    CHECK(linux_stat.nlink == 1 && linux_stat.uid == host.st_uid);
    CHECK(linux_stat.blksize > 0);
    CHECK(linux_stat.mtime == host.st_mtimespec.tv_sec && linux_stat.mtime_nsec == host.st_mtimespec.tv_nsec);
    CHECK(linux_stat.pad1 == 0 && linux_stat.pad2 == 0 && linux_stat.unused4 == 0 && linux_stat.unused5 == 0);

    // newfstatat with an empty path and AT_EMPTY_PATH is fstat, too.
    LinuxStat same{};
    CHECK(call(nr_newfstatat, {static_cast<std::uint64_t>(fd), reinterpret_cast<std::uint64_t>(""),
                               reinterpret_cast<std::uint64_t>(&same), 0x1000}) == 0);
    CHECK(same.ino == linux_stat.ino && same.size == 12);

    int fds[2];
    CHECK(::pipe(fds) == 0);
    CHECK(call(nr_fstat, {static_cast<std::uint64_t>(fds[1]), reinterpret_cast<std::uint64_t>(&linux_stat)}) == 0);
    CHECK((linux_stat.mode & 0170000) == 0010000);  // S_IFIFO
    CHECK(call(nr_fstat, {999, reinterpret_cast<std::uint64_t>(&linux_stat)}) == -ebadf);
    ::close(fds[0]);
    ::close(fds[1]);
    ::close(fd);
    ::unlink(path);
}

TEST(paths_outside_every_mount_do_not_exist) {
    // The guest sees only what the path table puts under Android paths (ARCHITECTURE.md 4.5).
    apkrt::vfs::reset();
    constexpr std::uint64_t at_fdcwd = static_cast<std::uint64_t>(-100);
    LinuxStat buffer{};
    CHECK(call(nr_newfstatat, {at_fdcwd, reinterpret_cast<std::uint64_t>("/dev/__properties__"),
                               reinterpret_cast<std::uint64_t>(&buffer), 0}) == -enoent);
    CHECK(call(nr_openat, {at_fdcwd, reinterpret_cast<std::uint64_t>("/etc/hosts"), 0, 0}) == -enoent);
    CHECK(call(nr_openat, {at_fdcwd, reinterpret_cast<std::uint64_t>("/"), 0, 0}) == -enoent);
}

TEST(the_basic_devices_are_there) {
    // /dev/null, /dev/zero and the random devices are always there: bionic's arc4random checks
    // access("/dev/urandom") early and otherwise lives on the 16 bytes of AT_RANDOM alone.
    apkrt::vfs::reset();
    constexpr std::uint64_t at_fdcwd = static_cast<std::uint64_t>(-100);
    constexpr std::uint64_t o_wronly = 1, o_nonblock = 04000, o_cloexec = 02000000;
    const auto path = [](const char *text) { return reinterpret_cast<std::uint64_t>(text); };

    const auto null = static_cast<int>(call(nr_openat, {at_fdcwd, path("/dev/null"), o_wronly | o_cloexec, 0}));
    CHECK(null >= 0);
    if (null >= 0) {
        CHECK(::write(null, "x", 1) == 1);
        CHECK((::fcntl(null, F_GETFD) & FD_CLOEXEC) != 0);
        CHECK((::fcntl(null, F_GETFL) & O_ACCMODE) == O_WRONLY);
        ::close(null);
    }
    const auto random = static_cast<int>(call(nr_openat, {at_fdcwd, path("/dev/urandom"), o_nonblock, 0}));
    CHECK(random >= 0);
    if (random >= 0) {
        unsigned char bytes[32] = {};
        CHECK(::read(random, bytes, sizeof bytes) == sizeof bytes);
        CHECK(std::count(std::begin(bytes), std::end(bytes), 0) < 8);
        CHECK((::fcntl(random, F_GETFD) & FD_CLOEXEC) == 0);
        CHECK((::fcntl(random, F_GETFL) & O_NONBLOCK) != 0);
        ::close(random);
    }
    CHECK(call(nr_faccessat, {at_fdcwd, path("/dev/urandom"), 4 /* R_OK */}) == 0);
    CHECK(call(nr_faccessat, {at_fdcwd, path("/dev/random"), 0 /* F_OK */}) == 0);
    CHECK(call(nr_faccessat2, {at_fdcwd, path("/dev/null"), 2 /* W_OK */, 0}) == 0);
    CHECK(call(nr_faccessat, {at_fdcwd, path("/dev/nonsense"), 0}) == -enoent);
    CHECK(call(nr_faccessat, {at_fdcwd, path("/data/local/tmp"), 0}) == -enoent);
    CHECK(call(nr_faccessat, {at_fdcwd, path("dev/null"), 0}) == 0);  // relative to the working directory "/"
}

// --- Files through the path table (ARCHITECTURE.md 4.5) ---------------------------------------------

constexpr std::uint64_t nr_getcwd = 17, nr_dup = 23, nr_dup3 = 24, nr_ioctl = 29, nr_mkdirat = 34, nr_unlinkat = 35,
                        nr_symlinkat = 36, nr_statfs = 43, nr_ftruncate = 46, nr_chdir = 49, nr_fchdir = 50,
                        nr_close = 57, nr_pipe2 = 59, nr_getdents64 = 61, nr_lseek = 62, nr_read = 63, nr_write = 64,
                        nr_readv = 65, nr_writev = 66, nr_pread64 = 67, nr_pwrite64 = 68, nr_readlinkat = 78,
                        nr_fsync = 82, nr_fdatasync = 83, nr_utimensat = 88, nr_renameat2 = 276;
constexpr std::uint64_t o_rdonly = 0, o_wronly = 1, o_rdwr = 2, o_creat = 0100, o_excl = 0200, o_trunc = 01000,
                        o_append = 02000, o_nonblock = 04000, o_directory = 040000, o_cloexec = 02000000;
constexpr std::uint64_t at_fdcwd = static_cast<std::uint64_t>(-100);
constexpr std::uint64_t at_symlink_nofollow = 0x100, at_removedir = 0x200;
constexpr std::int64_t eisdir = 21, enotdir = 20, enotempty = 39, erange = 34, espipe = 29, enotty = 25;

std::uint64_t address(const void *pointer) {
    return reinterpret_cast<std::uint64_t>(pointer);
}

std::uint64_t unsigned_of(std::int64_t value) {
    return static_cast<std::uint64_t>(value);
}

/// An empty host directory as the guest's /data/local/tmp, which is also the working directory.
struct Scratch {
    std::string host;

    Scratch() {
        char pattern[] = "/tmp/apkrt-files-XXXXXX";
        char resolved[PATH_MAX];
        host = ::realpath(::mkdtemp(pattern), resolved);
        apkrt::vfs::reset();
        apkrt::vfs::mount("/data/local/tmp", host);
        apkrt::vfs::set_working_directory("/data/local/tmp");
    }
    ~Scratch() {
        const std::string command = "rm -rf '" + host + "'";
        static_cast<void>(std::system(command.c_str()));
        apkrt::vfs::reset();
    }
    Scratch(const Scratch &) = delete;
    Scratch &operator=(const Scratch &) = delete;

    std::string contents(const std::string &name) const {
        std::string result;
        if (FILE *file = std::fopen((host + "/" + name).c_str(), "rb")) {
            char buffer[256];
            std::size_t count;
            while ((count = std::fread(buffer, 1, sizeof buffer, file)) > 0) result.append(buffer, count);
            std::fclose(file);
        }
        return result;
    }
};

std::int64_t open_guest(std::uint64_t directory, const char *path, std::uint64_t flags, std::uint64_t mode = 0) {
    return call(nr_openat, {directory, address(path), flags, mode});
}

std::int64_t write_text(std::int64_t fd, std::string_view text) {
    return call(nr_write, {unsigned_of(fd), address(text.data()), text.size()});
}

std::string read_text(std::int64_t fd, std::size_t size) {
    std::string buffer(size, '\0');
    const auto count = call(nr_read, {unsigned_of(fd), address(buffer.data()), size});
    return count < 0 ? "error " + std::to_string(-count) : buffer.substr(0, static_cast<std::size_t>(count));
}

TEST(files_under_a_mount_are_host_files) {
    Scratch scratch;
    const auto fd = open_guest(at_fdcwd, "/data/local/tmp/a.txt", o_creat | o_wronly | o_trunc, 0640);
    CHECK(fd >= 0);
    CHECK(write_text(fd, "hello") == 5);
    CHECK(call(nr_close, {unsigned_of(fd)}) == 0);
    CHECK(scratch.contents("a.txt") == "hello");
    struct stat host;
    CHECK(::stat((scratch.host + "/a.txt").c_str(), &host) == 0 && (host.st_mode & 0777) == 0640);

    const auto again = open_guest(at_fdcwd, "/data/local/tmp/a.txt", o_rdonly);
    CHECK(read_text(again, 16) == "hello");
    CHECK(read_text(again, 16).empty());  // end of file
    CHECK(call(nr_close, {unsigned_of(again)}) == 0);
    CHECK(call(nr_close, {unsigned_of(again)}) == -ebadf);
}

TEST(relative_paths_start_at_the_working_directory_or_a_directory_fd) {
    Scratch scratch;
    const auto fd = open_guest(at_fdcwd, "b.txt", o_creat | o_rdwr, 0600);
    CHECK(fd >= 0 && write_text(fd, "b") == 1);
    CHECK(scratch.contents("b.txt") == "b");
    CHECK(call(nr_mkdirat, {at_fdcwd, address("sub"), 0755}) == 0);
    const auto directory = open_guest(at_fdcwd, "./sub/../sub", o_rdonly | o_directory);
    CHECK(directory >= 0);
    const auto inner = open_guest(unsigned_of(directory), "c.txt", o_creat | o_wronly, 0600);
    CHECK(inner >= 0 && write_text(inner, "c") == 1);
    CHECK(scratch.contents("sub/c.txt") == "c");
    for (const auto open_fd : {fd, directory, inner}) call(nr_close, {unsigned_of(open_fd)});
}

TEST(open_reports_linux_errors) {
    Scratch scratch;
    call(nr_close, {unsigned_of(open_guest(at_fdcwd, "file", o_creat | o_wronly, 0600))});
    call(nr_mkdirat, {at_fdcwd, address("directory"), 0700});
    CHECK(open_guest(at_fdcwd, "missing", o_rdonly) == -enoent);
    CHECK(open_guest(at_fdcwd, "file", o_creat | o_excl | o_wronly, 0600) == -eexist);
    CHECK(open_guest(at_fdcwd, "file", o_rdonly | o_directory) == -enotdir);
    CHECK(open_guest(at_fdcwd, "file/below", o_rdonly) == -enotdir);
    CHECK(open_guest(at_fdcwd, "directory", o_wronly) == -eisdir);
    CHECK(open_guest(at_fdcwd, "/data/local/elsewhere", o_rdonly) == -enoent);
    CHECK(call(nr_mkdirat, {at_fdcwd, address("directory"), 0700}) == -eexist);
}

TEST(open_translates_the_linux_flags) {
    Scratch scratch;
    const auto first = open_guest(at_fdcwd, "log", o_creat | o_wronly, 0600);
    CHECK(write_text(first, "ab") == 2);
    const auto appending = open_guest(at_fdcwd, "log", o_wronly | o_append | o_cloexec);
    CHECK(write_text(first, "") == 0);
    CHECK(call(nr_lseek, {unsigned_of(appending), 0, 0}) == 0);
    CHECK(write_text(appending, "cd") == 2);  // at the end all the same
    CHECK(scratch.contents("log") == "abcd");
    CHECK((::fcntl(static_cast<int>(appending), F_GETFD) & FD_CLOEXEC) != 0);
    CHECK((::fcntl(static_cast<int>(first), F_GETFD) & FD_CLOEXEC) == 0);
    CHECK(call(nr_fcntl, {unsigned_of(appending), 3 /* F_GETFL */}) == static_cast<std::int64_t>(o_wronly | o_append));
    const auto truncating = open_guest(at_fdcwd, "log", o_wronly | o_trunc);
    CHECK(scratch.contents("log").empty());
    for (const auto fd : {first, appending, truncating}) call(nr_close, {unsigned_of(fd)});
}

TEST(offsets_and_vectors_work_like_linux) {
    Scratch scratch;
    const auto fd = open_guest(at_fdcwd, "digits", o_creat | o_rdwr, 0600);
    CHECK(write_text(fd, "0123456789") == 10);
    CHECK(call(nr_lseek, {unsigned_of(fd), 3, 0 /* SEEK_SET */}) == 3);
    CHECK(read_text(fd, 2) == "34");
    CHECK(call(nr_lseek, {unsigned_of(fd), 0, 1 /* SEEK_CUR */}) == 5);
    CHECK(call(nr_lseek, {unsigned_of(fd), unsigned_of(-1), 2 /* SEEK_END */}) == 9);
    CHECK(call(nr_lseek, {unsigned_of(fd), unsigned_of(-20), 1}) == -einval);
    CHECK(call(nr_lseek, {unsigned_of(fd), 0, 3 /* SEEK_DATA */}) == 0);
    CHECK(call(nr_lseek, {unsigned_of(fd), 0, 4 /* SEEK_HOLE */}) == 10);

    char three[3];
    CHECK(call(nr_pread64, {unsigned_of(fd), address(three), 3, 7}) == 3 && std::memcmp(three, "789", 3) == 0);
    CHECK(call(nr_lseek, {unsigned_of(fd), 0, 1}) == 10);  // pread leaves the offset where SEEK_HOLE put it
    CHECK(call(nr_pwrite64, {unsigned_of(fd), address("xy"), 2, 0}) == 2);
    CHECK(scratch.contents("digits") == "xy23456789");

    char first[4] = {}, second[6] = {};
    const iovec parts[] = {{first, sizeof first}, {second, sizeof second}};
    CHECK(call(nr_lseek, {unsigned_of(fd), 0, 0}) == 0);
    CHECK(call(nr_readv, {unsigned_of(fd), address(parts), 2}) == 10);
    CHECK(std::memcmp(first, "xy23", 4) == 0 && std::memcmp(second, "456789", 6) == 0);
    const iovec out[] = {{const_cast<char *>("A"), 1}, {const_cast<char *>("BC"), 2}};
    CHECK(call(nr_writev, {unsigned_of(fd), address(out), 2}) == 3);
    CHECK(scratch.contents("digits") == "xy23456789ABC");

    int fds[2];
    CHECK(::pipe(fds) == 0);
    CHECK(call(nr_lseek, {static_cast<std::uint64_t>(fds[0]), 0, 0}) == -espipe);
    ::close(fds[0]);
    ::close(fds[1]);
    call(nr_close, {unsigned_of(fd)});
}

TEST(size_changes_and_syncs_show_in_stat) {
    Scratch scratch;
    const auto fd = open_guest(at_fdcwd, "sized", o_creat | o_rdwr, 0644);
    CHECK(call(nr_ftruncate, {unsigned_of(fd), 100}) == 0);
    CHECK(call(nr_fsync, {unsigned_of(fd)}) == 0);
    CHECK(call(nr_fdatasync, {unsigned_of(fd)}) == 0);
    LinuxStat status{};
    CHECK(call(nr_newfstatat, {at_fdcwd, address("sized"), address(&status), 0}) == 0);
    CHECK(status.size == 100 && (status.mode & 0170777) == 0100644);
    CHECK(call(nr_newfstatat, {at_fdcwd, address("/data/local/tmp"), address(&status), 0}) == 0);
    CHECK((status.mode & 0170000) == 0040000);  // S_IFDIR
    CHECK(call(nr_newfstatat, {at_fdcwd, address("nothing"), address(&status), 0}) == -enoent);
    call(nr_close, {unsigned_of(fd)});
}

struct DirectoryEntry {
    std::string name;
    unsigned type;
};

/// The entries getdents64 fills into `buffer`: struct linux_dirent64, 8-byte aligned records.
std::vector<DirectoryEntry> entries_of(const char *buffer, std::int64_t size) {
    std::vector<DirectoryEntry> entries;
    for (std::int64_t offset = 0; offset < size;) {
        std::uint16_t length;
        std::memcpy(&length, buffer + offset + 16, sizeof length);
        CHECK(length % 8 == 0 && length >= 24);
        entries.push_back({buffer + offset + 19, static_cast<unsigned char>(buffer[offset + 18])});
        offset += length;
    }
    return entries;
}

TEST(directories_list_their_entries) {
    Scratch scratch;
    call(nr_mkdirat, {at_fdcwd, address("d"), 0755});
    call(nr_close, {unsigned_of(open_guest(at_fdcwd, "d/x", o_creat | o_wronly, 0600))});
    call(nr_mkdirat, {at_fdcwd, address("d/y"), 0755});
    const auto fd = open_guest(at_fdcwd, "d", o_rdonly | o_directory | o_cloexec);
    CHECK(fd >= 0);

    alignas(8) char buffer[4096];
    const auto size = call(nr_getdents64, {unsigned_of(fd), address(buffer), sizeof buffer});
    CHECK(size > 0);
    std::set<std::string> names;
    for (const auto &entry : entries_of(buffer, size)) {
        names.insert(entry.name);
        if (entry.name == "x") CHECK(entry.type == 8);  // DT_REG
        if (entry.name == "y" || entry.name == "." || entry.name == "..") CHECK(entry.type == 4);  // DT_DIR
    }
    CHECK((names == std::set<std::string>{".", "..", "x", "y"}));
    CHECK(call(nr_getdents64, {unsigned_of(fd), address(buffer), sizeof buffer}) == 0);  // the end

    // Back to the start, then one entry at a time: a buffer for one record gets one record.
    CHECK(call(nr_lseek, {unsigned_of(fd), 0, 0}) == 0);
    std::set<std::string> again;
    for (int round = 0; round < 10; ++round) {
        const auto one = call(nr_getdents64, {unsigned_of(fd), address(buffer), 24});
        if (one <= 0) break;
        const auto list = entries_of(buffer, one);
        CHECK(list.size() == 1);
        again.insert(list.front().name);
    }
    CHECK(again == names);
    CHECK(call(nr_lseek, {unsigned_of(fd), 0, 0}) == 0);
    CHECK(call(nr_getdents64, {unsigned_of(fd), address(buffer), 8}) == -einval);  // too small for any record
    CHECK(call(nr_getdents64, {unsigned_of(open_guest(at_fdcwd, "d/x", o_rdonly)), address(buffer), 4096}) == -enotdir);
    call(nr_close, {unsigned_of(fd)});
}

TEST(unlink_rmdir_and_rename_answer_like_linux) {
    Scratch scratch;
    call(nr_close, {unsigned_of(open_guest(at_fdcwd, "one", o_creat | o_wronly, 0600))});
    call(nr_close, {unsigned_of(open_guest(at_fdcwd, "two", o_creat | o_wronly, 0600))});
    call(nr_mkdirat, {at_fdcwd, address("full"), 0755});
    call(nr_close, {unsigned_of(open_guest(at_fdcwd, "full/inside", o_creat | o_wronly, 0600))});
    call(nr_mkdirat, {at_fdcwd, address("empty"), 0755});

    CHECK(call(nr_unlinkat, {at_fdcwd, address("empty"), 0}) == -eisdir);
    CHECK(call(nr_unlinkat, {at_fdcwd, address("full"), at_removedir}) == -enotempty);
    CHECK(call(nr_unlinkat, {at_fdcwd, address("one"), at_removedir}) == -enotdir);
    CHECK(call(nr_unlinkat, {at_fdcwd, address("empty"), at_removedir}) == 0);
    CHECK(call(nr_unlinkat, {at_fdcwd, address("missing"), 0}) == -enoent);

    CHECK(call(nr_renameat2, {at_fdcwd, address("one"), at_fdcwd, address("three"), 0}) == 0);
    CHECK(open_guest(at_fdcwd, "one", o_rdonly) == -enoent);
    CHECK(call(nr_renameat2, {at_fdcwd, address("three"), at_fdcwd, address("two"), 1 /* RENAME_NOREPLACE */}) == -eexist);
    CHECK(call(nr_renameat2, {at_fdcwd, address("three"), at_fdcwd, address("two"), 0}) == 0);  // replaces
    CHECK(call(nr_unlinkat, {at_fdcwd, address("two"), 0}) == 0);
    CHECK(call(nr_unlinkat, {at_fdcwd, address("three"), 0}) == -enoent);
}

TEST(the_working_directory_follows_chdir) {
    Scratch scratch;
    char buffer[256];
    CHECK(call(nr_getcwd, {address(buffer), sizeof buffer}) == 16);  // with the terminating NUL
    CHECK(std::string(buffer) == "/data/local/tmp");
    CHECK(call(nr_getcwd, {address(buffer), 10}) == -erange);
    call(nr_mkdirat, {at_fdcwd, address("sub"), 0755});
    call(nr_close, {unsigned_of(open_guest(at_fdcwd, "plain", o_creat | o_wronly, 0600))});

    CHECK(call(nr_chdir, {address("sub")}) == 0);
    CHECK(call(nr_getcwd, {address(buffer), sizeof buffer}) > 0 && std::string(buffer) == "/data/local/tmp/sub");
    call(nr_close, {unsigned_of(open_guest(at_fdcwd, "here", o_creat | o_wronly, 0600))});
    CHECK(scratch.contents("sub/here").empty() && ::access((scratch.host + "/sub/here").c_str(), F_OK) == 0);
    CHECK(call(nr_chdir, {address("..")}) == 0);
    CHECK(call(nr_getcwd, {address(buffer), sizeof buffer}) > 0 && std::string(buffer) == "/data/local/tmp");
    CHECK(call(nr_chdir, {address("plain")}) == -enotdir);
    CHECK(call(nr_chdir, {address("nowhere")}) == -enoent);

    const auto fd = open_guest(at_fdcwd, "sub", o_rdonly | o_directory);
    CHECK(call(nr_fchdir, {unsigned_of(fd)}) == 0);
    CHECK(call(nr_getcwd, {address(buffer), sizeof buffer}) > 0 && std::string(buffer) == "/data/local/tmp/sub");
    call(nr_close, {unsigned_of(fd)});
}

TEST(descriptors_duplicate_and_pipes_connect) {
    Scratch scratch;
    const auto fd = open_guest(at_fdcwd, "dup", o_creat | o_rdwr, 0600);
    const auto copy = call(nr_dup, {unsigned_of(fd)});
    CHECK(copy >= 0 && copy != fd);
    CHECK(write_text(copy, "via copy") == 8);
    CHECK(call(nr_dup3, {unsigned_of(fd), 60, o_cloexec}) == 60);
    CHECK((::fcntl(60, F_GETFD) & FD_CLOEXEC) != 0);
    CHECK(call(nr_dup3, {unsigned_of(fd), unsigned_of(fd), 0}) == -einval);
    for (const auto open_fd : {fd, copy, std::int64_t{60}}) call(nr_close, {unsigned_of(open_fd)});

    int fds[2] = {-1, -1};
    CHECK(call(nr_pipe2, {address(fds), o_cloexec | o_nonblock}) == 0);
    CHECK((::fcntl(fds[0], F_GETFD) & FD_CLOEXEC) != 0 && (::fcntl(fds[1], F_GETFL) & O_NONBLOCK) != 0);
    CHECK(read_text(fds[0], 4) == "error 11");  // EAGAIN on an empty non-blocking pipe
    CHECK(write_text(fds[1], "abc") == 3);
    int available = -1;
    CHECK(call(nr_ioctl, {static_cast<std::uint64_t>(fds[0]), 0x541b /* FIONREAD */, address(&available)}) == 0);
    CHECK(available == 3);
    CHECK(read_text(fds[0], 4) == "abc");
    CHECK(call(nr_ioctl, {static_cast<std::uint64_t>(fds[0]), 0x5401 /* TCGETS */, address(buffer_for_termios())}) ==
          -enotty);
    call(nr_close, {static_cast<std::uint64_t>(fds[0])});
    call(nr_close, {static_cast<std::uint64_t>(fds[1])});
}

TEST(symlinks_keep_their_text_and_access_checks_modes) {
    Scratch scratch;
    call(nr_close, {unsigned_of(open_guest(at_fdcwd, "target", o_creat | o_wronly, 0400))});
    CHECK(call(nr_symlinkat, {address("target"), at_fdcwd, address("link")}) == 0);
    char text[64] = {};
    CHECK(call(nr_readlinkat, {at_fdcwd, address("link"), address(text), sizeof text}) == 6);
    CHECK(std::string(text) == "target");
    CHECK(call(nr_readlinkat, {at_fdcwd, address("target"), address(text), sizeof text}) == -einval);
    LinuxStat status{};
    CHECK(call(nr_newfstatat, {at_fdcwd, address("link"), address(&status), at_symlink_nofollow}) == 0);
    CHECK((status.mode & 0170000) == 0120000);  // S_IFLNK
    CHECK(call(nr_newfstatat, {at_fdcwd, address("link"), address(&status), 0}) == 0);
    CHECK((status.mode & 0170000) == 0100000);  // followed to the file
    CHECK(call(nr_faccessat, {at_fdcwd, address("link"), 0 /* F_OK */}) == 0);
    CHECK(call(nr_faccessat, {at_fdcwd, address("target"), 4 /* R_OK */}) == 0);
    if (::geteuid() != 0) CHECK(call(nr_faccessat, {at_fdcwd, address("target"), 2 /* W_OK */}) == -eacces);
}

TEST(file_times_follow_utimensat) {
    Scratch scratch;
    call(nr_close, {unsigned_of(open_guest(at_fdcwd, "timed", o_creat | o_wronly, 0600))});
    const struct {
        std::int64_t seconds, nanoseconds;
    } times[2] = {{1'000'000'000, 5000}, {1'100'000'000, 7000}};
    CHECK(call(nr_utimensat, {at_fdcwd, address("timed"), address(times), 0}) == 0);
    LinuxStat status{};
    CHECK(call(nr_newfstatat, {at_fdcwd, address("timed"), address(&status), 0}) == 0);
    CHECK(status.atime == 1'000'000'000 && status.atime_nsec == 5000);
    CHECK(status.mtime == 1'100'000'000 && status.mtime_nsec == 7000);
    // UTIME_OMIT leaves a time alone, UTIME_NOW sets it to now (Linux values).
    const struct {
        std::int64_t seconds, nanoseconds;
    } special[2] = {{0, (1 << 30) - 2}, {0, (1 << 30) - 1}};
    CHECK(call(nr_utimensat, {at_fdcwd, address("timed"), address(special), 0}) == 0);
    CHECK(call(nr_newfstatat, {at_fdcwd, address("timed"), address(&status), 0}) == 0);
    CHECK(status.atime == 1'000'000'000 && status.mtime > 1'700'000'000);
}

TEST(umask_masks_the_mode_of_new_files) {
    Scratch scratch;
    constexpr std::uint64_t nr_umask = 166;
    const auto previous = call(nr_umask, {077});
    CHECK(previous >= 0 && previous <= 0777);
    CHECK(call(nr_umask, {0022 | 0170000}) == 077);  // only the permission bits count
    call(nr_close, {unsigned_of(open_guest(at_fdcwd, "masked", o_creat | o_wronly, 0666))});
    struct stat host;
    CHECK(::stat((scratch.host + "/masked").c_str(), &host) == 0 && (host.st_mode & 0777) == 0644);
    call(nr_umask, {static_cast<std::uint64_t>(previous)});
}

TEST(statfs_describes_the_file_system) {
    Scratch scratch;
    struct {
        std::int64_t type, block_size, blocks, free, available, files, free_files;
        std::int32_t fsid[2];
        std::int64_t name_length, fragment_size, flags, spare[4];
    } linux_statfs{};
    static_assert(sizeof linux_statfs == 120);
    CHECK(call(nr_statfs, {address("/data/local/tmp"), address(&linux_statfs)}) == 0);
    CHECK(linux_statfs.block_size > 0 && linux_statfs.blocks > 0 && linux_statfs.name_length == 255);
    CHECK(call(nr_statfs, {address("/nowhere"), address(&linux_statfs)}) == -enoent);
}

TEST(clock_gettime_maps_the_linux_clocks) {
    struct {
        std::int64_t seconds, nanoseconds;
    } linux_time{};
    const auto nanoseconds = [&] { return linux_time.seconds * 1'000'000'000 + linux_time.nanoseconds; };
    const auto host = [](clockid_t clock) { return static_cast<std::int64_t>(clock_gettime_nsec_np(clock)); };

    // F10: Linux CLOCK_MONOTONIC stops during sleep like Darwin's CLOCK_UPTIME_RAW, CLOCK_BOOTTIME goes on
    // like Darwin's CLOCK_MONOTONIC.
    const struct {
        std::uint64_t linux_clock;
        clockid_t darwin_clock;
    } pairs[] = {{0, CLOCK_REALTIME}, {1, CLOCK_UPTIME_RAW}, {4, CLOCK_UPTIME_RAW}, {5, CLOCK_REALTIME},
                 {6, CLOCK_UPTIME_RAW}, {7, CLOCK_MONOTONIC}};
    for (const auto &pair : pairs) {
        const auto before = host(pair.darwin_clock);
        CHECK(call(nr_clock_gettime, {pair.linux_clock, reinterpret_cast<std::uint64_t>(&linux_time)}) == 0);
        const auto after = host(pair.darwin_clock);
        if (!(before <= nanoseconds() && nanoseconds() <= after)) {
            std::fprintf(stderr, "Linux clock %llu is not Darwin clock %d\n",
                         static_cast<unsigned long long>(pair.linux_clock), pair.darwin_clock);
            CHECK(false);
        }
        CHECK(linux_time.nanoseconds >= 0 && linux_time.nanoseconds < 1'000'000'000);
    }
    CHECK(call(nr_clock_gettime, {2, reinterpret_cast<std::uint64_t>(&linux_time)}) == 0);  // process CPU time
    CHECK(nanoseconds() > 0);
    CHECK(call(nr_clock_gettime, {10, reinterpret_cast<std::uint64_t>(&linux_time)}) == -einval);  // no clock 10
}

TEST(gettimeofday_fills_the_linux_timeval) {
    constexpr std::uint64_t nr_gettimeofday = 169;
    struct {
        std::int64_t seconds, microseconds;  // both 64-bit on Linux; Darwin's tv_usec is 32-bit
    } value{-1, -1};
    struct {
        std::int32_t minutes_west, dst_time;
    } zone{7, 7};
    CHECK(call(nr_gettimeofday, {address(&value), address(&zone)}) == 0);
    const auto now = static_cast<std::int64_t>(::time(nullptr));
    CHECK(value.seconds >= now - 1 && value.seconds <= now + 1);
    CHECK(value.microseconds >= 0 && value.microseconds < 1'000'000);
    CHECK(zone.minutes_west == 0 && zone.dst_time == 0);
    CHECK(call(nr_gettimeofday, {0, 0}) == 0);
}

std::int64_t monotonic_nanoseconds() {
    return static_cast<std::int64_t>(clock_gettime_nsec_np(CLOCK_UPTIME_RAW));
}

TEST(nanosleep_sleeps_at_least_as_long_as_asked) {
    constexpr std::uint64_t nr_nanosleep = 101;
    const struct {
        std::int64_t seconds, nanoseconds;
    } ten_ms{0, 10'000'000}, invalid{0, 1'000'000'000};
    const auto before = monotonic_nanoseconds();
    CHECK(call(nr_nanosleep, {address(&ten_ms), 0}) == 0);
    CHECK(monotonic_nanoseconds() - before >= 10'000'000);
    CHECK(call(nr_nanosleep, {address(&invalid), 0}) == -einval);
}

TEST(clock_nanosleep_sleeps_relative_or_until_a_time) {
    constexpr std::uint64_t nr_clock_nanosleep = 115, clock_monotonic = 1, timer_abstime = 1;
    struct Time {
        std::int64_t seconds, nanoseconds;
    };
    const Time ten_ms{0, 10'000'000};
    auto before = monotonic_nanoseconds();
    CHECK(call(nr_clock_nanosleep, {clock_monotonic, 0, address(&ten_ms), 0}) == 0);
    CHECK(monotonic_nanoseconds() - before >= 10'000'000);

    Time now{};
    CHECK(call(nr_clock_gettime, {clock_monotonic, address(&now)}) == 0);
    Time until{now.seconds, now.nanoseconds + 10'000'000};
    if (until.nanoseconds >= 1'000'000'000) {
        until.seconds += 1;
        until.nanoseconds -= 1'000'000'000;
    }
    before = monotonic_nanoseconds();
    CHECK(call(nr_clock_nanosleep, {clock_monotonic, timer_abstime, address(&until), 0}) == 0);
    CHECK(monotonic_nanoseconds() - before >= 9'000'000);  // the clock read above came a little earlier
    CHECK(call(nr_clock_nanosleep, {clock_monotonic, timer_abstime, address(&now), 0}) == 0);  // in the past
    CHECK(call(nr_clock_nanosleep, {1234, 0, address(&ten_ms), 0}) == -einval);
}

TEST(sched_getaffinity_reports_every_active_cpu) {
    int cpus = 0;
    std::size_t size = sizeof cpus;
    CHECK(::sysctlbyname("hw.activecpu", &cpus, &size, nullptr, 0) == 0);
    std::uint64_t mask[2] = {~0ull, ~0ull};
    CHECK(call(nr_sched_getaffinity, {0, sizeof mask, reinterpret_cast<std::uint64_t>(mask)}) == 8);
    CHECK(mask[0] == (cpus >= 64 ? ~0ull : (1ull << cpus) - 1));
    CHECK(mask[1] == ~0ull);  // beyond the 8 bytes Linux reports, nothing is touched
    CHECK(call(nr_sched_getaffinity, {0, 4, reinterpret_cast<std::uint64_t>(mask)}) == -einval);
}

TEST(futex_waits_and_wakes_like_linux) {
    constexpr std::uint64_t futex_wait = 0, futex_wake = 1, futex_private = 128;
    alignas(4) static std::atomic<std::uint32_t> word{7};
    const auto address = reinterpret_cast<std::uint64_t>(&word);

    CHECK(call(nr_futex, {address, futex_wake | futex_private, 1}) == 0);  // nobody waits
    CHECK(call(nr_futex, {address, futex_wait | futex_private, 8, 0}) == -eagain);  // value differs

    struct {
        std::int64_t seconds, nanoseconds;
    } timeout{0, 20'000'000};
    CHECK(call(nr_futex, {address, futex_wait | futex_private, 7, reinterpret_cast<std::uint64_t>(&timeout)}) ==
          -etimedout);

    // One waiter on another thread, woken from here.
    static std::atomic<std::int64_t> waited{-1};
    pthread_t thread;
    CHECK(::pthread_create(
              &thread, nullptr,
              [](void *argument) -> void * {
                  waited = call(nr_futex, {reinterpret_cast<std::uint64_t>(argument), 0 | 128, 7, 0});
                  return nullptr;
              },
              &word) == 0);
    std::int64_t woken = 0;
    for (int attempt = 0; attempt < 2000 && woken == 0; ++attempt) {
        ::usleep(1000);
        woken = call(nr_futex, {address, futex_wake | futex_private, 1});
    }
    CHECK(woken == 1);
    CHECK(::pthread_join(thread, nullptr) == 0);
    CHECK(waited == 0);
}

TEST(prctl_names_anonymous_memory_like_an_android_kernel) {
    // PR_SET_VMA, PR_SET_VMA_ANON_NAME: Android's kernels have CONFIG_ANON_VMA_NAME, and bionic names
    // every thread stack with it. Failing it would leave EINVAL in errno of every new thread.
    const auto memory = map_anonymous_pages(1);
    CHECK(call(nr_prctl, {0x5356'4d41, 0, memory, page, reinterpret_cast<std::uint64_t>("stack")}) == 0);
    CHECK(call(nr_prctl, {0x5356'4d41, 1, memory, page, 0}) == -einval);  // no other PR_SET_VMA options
    call(nr_munmap, {memory, page});
}

TEST(prctl_offers_no_tagged_address_abi) {
    // PR_SET_TAGGED_ADDR_CTRL: with it, bionic would tag heap pointers in the top byte.
    CHECK(call(nr_prctl, {55, 1, 0, 0, 0}) == -einval);
    CHECK(call(nr_prctl, {56, 0, 0, 0, 0}) == -einval);
}

}  // namespace

int main() {
    return apk::testing::run_all();
}
