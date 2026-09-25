// Checks the guest's descriptor table (ADR-0027): the guest's numbers are Linux's, whatever the
// process holds on the host, every call that takes a descriptor goes through the table, and the
// guest's O_NONBLOCK is its own. Each call goes through the dispatcher with Linux numbers, as a
// guest's svc veneer delivers it.
#include "descriptors.h"
#include "signals.h"
#include "syscall_frame.h"
#include "threads.h"
#include "vfs.h"

#include "check.h"

#include <fcntl.h>
#include <limits.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

// Linux AArch64 syscall numbers and constants (asm-generic headers).
constexpr std::uint64_t nr_getcwd = 17, nr_dup = 23, nr_dup3 = 24, nr_fcntl = 25, nr_ioctl = 29, nr_fstatfs = 44,
                        nr_ftruncate = 46, nr_fchdir = 50, nr_fchmod = 52, nr_openat = 56, nr_close = 57,
                        nr_pipe2 = 59, nr_getdents64 = 61, nr_lseek = 62, nr_read = 63, nr_write = 64,
                        nr_readv = 65, nr_writev = 66, nr_pread64 = 67, nr_fstat = 80, nr_fsync = 82,
                        nr_utimensat = 88, nr_munmap = 215, nr_mmap = 222;
constexpr std::uint64_t f_dupfd = 0, f_getfd = 1, f_setfd = 2, f_getfl = 3, f_setfl = 4, f_dupfd_cloexec = 1030;
constexpr std::uint64_t o_rdonly = 0, o_rdwr = 2, o_creat = 0100, o_append = 02000, o_cloexec = 02000000;
constexpr std::uint64_t at_fdcwd = static_cast<std::uint64_t>(-100);
constexpr std::uint64_t seek_set = 0, seek_cur = 1;
constexpr std::uint64_t prot_read = 1, map_private = 2;
constexpr std::uint64_t o_nonblock = 04000;
constexpr std::uint64_t fionread = 0x541b, fionbio = 0x5421;
constexpr std::int64_t eintr = 4, ebadf = 9, eagain = 11, einval = 22, emfile = 24;

std::int64_t call(std::uint64_t number, std::initializer_list<std::uint64_t> arguments) {
    apkrt_syscall_frame frame{};
    std::size_t index = 0;
    for (const auto argument : arguments) frame.x[index++] = argument;
    frame.x[8] = number;
    apkrt_syscall_dispatch(&frame);
    return static_cast<std::int64_t>(frame.x[0]);
}

std::uint64_t address(const void *pointer) {
    return reinterpret_cast<std::uint64_t>(pointer);
}

std::uint64_t unsigned_of(std::int64_t value) {
    return static_cast<std::uint64_t>(value);
}

std::int64_t open_guest(const char *path, std::uint64_t flags, std::uint64_t mode = 0600) {
    return call(nr_openat, {at_fdcwd, address(path), flags, mode});
}

std::int64_t write_text(std::int64_t fd, std::string_view text) {
    return call(nr_write, {unsigned_of(fd), address(text.data()), text.size()});
}

std::string read_text(std::int64_t fd, std::size_t size) {
    std::string buffer(size, '\0');
    const auto count = call(nr_read, {unsigned_of(fd), address(buffer.data()), size});
    return count < 0 ? "error " + std::to_string(-count) : buffer.substr(0, static_cast<std::size_t>(count));
}

/// Everything a host descriptor delivers until its end.
std::string read_all(int host) {
    std::string result;
    char buffer[256];
    for (ssize_t count; (count = ::read(host, buffer, sizeof buffer)) > 0;) {
        result.append(buffer, static_cast<std::size_t>(count));
    }
    return result;
}

/// An empty host directory as the guest's /data/local/tmp and working directory, a fresh table with
/// the standard descriptors, and descriptors the process holds on the host, as the app does: a call
/// that took a guest number for a host descriptor would hit one of them.
struct Table {
    std::string host;
    std::vector<int> held;

    Table() {
        char pattern[] = "/tmp/apkrt-descriptors-XXXXXX";
        char resolved[PATH_MAX];
        host = ::realpath(::mkdtemp(pattern), resolved);
        apkrt::vfs::reset();
        apkrt::vfs::mount("/data/local/tmp", host);
        apkrt::vfs::set_working_directory("/data/local/tmp");
        // The starter's 0, 1 and 2 exist, as they do for the app and for apk-run.
        for (int standard = 0; standard < 3; ++standard) {
            if (::fcntl(standard, F_GETFD) == -1) ::open("/dev/null", O_RDWR);
        }
        apkrt::descriptors::reset();
        apkrt::descriptors::adopt_standard_descriptors();
        for (int count = 0; count < 16; ++count) held.push_back(::open("/dev/null", O_RDONLY | O_CLOEXEC));
    }
    ~Table() {
        for (const int fd : held) ::close(fd);
        apkrt::descriptors::reset();
        const std::string command = "rm -rf '" + host + "'";
        static_cast<void>(std::system(command.c_str()));
        apkrt::vfs::reset();
    }
    Table(const Table &) = delete;
    Table &operator=(const Table &) = delete;

