// Checks the guest's descriptor table (ADR-0027): the guest's numbers are Linux's, whatever the
// process holds on the host, and every call that takes a descriptor goes through the table. Each
// call goes through the dispatcher with Linux numbers, as a guest's svc veneer delivers it.
#include "descriptors.h"
#include "syscall_frame.h"
#include "vfs.h"

#include "check.h"

#include <fcntl.h>
#include <limits.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

// Linux AArch64 syscall numbers and constants (asm-generic headers).
constexpr std::uint64_t nr_getcwd = 17, nr_dup = 23, nr_dup3 = 24, nr_fcntl = 25, nr_ioctl = 29, nr_fstatfs = 44,
                        nr_ftruncate = 46, nr_fchdir = 50, nr_fchmod = 52, nr_openat = 56, nr_close = 57,
                        nr_pipe2 = 59, nr_getdents64 = 61, nr_lseek = 62, nr_read = 63, nr_write = 64,
                        nr_pread64 = 67, nr_fstat = 80, nr_fsync = 82, nr_utimensat = 88, nr_munmap = 215,
                        nr_mmap = 222;
constexpr std::uint64_t f_dupfd = 0, f_getfd = 1, f_setfd = 2, f_getfl = 3, f_setfl = 4, f_dupfd_cloexec = 1030;
constexpr std::uint64_t o_rdonly = 0, o_rdwr = 2, o_creat = 0100, o_append = 02000, o_cloexec = 02000000;
constexpr std::uint64_t at_fdcwd = static_cast<std::uint64_t>(-100);
constexpr std::uint64_t seek_set = 0, seek_cur = 1;
constexpr std::uint64_t prot_read = 1, map_private = 2;
constexpr std::uint64_t fionread = 0x541b;
constexpr std::int64_t ebadf = 9, einval = 22, emfile = 24;

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
/// change the process's standard descriptors or close numbers by the hundred.
template <typename Body>
bool in_child(Body body) {
    std::fflush(nullptr);
    const pid_t child = ::fork();
    if (child == 0) {
        body();
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

}  // namespace

int main() {
    return apk::testing::run_all();
}