    /// Whether the process still holds every descriptor of its own.
    bool holds_its_own() const {
        for (const int fd : held) {
            if (::fcntl(fd, F_GETFD) == -1) return false;
        }
        return true;
    }

    std::string contents(const std::string &name) const {
        const int fd = ::open((host + "/" + name).c_str(), O_RDONLY);
        if (fd < 0) return "missing";
        auto result = read_all(fd);
        ::close(fd);
        return result;
    }
};

/// Runs `body` in a child process and returns whether its checks passed there: for tests that
/// change the process's standard descriptors or close numbers by the hundred. It runs on a thread of
/// its own there, with a signal state of its own: the kqueue of the parent's thread for its waits on
/// descriptors (signals.h) does not survive fork.
template <typename Body>
bool in_child(Body body) {
    std::fflush(nullptr);
    const pid_t child = ::fork();
    if (child == 0) {
        std::thread(body).join();
        std::fflush(nullptr);
        ::_exit(apk::testing::failures == 0 ? 0 : 1);
    }
    int status = 0;
    ::waitpid(child, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

// --- Numbers (ADR-0027, test 1) -------------------------------------------------------------------------

TEST(guest_numbers_start_after_the_standard_ones_whatever_the_host_holds) {
    CHECK(in_child([] {
        Table table;
        CHECK(open_guest("first", o_creat | o_rdwr) == 3);
        // Closing every guest number from 3 to 1,023 leaves the process's own descriptors open.
        for (std::uint64_t fd = 3; fd < 1024; ++fd) call(nr_close, {fd});
        CHECK(table.holds_its_own());
    }));
}

TEST(numbers_are_the_lowest_free_ones) {
    Table table;
    CHECK(open_guest("a", o_creat | o_rdwr) == 3);
    CHECK(open_guest("b", o_creat | o_rdwr) == 4);
    CHECK(open_guest("c", o_creat | o_rdwr) == 5);
    CHECK(call(nr_close, {4}) == 0);
    CHECK(open_guest("d", o_creat | o_rdwr) == 4);
    CHECK(call(nr_dup, {3}) == 6);
    CHECK(call(nr_fcntl, {3, f_dupfd, 10}) == 10);
    CHECK(call(nr_fcntl, {3, f_dupfd, 10}) == 11);
    CHECK(call(nr_fcntl, {3, f_dupfd_cloexec, 0}) == 7);
    CHECK(call(nr_fcntl, {7, f_getfd}) == 1 && call(nr_fcntl, {3, f_getfd}) == 0);
    CHECK(call(nr_dup3, {3, 20, o_cloexec}) == 20);
    CHECK(call(nr_fcntl, {20, f_getfd}) == 1);
    // dup3 onto an open number: it refers to the other file from then on.
    CHECK(call(nr_dup3, {5, 20, 0}) == 20);
    CHECK(call(nr_fcntl, {20, f_getfd}) == 0);
    CHECK(write_text(20, "to c") == 4);
    CHECK(table.contents("c") == "to c" && table.contents("a").empty());
    CHECK(table.holds_its_own());
}

TEST(duplicates_share_the_offset_and_the_status_flags_but_not_close_on_exec) {
    Table table;
    const auto fd = open_guest("shared", o_creat | o_rdwr);
    const auto copy = call(nr_dup, {unsigned_of(fd)});
    CHECK(fd >= 3 && copy > fd);
    CHECK(write_text(fd, "0123456789") == 10);
    CHECK(call(nr_lseek, {unsigned_of(copy), 0, seek_cur}) == 10);
    CHECK(call(nr_lseek, {unsigned_of(copy), 4, seek_set}) == 4);
    CHECK(read_text(fd, 3) == "456");
    CHECK(call(nr_fcntl, {unsigned_of(copy), f_setfl, o_append}) == 0);
    CHECK((call(nr_fcntl, {unsigned_of(fd), f_getfl}) & static_cast<std::int64_t>(o_append)) != 0);
    CHECK(call(nr_fcntl, {unsigned_of(copy), f_setfd, 1}) == 0);
    CHECK(call(nr_fcntl, {unsigned_of(fd), f_getfd}) == 0);

    // The host descriptor goes with the last number that refers to it.
    int host = -1;
    if (const auto description = apkrt::descriptors::get(static_cast<int>(fd))) host = description->host();
    CHECK(host >= 0);
    if (host < 0) return;
    CHECK(call(nr_close, {unsigned_of(fd)}) == 0);
    CHECK(::fcntl(host, F_GETFD) != -1);
    CHECK(call(nr_close, {unsigned_of(copy)}) == 0);
    CHECK(::fcntl(host, F_GETFD) == -1);
}

TEST(descriptor_errors_are_linux_errors) {
    Table table;
    CHECK(open_guest("errors", o_creat | o_rdwr) == 3);
    char buffer[4];
    CHECK(call(nr_close, {9}) == -ebadf);
    CHECK(call(nr_dup, {9}) == -ebadf);
    CHECK(call(nr_read, {9, address(buffer), 1}) == -ebadf);
    CHECK(call(nr_fcntl, {9, f_getfd}) == -ebadf);
    CHECK(call(nr_fcntl, {9, f_getfl}) == -ebadf);
    CHECK(call(nr_close, {unsigned_of(-1)}) == -ebadf);
    CHECK(call(nr_close, {1u << 20}) == -ebadf);
    CHECK(call(nr_dup3, {3, 3, 0}) == -einval);
    CHECK(call(nr_dup3, {3, 8, 1}) == -einval);  // flags other than O_CLOEXEC
    CHECK(call(nr_dup3, {9, 8, 0}) == -ebadf);
    CHECK(call(nr_dup3, {3, unsigned_of(apkrt::descriptors::limit), 0}) == -ebadf);
    CHECK(call(nr_dup3, {3, unsigned_of(-1), 0}) == -ebadf);
    CHECK(call(nr_fcntl, {3, f_dupfd, unsigned_of(-1)}) == -einval);
    CHECK(call(nr_fcntl, {3, f_dupfd, unsigned_of(apkrt::descriptors::limit)}) == -einval);
    CHECK(table.holds_its_own());
}

TEST(a_full_table_answers_emfile) {
    CHECK(in_child([] {
        Table table;
        const auto fd = open_guest("full", o_creat | o_rdwr);
        CHECK(fd == 3);
        int added = 0;
        while (added <= apkrt::descriptors::limit && call(nr_dup, {unsigned_of(fd)}) >= 0) ++added;
        CHECK(added == apkrt::descriptors::limit - 4);  // 0, 1, 2 and fd were taken
        CHECK(call(nr_dup, {unsigned_of(fd)}) == -emfile);
        CHECK(call(nr_fcntl, {unsigned_of(fd), f_dupfd, 100}) == -emfile);
        CHECK(open_guest("one more", o_creat | o_rdwr) == -emfile);
        // With one free number, a pipe answers EMFILE and takes none.
        CHECK(call(nr_close, {100}) == 0);
        int ends[2] = {-1, -1};
        CHECK(call(nr_pipe2, {address(ends), 0}) == -emfile);
        CHECK(call(nr_dup, {unsigned_of(fd)}) == 100);
    }));
}

// --- The limit (ADR-0027, decision 2) ------------------------------------------------------------------

TEST(the_descriptor_limit_is_androids_and_counts) {
    // bionic's fdsan asks for it when a program closes a number above 127.
    constexpr std::uint64_t nr_getrlimit = 163, nr_setrlimit = 164, nr_prlimit64 = 261, rlimit_nofile = 7;
    constexpr std::int64_t eperm = 1;
    CHECK(in_child([] {
        Table table;
        struct Limits {
            std::uint64_t soft, hard;
        } limits{};
        CHECK(call(nr_getrlimit, {rlimit_nofile, address(&limits)}) == 0);
        CHECK(limits.soft == 32768 && limits.hard == 32768);
        limits = {};
        CHECK(call(nr_prlimit64, {0, rlimit_nofile, 0, address(&limits)}) == 0);
        CHECK(limits.soft == 32768 && limits.hard == 32768);

        // A lower soft limit counts for every new number.
        const Limits lower{16, 32768};
        CHECK(call(nr_setrlimit, {rlimit_nofile, address(&lower)}) == 0);
        const auto fd = open_guest("limited", o_creat | o_rdwr);
        CHECK(fd == 3);
        int added = 0;
        while (added < 100 && call(nr_dup, {unsigned_of(fd)}) >= 0) ++added;
        CHECK(added == 12);  // 4 to 15
        CHECK(call(nr_dup, {unsigned_of(fd)}) == -emfile);
        CHECK(call(nr_dup3, {unsigned_of(fd), 16, 0}) == -ebadf);
        CHECK(call(nr_fcntl, {unsigned_of(fd), f_dupfd, 16}) == -einval);

        // The hard limit does not rise, and the soft one stays below it.
        const Limits higher{32768, 65536};
        CHECK(call(nr_prlimit64, {0, rlimit_nofile, address(&higher), 0}) == -eperm);
        const Limits inverted{100, 50};
        CHECK(call(nr_setrlimit, {rlimit_nofile, address(&inverted)}) == -einval);
        const Limits back{32768, 32768};
        CHECK(call(nr_setrlimit, {rlimit_nofile, address(&back)}) == 0);
        CHECK(call(nr_dup, {unsigned_of(fd)}) == 16);
    }));
}

// --- The standard descriptors (ADR-0027, test 2) ----------------------------------------------------

TEST(the_standard_descriptors_are_the_starters_and_stay_open) {
    CHECK(in_child([] {
        int out[2], err[2];
        if (::pipe(out) != 0 || ::pipe(err) != 0) ::_exit(3);
        const int saved_out = ::dup(STDOUT_FILENO), saved_err = ::dup(STDERR_FILENO);
        ::dup2(out[1], STDOUT_FILENO);
        ::dup2(err[1], STDERR_FILENO);
        apkrt::descriptors::reset();
        apkrt::descriptors::adopt_standard_descriptors();
        const auto wrote_out = write_text(1, "to stdout");
        const auto wrote_err = write_text(2, "to stderr");
        const auto closed = call(nr_close, {1});
        const bool starter_keeps_1 = ::fcntl(STDOUT_FILENO, F_GETFD) != -1;
        const auto after_close = write_text(1, "gone");
        // The test's own streams come back before anything is checked.
        ::dup2(saved_out, STDOUT_FILENO);
        ::dup2(saved_err, STDERR_FILENO);
        apkrt::descriptors::reset();
        ::close(out[1]);
        ::close(err[1]);
        CHECK(wrote_out == 9 && wrote_err == 9 && closed == 0);
        CHECK(starter_keeps_1);
        CHECK(after_close == -ebadf);
        CHECK(read_all(out[0]) == "to stdout");
        CHECK(read_all(err[0]) == "to stderr");
    }));
}

// --- Calls that take a descriptor (ADR-0027, test 3) ------------------------------------------------

/// A host descriptor, taken into the table as the runtime takes one it opened for the guest.
std::int64_t guest_number_of(int host) {
    return apkrt::descriptors::add(host, false);
}

TEST(file_calls_go_through_the_table) {
    Table table;
    const std::string path = table.host + "/file";
    const int created = ::open(path.c_str(), O_CREAT | O_RDWR, 0644);
    CHECK(::write(created, "0123456789", 10) == 10);
    ::close(created);
    const auto fd = guest_number_of(::open(path.c_str(), O_RDWR));
    CHECK(fd == 3);

    struct {
        std::uint64_t dev, ino;
        std::uint32_t mode, nlink, uid, gid;
        std::uint64_t rdev, pad1;
        std::int64_t size;
        std::uint8_t rest[72];
    } status{};
    static_assert(sizeof status == 128);
    CHECK(call(nr_fstat, {unsigned_of(fd), address(&status)}) == 0);
    CHECK(status.size == 10 && (status.mode & 0170000) == 0100000);
    CHECK(call(nr_lseek, {unsigned_of(fd), 2, seek_set}) == 2);
    CHECK(read_text(fd, 3) == "234");
    char two[2] = {};
    CHECK(call(nr_pread64, {unsigned_of(fd), address(two), 2, 8}) == 2 && std::memcmp(two, "89", 2) == 0);
    CHECK(write_text(fd, "ab") == 2);
    CHECK(table.contents("file") == "01234ab789");
    CHECK(call(nr_fcntl, {unsigned_of(fd), f_getfl}) == static_cast<std::int64_t>(o_rdwr));
    std::uint8_t statfs_buffer[120];
    CHECK(call(nr_fstatfs, {unsigned_of(fd), address(statfs_buffer)}) == 0);
    CHECK(call(nr_fsync, {unsigned_of(fd)}) == 0);
    CHECK(call(nr_fchmod, {unsigned_of(fd), 0600}) == 0);
    struct stat host_status;
    CHECK(::stat(path.c_str(), &host_status) == 0 && (host_status.st_mode & 0777) == 0600);

    // utimensat with a NULL path changes the descriptor's file.
    const struct {
        std::int64_t seconds, nanoseconds;
    } times[2] = {{1'000'000'000, 0}, {1'200'000'000, 0}};
    CHECK(call(nr_utimensat, {unsigned_of(fd), 0, address(times), 0}) == 0);
    CHECK(::stat(path.c_str(), &host_status) == 0 && host_status.st_mtimespec.tv_sec == 1'200'000'000);

    // mmap maps the descriptor's file.
    const auto mapped = call(nr_mmap, {0, 16384, prot_read, map_private, unsigned_of(fd), 0});
    CHECK(mapped > 0);
    if (mapped > 0) {
        CHECK(std::memcmp(reinterpret_cast<const void *>(mapped), "01234ab789", 10) == 0);
        CHECK(call(nr_munmap, {unsigned_of(mapped), 16384}) == 0);
    }

    CHECK(call(nr_ftruncate, {unsigned_of(fd), 4}) == 0);
    CHECK(table.contents("file") == "0123");
    CHECK(table.holds_its_own());
}

TEST(directory_calls_go_through_the_table) {
    Table table;
    const std::string directory_path = table.host + "/dir";
    CHECK(::mkdir(directory_path.c_str(), 0755) == 0);
    for (const char *name : {"one", "two"}) {
        const int created = ::open((directory_path + "/" + name).c_str(), O_CREAT | O_WRONLY, 0644);
        CHECK(::write(created, name, 3) == 3);
        ::close(created);
    }
    const auto directory = guest_number_of(::open(directory_path.c_str(), O_RDONLY | O_DIRECTORY));
    CHECK(directory == 3);

    // A path relative to the directory descriptor.
    const auto one = call(nr_openat, {unsigned_of(directory), address("one"), o_rdonly, 0});
    CHECK(one >= 0 && read_text(one, 8) == "one");

    // getdents64: the position belongs to the open directory, and a duplicate shares it. 24 bytes
    // hold exactly one record for ".", "..", "one" or "two".
    const auto copy = call(nr_dup, {unsigned_of(directory)});
    std::set<std::string> names;
    std::string previous;
    for (int round = 0; round < 4; ++round) {
        alignas(8) char record[24] = {};
        const auto reader = round % 2 == 0 ? directory : copy;
        CHECK(call(nr_getdents64, {unsigned_of(reader), address(record), sizeof record}) == 24);
        const std::string name(record + 19);
        CHECK(name != previous);
        names.insert(name);
        previous = name;
    }
    CHECK(names == std::set<std::string>({".", "..", "one", "two"}));
    alignas(8) char end[24];
    CHECK(call(nr_getdents64, {unsigned_of(copy), address(end), sizeof end}) == 0);
    CHECK(call(nr_lseek, {unsigned_of(copy), 0, seek_set}) == 0);
    CHECK(call(nr_getdents64, {unsigned_of(directory), address(end), sizeof end}) == 24);

    // fchdir takes the directory's guest path.
    CHECK(call(nr_fchdir, {unsigned_of(directory)}) == 0);
    char cwd[128] = {};
    CHECK(call(nr_getcwd, {address(cwd), sizeof cwd}) > 0 && std::string(cwd) == "/data/local/tmp/dir");
    CHECK(table.holds_its_own());
}

TEST(pipe_calls_go_through_the_table) {
    Table table;
    int ends[2];
    CHECK(::pipe(ends) == 0);
    CHECK(::write(ends[1], "xyz", 3) == 3);
    ::close(ends[1]);
    const auto reader = guest_number_of(ends[0]);
    CHECK(reader == 3);
    int available = -1;
    CHECK(call(nr_ioctl, {unsigned_of(reader), fionread, address(&available)}) == 0);
    CHECK(available == 3);
    CHECK(read_text(reader, 8) == "xyz");
    CHECK(read_text(reader, 8).empty());  // the writer is gone

    // pipe2's ends are guest numbers, and the host's close-on-exec is set on both.
    int guest_ends[2] = {-1, -1};
    CHECK(call(nr_pipe2, {address(guest_ends), 0}) == 0);
    CHECK(guest_ends[0] == 4 && guest_ends[1] == 5);
    CHECK(write_text(guest_ends[1], "through") == 7);
    CHECK(read_text(guest_ends[0], 16) == "through");
    CHECK(call(nr_fcntl, {unsigned_of(guest_ends[0]), f_getfd}) == 0);
    for (const int end : guest_ends) {
        const auto description = apkrt::descriptors::get(end);
        CHECK(description != nullptr);
        if (description != nullptr) CHECK((::fcntl(description->host(), F_GETFD) & FD_CLOEXEC) != 0);
    }
    CHECK(table.holds_its_own());
}

// --- Descriptors that can block (ADR-0027, decisions 5 and 7, test 7) -------------------------------

using namespace std::chrono_literals;

/// The host's status flags behind a guest number.
int host_status_flags(std::int64_t fd) {
    const auto description = apkrt::descriptors::get(static_cast<int>(fd));
    return description != nullptr ? ::fcntl(description->host(), F_GETFL) : -1;
}

/// Whether F_GETFL shows the guest's O_NONBLOCK on `fd`.
bool nonblocking(std::int64_t fd) {
    return (call(nr_fcntl, {unsigned_of(fd), f_getfl}) & static_cast<std::int64_t>(o_nonblock)) != 0;
}

/// Runs `action` on a thread of its own 50 ms from now; the thread goes with the object.
class Later {
public:
    template <typename Action>
    explicit Later(Action action)
        : thread_([action] {
              std::this_thread::sleep_for(50ms);
              action();
          }) {}
    ~Later() { thread_.join(); }
    Later(const Later &) = delete;
    Later &operator=(const Later &) = delete;

private:
    std::thread thread_;
};

TEST(o_nonblock_is_the_guests_own_and_a_blocking_read_still_waits) {
    Table table;
    int quick[2] = {-1, -1}, slow[2] = {-1, -1};
    CHECK(call(nr_pipe2, {address(quick), o_nonblock}) == 0);
    CHECK(call(nr_pipe2, {address(slow), 0}) == 0);
    char byte = 0;
    CHECK(call(nr_read, {unsigned_of(quick[0]), address(&byte), 1}) == -eagain);
    CHECK(nonblocking(quick[0]) && nonblocking(quick[1]));
    CHECK(!nonblocking(slow[0]) && !nonblocking(slow[1]));
    // Every host descriptor of a pipe is non-blocking, whatever the guest's flag says.
    for (const int fd : {quick[0], quick[1], slow[0], slow[1]}) CHECK((host_status_flags(fd) & O_NONBLOCK) != 0);

    // A blocking read of the other pipe still waits: for the byte another thread writes later.
    const auto start = std::chrono::steady_clock::now();
    {
        const Later writer([&] { call(nr_write, {unsigned_of(slow[1]), address("w"), 1}); });
        CHECK(call(nr_read, {unsigned_of(slow[0]), address(&byte), 1}) == 1 && byte == 'w');
    }
    CHECK(std::chrono::steady_clock::now() - start >= 40ms);

    // F_SETFL and FIONBIO change the guest's flag only.
    CHECK(call(nr_fcntl, {unsigned_of(slow[0]), f_setfl, o_nonblock}) == 0);
    CHECK(nonblocking(slow[0]));
    CHECK(call(nr_read, {unsigned_of(slow[0]), address(&byte), 1}) == -eagain);
    const int off = 0, on = 1;
    CHECK(call(nr_ioctl, {unsigned_of(slow[0]), fionbio, address(&off)}) == 0);
    CHECK(!nonblocking(slow[0]));
    CHECK(call(nr_ioctl, {unsigned_of(slow[1]), fionbio, address(&on)}) == 0);
    CHECK(nonblocking(slow[1]));
    for (const int fd : {slow[0], slow[1]}) CHECK((host_status_flags(fd) & O_NONBLOCK) != 0);

    // The flag belongs to the open file: a duplicate shares it.
    const auto copy = call(nr_dup, {unsigned_of(slow[0])});
    CHECK(call(nr_fcntl, {unsigned_of(copy), f_setfl, o_nonblock}) == 0);
    CHECK(nonblocking(slow[0]));
    CHECK(call(nr_read, {unsigned_of(slow[0]), address(&byte), 1}) == -eagain);
    CHECK(table.holds_its_own());
}

TEST(a_terminal_can_block_and_a_regular_file_keeps_the_guests_o_nonblock) {
    Table table;
    // A terminal's host descriptor is non-blocking too, and the guest's flag stays apart.
    const int master = ::posix_openpt(O_RDWR | O_NOCTTY);
    CHECK(master >= 0 && ::grantpt(master) == 0 && ::unlockpt(master) == 0);
    const auto terminal = guest_number_of(::open(::ptsname(master), O_RDWR | O_NOCTTY));
    CHECK(terminal >= 0);
    CHECK((host_status_flags(terminal) & O_NONBLOCK) != 0);
    CHECK(!nonblocking(terminal));
    CHECK(call(nr_close, {unsigned_of(terminal)}) == 0);
    ::close(master);
    // A regular file cannot block, and F_GETFL still shows the O_NONBLOCK it was opened with.
    const auto file = open_guest("plain", o_creat | o_rdwr | o_nonblock);
    CHECK(file >= 0 && nonblocking(file));
    const auto blocking_file = open_guest("plain", o_rdwr);
    CHECK(blocking_file >= 0 && !nonblocking(blocking_file));
    CHECK(table.holds_its_own());
}

TEST(a_standard_descriptor_starts_with_the_starters_o_nonblock) {
    CHECK(in_child([] {
        ::alarm(5);  // a read that blocks for good ends the child, and with it the check
        int in[2];
        if (::pipe(in) != 0 || ::dup2(in[0], STDIN_FILENO) != STDIN_FILENO) ::_exit(3);
        ::fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK);
        apkrt::descriptors::reset();
        apkrt::descriptors::adopt_standard_descriptors();
        CHECK(nonblocking(0));
        char byte = 0;
        CHECK(call(nr_read, {0, address(&byte), 1}) == -eagain);
        apkrt::descriptors::reset();
    }));
}

TEST(the_guests_o_nonblock_on_a_standard_descriptor_leaves_the_starters_flags_alone) {
    CHECK(in_child([] {
        ::alarm(5);  // a read that blocks for good ends the child, and with it the check
        int in[2];
        if (::pipe(in) != 0 || ::dup2(in[0], STDIN_FILENO) != STDIN_FILENO) ::_exit(3);
        apkrt::descriptors::reset();
        apkrt::descriptors::adopt_standard_descriptors();
        char byte = 0;
        CHECK(call(nr_fcntl, {0, f_setfl, o_nonblock}) == 0);
        CHECK(nonblocking(0));
        CHECK((::fcntl(STDIN_FILENO, F_GETFL) & O_NONBLOCK) == 0);  // the starter's stays blocking
        CHECK(call(nr_read, {0, address(&byte), 1}) == -eagain);
        CHECK(::write(in[1], "i", 1) == 1);
        CHECK(call(nr_read, {0, address(&byte), 1}) == 1 && byte == 'i');

        // Blocking again, the read waits for the byte that comes later.
        CHECK(call(nr_fcntl, {0, f_setfl, 0}) == 0);
        CHECK(!nonblocking(0));
        const auto start = std::chrono::steady_clock::now();
        {
            const Later writer([&] { static_cast<void>(::write(in[1], "j", 1)); });
            CHECK(call(nr_read, {0, address(&byte), 1}) == 1 && byte == 'j');
        }
        CHECK(std::chrono::steady_clock::now() - start >= 40ms);
        apkrt::descriptors::reset();
    }));
}

/// Fills the host pipe that `host` writes to until not a byte more fits; `host` blocks again after.
void fill(int host) {
    ::fcntl(host, F_SETFL, O_NONBLOCK);
    static const char chunk[4096] = {};
    for (const std::size_t size : {sizeof chunk, std::size_t{1}}) {
        while (::write(host, chunk, size) > 0) {
        }
    }
    ::fcntl(host, F_SETFL, 0);
}

TEST(a_blocking_transfer_on_a_standard_descriptor_ends_at_once_while_a_signal_is_deliverable) {
    CHECK(in_child([] {
        ::alarm(5);  // a read or write that blocks for good ends the child, and with it the check
        int in[2], out[2];
        if (::pipe(in) != 0 || ::pipe(out) != 0) ::_exit(3);
        if (::dup2(in[0], STDIN_FILENO) != STDIN_FILENO || ::dup2(out[1], STDOUT_FILENO) != STDOUT_FILENO) {
            ::_exit(3);
        }
        fill(out[1]);  // the starter's standard output holds not a byte more
        apkrt::descriptors::reset();
        apkrt::descriptors::adopt_standard_descriptors();

        auto &state = apkrt::signals::current();
        constexpr std::uint64_t sigusr1 = std::uint64_t{1} << 9;
        state.pending.fetch_or(sigusr1);
        char byte = 0;
        // What is there to read comes first, as on Linux; then the read ends with EINTR at once.
        CHECK(::write(in[1], "d", 1) == 1);
        CHECK(call(nr_read, {0, address(&byte), 1}) == 1 && byte == 'd');
        CHECK(call(nr_read, {0, address(&byte), 1}) == -eintr);
        CHECK(call(nr_write, {1, address("w"), 1}) == -eintr);
        state.pending.fetch_and(~sigusr1);

        // A signal sent during the wait ends it as well, without the runtime signal.
        {
            const Later sender([&state] {
                state.pending.fetch_or(sigusr1);
                apkrt::signals::notify(state);
            });
            CHECK(call(nr_read, {0, address(&byte), 1}) == -eintr);
        }
        state.pending.fetch_and(~sigusr1);
        apkrt::descriptors::reset();
    }));
}

TEST(a_write_to_a_standard_descriptor_that_does_not_fit_takes_what_fits_and_ends) {
    // As Linux's pipe_write, with a signal deliverable or the guest's O_NONBLOCK: the bytes that fit,
    // at once, never a wait in the host call for the rest.
    CHECK(in_child([] {
        ::alarm(5);  // a write that blocks for good ends the child, and with it the check
        int out[2];
        if (::pipe(out) != 0 || ::dup2(out[1], STDOUT_FILENO) != STDOUT_FILENO) ::_exit(3);
        static char drained[4096];
        const auto make_room = [&] { CHECK(::read(out[0], drained, sizeof drained) == 4096); };
        fill(out[1]);
        make_room();  // 4,096 bytes free
        apkrt::descriptors::reset();
        apkrt::descriptors::adopt_standard_descriptors();
        static const char big[65536] = {};

        auto &state = apkrt::signals::current();
        constexpr std::uint64_t sigusr1 = std::uint64_t{1} << 9;
        state.pending.fetch_or(sigusr1);
        CHECK(call(nr_write, {1, address(big), sizeof big}) == 4096);
        state.pending.fetch_and(~sigusr1);

        make_room();
        CHECK(call(nr_fcntl, {1, f_setfl, o_nonblock}) == 0);
        CHECK(call(nr_write, {1, address(big), sizeof big}) == 4096);

        // The same through writev, whose vectors the room ends within the second.
        make_room();
        const iovec vectors[3] = {{const_cast<char *>(big), 1000}, {const_cast<char *>(big), 30000},
                                  {const_cast<char *>(big), 30000}};
        CHECK(call(nr_writev, {1, address(vectors), 3}) == 4096);
        apkrt::descriptors::reset();
    }));
}

TEST(a_write_to_a_standard_descriptor_whose_reader_goes_raises_sigpipe_with_its_count) {
    // As Linux's pipe_write: the bytes that went out, and SIGPIPE all the same.
    CHECK(in_child([] {
        ::alarm(5);  // a write that waits for good ends the child, and with it the check
        // The program's first thread, which the SIGPIPE of a host thread's syscall goes to (sys_gettid).
        static apkrt::threads::GuestThread first{};
        first.tid = ::getpid();
        apkrt::threads::add(first);
        ::signal(SIGPIPE, SIG_IGN);  // as the runtime has it (signals.cpp): EPIPE instead
        int out[2];
        if (::pipe(out) != 0 || ::dup2(out[1], STDOUT_FILENO) != STDOUT_FILENO) ::_exit(3);
        fill(out[1]);
        static char drained[4096];
        CHECK(::read(out[0], drained, sizeof drained) == 4096);  // room for 4,096 bytes
        apkrt::descriptors::reset();
        apkrt::descriptors::adopt_standard_descriptors();
        static const char big[65536] = {};
        {
            const Later reader_goes([&] { ::close(out[0]); });
            CHECK(call(nr_write, {1, address(big), sizeof big}) == 4096);
        }
        constexpr std::uint64_t sigpipe = std::uint64_t{1} << 12;
        CHECK((first.signals.pending.load() & sigpipe) != 0);
        apkrt::descriptors::reset();
    }));
}

TEST(a_read_on_a_standard_descriptor_takes_what_is_there_without_a_kqueue) {
    // With no descriptor left for the thread's kqueue, the read makes the blocking host call, as
    // before the waits on descriptors.
    CHECK(in_child([] {
        ::alarm(5);  // a read that waits for good ends the child, and with it the check
        int in[2];
        if (::pipe(in) != 0 || ::dup2(in[0], STDIN_FILENO) != STDIN_FILENO) ::_exit(3);
        CHECK(::write(in[1], "k", 1) == 1);
        apkrt::descriptors::reset();
        apkrt::descriptors::adopt_standard_descriptors();
        const rlimit few{64, 64};
        CHECK(::setrlimit(RLIMIT_NOFILE, &few) == 0);
        while (::open("/dev/null", O_RDONLY) >= 0) {
        }
        char byte = 0;
        CHECK(call(nr_read, {0, address(&byte), 1}) == 1 && byte == 'k');
    }));
}

TEST(vector_calls_with_no_vectors_answer_0) {
    // Linux answers 0 for readv and writev with no vectors; Darwin answers EINVAL.
    Table table;
    int ends[2] = {-1, -1};
    CHECK(call(nr_pipe2, {address(ends), 0}) == 0);
    const auto file = open_guest("vectors", o_creat | o_rdwr);
    iovec none{nullptr, 0};
    for (const std::int64_t fd : {std::int64_t{ends[0]}, file}) CHECK(call(nr_readv, {unsigned_of(fd), address(&none), 0}) == 0);
    for (const std::int64_t fd : {std::int64_t{ends[1]}, file}) CHECK(call(nr_writev, {unsigned_of(fd), address(&none), 0}) == 0);
    CHECK(table.holds_its_own());
}

}  // namespace

int main() {
    return apk::testing::run_all();
}
