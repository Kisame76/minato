// Checks eventfd, epoll, ppoll and pselect6 (ADR-0027, decisions 8 to 10) as Linux has them: the counter
// of an eventfd, epoll's interests and events, and what poll and select report, with their timeouts and
// signal masks. Each call goes through the dispatcher with Linux numbers, as a guest's svc veneer
// delivers it; the tests with guest threads and handlers are in blocking_test.cpp.
#include "descriptors.h"
#include "signals.h"
#include "syscall_frame.h"

#include "check.h"

#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include <thread>

namespace {

using namespace std::chrono_literals;

// Linux AArch64 syscall numbers and constants (asm-generic headers, linux/eventfd.h, linux/eventpoll.h,
// linux/poll.h).
constexpr std::uint64_t nr_eventfd2 = 19, nr_epoll_create1 = 20, nr_epoll_ctl = 21, nr_epoll_pwait = 22, nr_dup = 23,
                        nr_dup3 = 24, nr_fcntl = 25, nr_ioctl = 29, nr_ftruncate = 46, nr_close = 57, nr_pipe2 = 59,
                        nr_lseek = 62, nr_read = 63, nr_write = 64, nr_readv = 65, nr_writev = 66, nr_pread64 = 67,
                        nr_fstat = 80, nr_fsync = 82, nr_mmap = 222, nr_epoll_pwait2 = 441;
constexpr std::uint64_t efd_semaphore = 1, efd_nonblock = 04000, efd_cloexec = 02000000;
constexpr std::uint64_t f_getfd = 1, f_getfl = 3, f_setfl = 4, o_rdwr = 2, o_nonblock = 04000, o_cloexec = 02000000;
constexpr std::uint64_t fionread = 0x541b, seek_cur = 1, seek_set = 0, prot_read = 1, map_private = 2;
constexpr std::uint64_t epoll_ctl_add = 1, epoll_ctl_del = 2, epoll_ctl_mod = 3;
constexpr std::uint32_t epollin = 0x1, epollout = 0x4, epollrdnorm = 0x40, epollwrnorm = 0x100,
                        epollexclusive = 1u << 28, epollwakeup = 1u << 29, epolloneshot = 1u << 30, epollet = 1u << 31;
constexpr std::int64_t eperm = 1, ebadf = 9, eagain = 11, efault = 14, eexist = 17, enodev = 19, einval = 22,
                       enotty = 25, espipe = 29, eloop = 40, eintr = 4, enoent = 2;

std::int64_t call(std::uint64_t number, std::initializer_list<std::uint64_t> arguments) {
    apkrt_syscall_frame frame{};
    std::size_t index = 0;
    for (const auto argument : arguments) frame.x[index++] = argument;
    frame.x[8] = number;
    apkrt_syscall_dispatch(&frame);
    return static_cast<std::int64_t>(frame.x[0]);
}

template <typename T>
std::uint64_t address(T *pointer) {
    return reinterpret_cast<std::uint64_t>(pointer);
}

std::uint64_t unsigned_of(std::int64_t value) {
    return static_cast<std::uint64_t>(value);
}

/// A fresh descriptor table with the standard descriptors, as every test starts with.
struct Table {
    Table() {
        apkrt::descriptors::reset();
        apkrt::descriptors::adopt_standard_descriptors();
    }
    ~Table() { apkrt::descriptors::reset(); }
    Table(const Table &) = delete;
    Table &operator=(const Table &) = delete;
};

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

/// struct timespec, the same on Linux arm64 and Darwin.
struct Timespec {
    std::int64_t seconds;
    std::int64_t nanoseconds;
};

/// A guest number for a host descriptor, taken into the table as the runtime takes one it opened.
std::int64_t guest_number_of(int host) {
    return apkrt::descriptors::add(host, false);
}

/// A regular file, /dev/null and a directory, as guest numbers.
struct Files {
    std::int64_t file = -1, null = -1, directory = -1;

    Files() {
        char pattern[] = "/tmp/apkrt-events-XXXXXX";
        const int host = ::mkstemp(pattern);
        ::unlink(pattern);
        file = guest_number_of(host);
        null = guest_number_of(::open("/dev/null", O_RDWR));
        directory = guest_number_of(::open("/tmp", O_RDONLY | O_DIRECTORY));
    }
};

/// Both ends of a pipe2 of the guest's.
struct Ends {
    std::int64_t read_end = -1, write_end = -1;
};

Ends make_pipe() {
    int ends[2] = {-1, -1};
    CHECK(call(nr_pipe2, {address(ends), 0}) == 0);
    return {ends[0], ends[1]};
}

std::int64_t write_byte(std::int64_t fd) {
    return call(nr_write, {unsigned_of(fd), address("b"), 1});
}

std::int64_t read_byte(std::int64_t fd) {
    char byte = 0;
    return call(nr_read, {unsigned_of(fd), address(&byte), 1});
}

std::int64_t close_number(std::int64_t fd) {
    return call(nr_close, {unsigned_of(fd)});
}

std::chrono::nanoseconds since(std::chrono::steady_clock::time_point start) {
    return std::chrono::steady_clock::now() - start;
}

constexpr std::uint64_t sigusr1_bit = std::uint64_t{1} << 9;

// --- eventfd (ADR-0027, decision 8, test 8) ---------------------------------------------------------------

std::int64_t eventfd(std::uint64_t initial, std::uint64_t flags) {
    return call(nr_eventfd2, {initial, flags});
}

/// A read of the counter: the value, or the negative error.
std::int64_t take(std::int64_t fd) {
    std::uint64_t value = 0;
    const auto result = call(nr_read, {unsigned_of(fd), address(&value), sizeof value});
    return result == 8 ? static_cast<std::int64_t>(value) : result;
}

std::int64_t add(std::int64_t fd, std::uint64_t value) {
    return call(nr_write, {unsigned_of(fd), address(&value), sizeof value});
}

TEST(an_eventfd_counts_what_is_written_and_a_read_takes_it_all) {
    Table table;
    const auto fd = eventfd(0, efd_nonblock | efd_cloexec);
    CHECK(fd == 3);
    CHECK(take(fd) == -eagain);
    CHECK(add(fd, 1) == 8 && add(fd, 2) == 8);
    CHECK(take(fd) == 3);
    CHECK(take(fd) == -eagain);
    // The initial value counts as a write.
    const auto started = eventfd(5, efd_nonblock);
    CHECK(take(started) == 5);
    // Read and write, and the flags as Linux shows them for an eventfd.
    CHECK(call(nr_fcntl, {unsigned_of(fd), f_getfl}) == static_cast<std::int64_t>(o_rdwr | o_nonblock));
    CHECK(call(nr_fcntl, {unsigned_of(fd), f_getfd}) == 1);
    CHECK(call(nr_fcntl, {unsigned_of(started), f_getfd}) == 0);
    CHECK(call(nr_fcntl, {unsigned_of(fd), f_setfl, 0}) == 0);
    CHECK(call(nr_fcntl, {unsigned_of(fd), f_getfl}) == static_cast<std::int64_t>(o_rdwr));
}

TEST(a_semaphore_eventfd_hands_out_one_at_a_time) {
    Table table;
    const auto fd = eventfd(2, efd_semaphore | efd_nonblock);
    CHECK(take(fd) == 1);
    CHECK(take(fd) == 1);
    CHECK(take(fd) == -eagain);
    CHECK(add(fd, 3) == 8);
    CHECK(take(fd) == 1);
}

TEST(an_eventfd_takes_eight_bytes_and_answers_einval_for_fewer) {
    Table table;
    const auto fd = eventfd(0, efd_nonblock);
    char small[8] = {};
    CHECK(call(nr_read, {unsigned_of(fd), address(small), 0}) == -einval);
    CHECK(call(nr_read, {unsigned_of(fd), address(small), 7}) == -einval);
    CHECK(call(nr_write, {unsigned_of(fd), address(small), 4}) == -einval);
    // More than eight: the first eight count.
    const std::uint64_t pair[2] = {5, 7};
    CHECK(call(nr_write, {unsigned_of(fd), address(pair), sizeof pair}) == 8);
    std::uint64_t large[2] = {0, 0};
    CHECK(call(nr_read, {unsigned_of(fd), address(large), sizeof large}) == 8 && large[0] == 5);
    // readv spreads the value over its vectors; writev writes each vector for itself, as Linux does for
    // a file without write_iter.
    CHECK(add(fd, 0x0102'0304'0506'0708) == 8);
    std::uint32_t low = 0, high = 0;
    const iovec halves[2] = {{&low, 4}, {&high, 4}};
    CHECK(call(nr_readv, {unsigned_of(fd), address(halves), 2}) == 8);
    CHECK(low == 0x0506'0708 && high == 0x0102'0304);
    const std::uint32_t four[2] = {1, 0};
    const iovec small_vectors[2] = {{const_cast<std::uint32_t *>(&four[0]), 4}, {const_cast<std::uint32_t *>(&four[1]), 4}};
    CHECK(call(nr_writev, {unsigned_of(fd), address(small_vectors), 2}) == -einval);
    const std::uint64_t one = 1, two = 2;
    const iovec vectors[2] = {{const_cast<std::uint64_t *>(&one), 8}, {const_cast<std::uint64_t *>(&two), 8}};
    CHECK(call(nr_writev, {unsigned_of(fd), address(vectors), 2}) == 16);
    CHECK(take(fd) == 3);
}

TEST(an_eventfd_stops_below_2_to_the_64_minus_1) {
    Table table;
    for (const std::uint64_t flags : {efd_nonblock, std::uint64_t{0}}) {
        const auto fd = eventfd(0, flags);
        CHECK(add(fd, UINT64_MAX) == -einval);
        CHECK(add(fd, UINT64_MAX - 1) == 8);
        // Linux waits here without EFD_NONBLOCK; no program comes close (ADR-0027, decision 8).
        CHECK(add(fd, 1) == -eagain);
        CHECK(add(fd, 0) == 8);
        CHECK(take(fd) == static_cast<std::int64_t>(UINT64_MAX - 1));
        CHECK(call(nr_close, {unsigned_of(fd)}) == 0);
    }
}

TEST(eventfd_takes_only_its_own_flags) {
    Table table;
    CHECK(eventfd(0, 8) == -einval);
    CHECK(eventfd(0, 02) == -einval);  // O_RDWR
    CHECK(eventfd(3, efd_semaphore | efd_cloexec | efd_nonblock) == 3);
}

TEST(an_eventfd_shows_an_anonymous_inode_and_refuses_what_linux_refuses) {
    Table table;
    const auto fd = eventfd(1, efd_nonblock);
    struct {
        std::uint64_t dev, ino;
        std::uint32_t mode, nlink, uid, gid;
        std::uint64_t rdev, pad1;
        std::int64_t size;
        std::int32_t blksize, pad2;
        std::int64_t blocks;
        std::uint8_t rest[56];
    } status{};
    static_assert(sizeof status == 128);
    CHECK(call(nr_fstat, {unsigned_of(fd), address(&status)}) == 0);
    CHECK(status.mode == 0600 && status.nlink == 1 && status.uid == 0 && status.gid == 0);
    CHECK(status.size == 0 && status.blksize == 4096 && status.blocks == 0 && status.rdev == 0);
    // noop_llseek: the position stays 0, whatever the arguments.
    CHECK(call(nr_lseek, {unsigned_of(fd), 5, seek_set}) == 0);
    CHECK(call(nr_lseek, {unsigned_of(fd), 0, seek_cur}) == 0);
    std::uint64_t value = 0;
    CHECK(call(nr_pread64, {unsigned_of(fd), address(&value), 8, 0}) == -espipe);
    CHECK(call(nr_mmap, {0, 16384, prot_read, map_private, unsigned_of(fd), 0}) == -enodev);
    int available = -1;
    CHECK(call(nr_ioctl, {unsigned_of(fd), fionread, address(&available)}) == -enotty);
    CHECK(call(nr_fsync, {unsigned_of(fd)}) == -einval);
    CHECK(call(nr_ftruncate, {unsigned_of(fd), 0}) == -einval);
    CHECK(take(fd) == 1);  // none of it took the count
}

TEST(a_blocking_eventfd_read_waits_for_a_write) {
    Table table;
    const auto fd = eventfd(0, 0);
    const auto start = std::chrono::steady_clock::now();
    {
        const Later writer([fd] { add(fd, 42); });
        CHECK(take(fd) == 42);
    }
    CHECK(std::chrono::steady_clock::now() - start >= 40ms);
}

// --- epoll (ADR-0027, decision 9, test 9) ---------------------------------------------------------------

/// struct epoll_event on arm64: 16 bytes, the data at offset 8 (packed only on x86_64).
struct EpollEvent {
    std::uint32_t events;
    std::uint64_t data;
};
static_assert(sizeof(EpollEvent) == 16);

std::int64_t epoll_create(std::uint64_t flags = 0) {
    return call(nr_epoll_create1, {flags});
}

std::int64_t control(std::int64_t epoll, std::uint64_t operation, std::int64_t fd, std::uint32_t events,
                     std::uint64_t data = 0) {
    EpollEvent event{events, data};
    return call(nr_epoll_ctl, {unsigned_of(epoll), operation, unsigned_of(fd), address(&event)});
}

std::int64_t epoll_wait(std::int64_t epoll, EpollEvent *events, int maximum, int timeout) {
    return call(nr_epoll_pwait, {unsigned_of(epoll), address(events), static_cast<std::uint64_t>(maximum),
                                 static_cast<std::uint64_t>(timeout), 0, 8});
}

/// What a wait of `timeout` ms on `epoll` reports, as "events/data" in the order of the data, in hex
/// and decimal; "error N" for -N.
std::string ready(std::int64_t epoll, int timeout = 0, int maximum = 8) {
    EpollEvent events[8] = {};
    const auto count = epoll_wait(epoll, events, maximum, timeout);
    if (count < 0) return "error " + std::to_string(-count);
    for (std::int64_t index = 1; index < count; ++index) {
        for (auto other = index; other > 0 && events[other - 1].data > events[other].data; --other) {
            std::swap(events[other], events[other - 1]);
        }
    }
    std::string text;
    for (std::int64_t index = 0; index < count; ++index) {
        char item[48];
        std::snprintf(item, sizeof item, "%s%x/%llu", text.empty() ? "" : " ", events[index].events,
                      static_cast<unsigned long long>(events[index].data));
        text += item;
    }
    return text;
}

TEST(a_level_triggered_interest_reports_while_its_file_is_ready) {
    Table table;
    const auto epoll = epoll_create();
    const auto pipe = make_pipe();
    CHECK(control(epoll, epoll_ctl_add, pipe.read_end, epollin, 10) == 0);
    CHECK(ready(epoll).empty());
    CHECK(write_byte(pipe.write_end) == 1);
    CHECK(ready(epoll) == "1/10");
    CHECK(ready(epoll) == "1/10");
    CHECK(read_byte(pipe.read_end) == 1);
    CHECK(ready(epoll).empty());
    // The data is the guest's, all 64 bits of it.
    CHECK(control(epoll, epoll_ctl_mod, pipe.read_end, epollin, 0xfedc'ba98'7654'3210) == 0);
    CHECK(write_byte(pipe.write_end) == 1);
    CHECK(ready(epoll) == "1/18364758544493064720");
}

TEST(an_edge_triggered_interest_reports_each_arrival_once) {
    Table table;
    const auto epoll = epoll_create();
    const auto pipe = make_pipe();
    CHECK(control(epoll, epoll_ctl_add, pipe.read_end, epollin | epollet, 10) == 0);
    CHECK(write_byte(pipe.write_end) == 1);
    CHECK(ready(epoll) == "1/10");
    CHECK(ready(epoll).empty());
    CHECK(write_byte(pipe.write_end) == 1);
    CHECK(ready(epoll) == "1/10");
    CHECK(ready(epoll).empty());
}

TEST(a_one_shot_interest_reports_once_until_mod_arms_it_again) {
    Table table;
    const auto epoll = epoll_create();
    const auto pipe = make_pipe();
    CHECK(control(epoll, epoll_ctl_add, pipe.read_end, epollin | epolloneshot, 11) == 0);
    CHECK(write_byte(pipe.write_end) == 1);
    CHECK(ready(epoll) == "1/11");
    CHECK(ready(epoll).empty());
    CHECK(write_byte(pipe.write_end) == 1);
    CHECK(ready(epoll).empty());
    CHECK(control(epoll, epoll_ctl_mod, pipe.read_end, epollin | epolloneshot, 12) == 0);
    CHECK(ready(epoll) == "1/12");
    CHECK(ready(epoll).empty());
    // One event disarms the whole interest: an eventfd reported writable is no longer readable either.
    const auto counter = eventfd(0, efd_nonblock);
    CHECK(control(epoll, epoll_ctl_add, counter, epollin | epollout | epolloneshot, 13) == 0);
    CHECK(ready(epoll) == "4/13");
    CHECK(add(counter, 1) == 8);
    CHECK(ready(epoll).empty());
    CHECK(control(epoll, epoll_ctl_mod, counter, epollin | epollout | epolloneshot, 14) == 0);
    CHECK(ready(epoll) == "5/14");
}

TEST(an_eventfd_is_writable_and_readable_while_its_counter_is_not_0) {
    Table table;
    const auto epoll = epoll_create();
    const auto counter = eventfd(0, efd_nonblock);
    CHECK(control(epoll, epoll_ctl_add, counter, epollin | epollout, 14) == 0);
    CHECK(ready(epoll) == "4/14");
    CHECK(add(counter, 1) == 8);
    CHECK(ready(epoll) == "5/14");
    // Edge-triggered, a write and a read each report what holds then, as eventfd_poll tells it.
    CHECK(control(epoll, epoll_ctl_mod, counter, epollin | epollout | epollet, 15) == 0);
    CHECK(ready(epoll) == "5/15");
    CHECK(ready(epoll).empty());
    CHECK(add(counter, 1) == 8);
    CHECK(ready(epoll) == "5/15");
    CHECK(take(counter) == 2);
    CHECK(ready(epoll) == "4/15");
    // An eventfd reports neither EPOLLRDNORM nor EPOLLWRNORM.
    CHECK(control(epoll, epoll_ctl_mod, counter, epollin | epollrdnorm | epollout | epollwrnorm, 16) == 0);
    CHECK(add(counter, 1) == 8);
    CHECK(ready(epoll) == "5/16");
    // At 2^64 - 2 it takes no write, and Linux reports no EPOLLOUT; here EPOLLOUT stays true (decision 8).
    CHECK(take(counter) == 1);
    CHECK(add(counter, UINT64_MAX - 1) == 8);
    CHECK(control(epoll, epoll_ctl_mod, counter, epollout, 17) == 0);
    CHECK(ready(epoll) == "4/17");
}

TEST(pipes_report_hangups_and_errors_whether_asked_or_not) {
    Table table;
    const auto epoll = epoll_create();
    auto pipe = make_pipe();
    CHECK(control(epoll, epoll_ctl_add, pipe.read_end, epollin, 20) == 0);
    CHECK(control(epoll, epoll_ctl_add, pipe.write_end, epollout, 21) == 0);
    CHECK(ready(epoll) == "4/21");
    CHECK(call(nr_write, {unsigned_of(pipe.write_end), address("ab"), 2}) == 2);
    CHECK(close_number(pipe.write_end) == 0);  // its interest goes with it
    CHECK(ready(epoll) == "11/20");
    CHECK(read_byte(pipe.read_end) == 1 && read_byte(pipe.read_end) == 1);
    CHECK(ready(epoll) == "10/20");
    CHECK(close_number(pipe.read_end) == 0);

    // EPOLLHUP and EPOLLERR come whatever the interest asks for.
    pipe = make_pipe();
    CHECK(control(epoll, epoll_ctl_add, pipe.read_end, 0, 30) == 0);
    CHECK(write_byte(pipe.write_end) == 1);
    CHECK(ready(epoll).empty());  // the byte is not asked for
    CHECK(close_number(pipe.write_end) == 0);
    CHECK(ready(epoll) == "10/30");
    CHECK(close_number(pipe.read_end) == 0);
    pipe = make_pipe();
    CHECK(control(epoll, epoll_ctl_add, pipe.write_end, epollin, 31) == 0);
    CHECK(ready(epoll).empty());
    CHECK(close_number(pipe.read_end) == 0);
    CHECK(ready(epoll) == "8/31");
    CHECK(close_number(pipe.write_end) == 0);
    pipe = make_pipe();
    CHECK(control(epoll, epoll_ctl_add, pipe.write_end, epollout, 32) == 0);
    CHECK(close_number(pipe.read_end) == 0);
    CHECK(ready(epoll) == "c/32");
    CHECK(close_number(pipe.write_end) == 0);

    // What the interest asks for is what it gets: EPOLLRDNORM alone.
    pipe = make_pipe();
    CHECK(control(epoll, epoll_ctl_add, pipe.read_end, epollrdnorm, 33) == 0);
    CHECK(write_byte(pipe.write_end) == 1);
    CHECK(ready(epoll) == "40/33");
}

TEST(epoll_ctl_answers_linuxs_errors_in_linuxs_order) {
    Table table;
    const Files files;
    const auto epoll = epoll_create();
    const auto counter = eventfd(0, 0), other = eventfd(0, 0);
    CHECK(control(epoll, epoll_ctl_add, files.file, epollin) == -eperm);
    CHECK(control(epoll, epoll_ctl_add, files.null, epollin) == -eperm);
    CHECK(control(epoll, epoll_ctl_add, files.directory, epollin) == -eperm);
    CHECK(control(epoll, epoll_ctl_add, epoll, epollin) == -einval);
    CHECK(control(epoll, epoll_ctl_add, 900, epollin) == -ebadf);
    CHECK(control(900, epoll_ctl_add, counter, epollin) == -ebadf);
    CHECK(control(counter, epoll_ctl_add, other, epollin) == -einval);  // not an epoll
    CHECK(control(counter, epoll_ctl_add, files.file, epollin) == -eperm);  // the file is looked at first
    CHECK(control(epoll, 99, counter, epollin) == -einval);
    CHECK(control(epoll, epoll_ctl_add, counter, epollin) == 0);
    CHECK(control(epoll, epoll_ctl_add, counter, epollin) == -eexist);
    CHECK(control(epoll, epoll_ctl_mod, other, epollin) == -enoent);
    CHECK(call(nr_epoll_ctl, {unsigned_of(epoll), epoll_ctl_del, unsigned_of(other), 0}) == -enoent);
    CHECK(call(nr_epoll_ctl, {unsigned_of(epoll), epoll_ctl_add, unsigned_of(other), 0}) == -efault);
    CHECK(call(nr_epoll_ctl, {unsigned_of(epoll), epoll_ctl_del, unsigned_of(counter), 0}) == 0);  // needs none

    // EPOLLEXCLUSIVE is taken where Linux takes it, and changes nothing; so is EPOLLWAKEUP.
    CHECK(control(epoll, epoll_ctl_add, counter, epollin | epollexclusive) == 0);
    CHECK(control(epoll, epoll_ctl_mod, counter, epollin | epollexclusive) == -einval);
    CHECK(control(epoll, epoll_ctl_mod, counter, epollin) == -einval);  // an exclusive interest stays as it is
    CHECK(control(epoll, epoll_ctl_add, other, epollin | epollexclusive | epolloneshot) == -einval);
    const auto inner = epoll_create();
    CHECK(control(epoll, epoll_ctl_add, inner, epollin | epollexclusive) == -einval);
    CHECK(control(epoll, epoll_ctl_add, other, epollin | epollwakeup) == 0);
    CHECK(control(epoll, epoll_ctl_mod, other, epollin | epollexclusive) == -einval);  // never with MOD
    CHECK(add(other, 1) == 8);
    CHECK(ready(epoll) == "1/0");

    CHECK(epoll_create(1) == -einval);
    const auto closing = epoll_create(o_cloexec);
    CHECK(call(nr_fcntl, {unsigned_of(closing), f_getfd}) == 1);
}

TEST(an_epoll_answers_linuxs_errors_and_is_an_anonymous_file) {
    Table table;
    const auto epoll = epoll_create();
    const auto counter = eventfd(0, 0);
    EpollEvent events[1];
    CHECK(epoll_wait(epoll, events, 0, 0) == -einval);
    CHECK(epoll_wait(epoll, events, -1, 0) == -einval);
    CHECK(epoll_wait(counter, events, 1, 0) == -einval);
    CHECK(epoll_wait(900, events, 1, 0) == -ebadf);
    CHECK(call(nr_epoll_pwait, {unsigned_of(epoll), 0, 1, 0, 0, 8}) == -efault);
    const std::uint64_t mask = 0;
    CHECK(call(nr_epoll_pwait, {unsigned_of(epoll), address(events), 1, 0, address(&mask), 4}) == -einval);
    const Timespec invalid{0, 1'000'000'000};
    CHECK(call(nr_epoll_pwait2, {unsigned_of(epoll), address(events), 1, address(&invalid), 0, 8}) == -einval);
    // No read or write; the rest as for an eventfd.
    std::uint64_t value = 0;
    CHECK(call(nr_read, {unsigned_of(epoll), address(&value), 8}) == -einval);
    CHECK(call(nr_write, {unsigned_of(epoll), address(&value), 8}) == -einval);
    CHECK(call(nr_fcntl, {unsigned_of(epoll), f_getfl}) == static_cast<std::int64_t>(o_rdwr));
    std::uint32_t status[32] = {};
    CHECK(call(nr_fstat, {unsigned_of(epoll), address(status)}) == 0 && status[4] == 0600);
    CHECK(call(nr_lseek, {unsigned_of(epoll), 0, seek_cur}) == 0);
    CHECK(call(nr_mmap, {0, 16384, prot_read, map_private, unsigned_of(epoll), 0}) == -enodev);
    int available = -1;
    CHECK(call(nr_ioctl, {unsigned_of(epoll), fionread, address(&available)}) == -enotty);
}

TEST(an_interest_goes_with_its_open_file_not_with_its_number) {
    Table table;
    const auto epoll = epoll_create();
    const auto pipe = make_pipe();
    const auto copy = call(nr_dup, {unsigned_of(pipe.read_end)});
    CHECK(control(epoll, epoll_ctl_add, pipe.read_end, epollin, 30) == 0);
    CHECK(control(epoll, epoll_ctl_add, copy, epollin, 31) == 0);
    CHECK(write_byte(pipe.write_end) == 1);
    CHECK(ready(epoll) == "1/30 1/31");
    // Closing one number leaves both interests: the file is still open.
    CHECK(close_number(pipe.read_end) == 0);
    CHECK(ready(epoll) == "1/30 1/31");
    CHECK(call(nr_epoll_ctl, {unsigned_of(epoll), epoll_ctl_del, unsigned_of(pipe.read_end), 0}) == -ebadf);
    CHECK(call(nr_dup3, {unsigned_of(copy), unsigned_of(pipe.read_end), 0}) == pipe.read_end);
    CHECK(control(epoll, epoll_ctl_add, pipe.read_end, epollin, 32) == -eexist);  // the same file, the same number
    // The last number takes them along.
    CHECK(close_number(pipe.read_end) == 0 && close_number(copy) == 0);
    CHECK(ready(epoll).empty());
    const auto again = make_pipe();
    CHECK(again.read_end == pipe.read_end);
    CHECK(control(epoll, epoll_ctl_add, again.read_end, epollin, 33) == 0);
    CHECK(write_byte(again.write_end) == 1);
    CHECK(ready(epoll) == "1/33");
}

TEST(two_numbers_of_one_file_are_interests_of_their_own_in_modes_of_their_own) {
    Table table;
    const auto epoll = epoll_create();
    const auto pipe = make_pipe();
    const auto copy = call(nr_dup, {unsigned_of(pipe.read_end)});
    CHECK(control(epoll, epoll_ctl_add, pipe.read_end, epollin | epollet, 1) == 0);
    CHECK(control(epoll, epoll_ctl_add, copy, epollin, 2) == 0);
    CHECK(write_byte(pipe.write_end) == 1);
    CHECK(ready(epoll) == "1/1 1/2");
    CHECK(ready(epoll) == "1/2");  // level-triggered: again, edge-triggered: not
    CHECK(control(epoll, epoll_ctl_mod, copy, epollin | epolloneshot, 3) == 0);
    CHECK(ready(epoll) == "1/3");
    CHECK(ready(epoll).empty());
    // A change of one leaves the other alone: no new edge for the first.
    CHECK(control(epoll, epoll_ctl_mod, copy, epollin | epolloneshot, 4) == 0);
    CHECK(ready(epoll) == "1/4");
    CHECK(call(nr_epoll_ctl, {unsigned_of(epoll), epoll_ctl_del, unsigned_of(pipe.read_end), 0}) == 0);
    CHECK(control(epoll, epoll_ctl_mod, copy, epollin, 5) == 0);
    CHECK(write_byte(pipe.write_end) == 1);
    CHECK(ready(epoll) == "1/5");
}

TEST(a_wait_reports_no_more_events_than_asked_and_keeps_the_rest) {
    Table table;
    const auto epoll = epoll_create();
    const auto first = make_pipe(), second = make_pipe();
    CHECK(control(epoll, epoll_ctl_add, first.read_end, epollin | epollet, 1) == 0);
    CHECK(control(epoll, epoll_ctl_add, second.read_end, epollin | epollet, 2) == 0);
    CHECK(write_byte(first.write_end) == 1 && write_byte(second.write_end) == 1);
    const auto one = ready(epoll, 0, 1);
    const auto other = ready(epoll, 0, 1);
    CHECK((one == "1/1" && other == "1/2") || (one == "1/2" && other == "1/1"));
    CHECK(ready(epoll).empty());
    // Two numbers of one file share its filter, which fires once for both: the second event waits.
    const auto copy = call(nr_dup, {unsigned_of(first.read_end)});
    CHECK(control(epoll, epoll_ctl_add, copy, epollin | epollet, 3) == 0);
    CHECK(write_byte(first.write_end) == 1);
    const auto third = ready(epoll, 0, 1);
    const auto fourth = ready(epoll, 0, 1);
    CHECK((third == "1/1" && fourth == "1/3") || (third == "1/3" && fourth == "1/1"));
    CHECK(ready(epoll).empty());
}

/// An epoll with two numbers of one pipe's read end, data 1 and 2, in the mode `mode`, and a byte to read.
struct TwoNumbers {
    std::int64_t epoll = -1, copy = -1;
    Ends pipe;

    explicit TwoNumbers(std::uint32_t mode) {
        epoll = epoll_create();
        pipe = make_pipe();
        copy = call(nr_dup, {unsigned_of(pipe.read_end)});
        CHECK(control(epoll, epoll_ctl_add, pipe.read_end, epollin | mode, 1) == 0);
        CHECK(control(epoll, epoll_ctl_add, copy, epollin | mode, 2) == 0);
        CHECK(write_byte(pipe.write_end) == 1);
    }

    /// The number whose event did not fit into a wait that reported `reported`.
    std::int64_t other_than(const std::string &reported) const { return reported == "1/1" ? copy : pipe.read_end; }
};

TEST(an_event_that_waits_for_room_goes_with_its_interest) {
    Table table;
    TwoNumbers removed(epollet);
    const auto first = ready(removed.epoll, 0, 1);
    CHECK(call(nr_epoll_ctl, {unsigned_of(removed.epoll), epoll_ctl_del, unsigned_of(removed.other_than(first)), 0}) == 0);
    CHECK(ready(removed.epoll).empty());
    // A change looks at the file anew and reports with the new data, never the old, and for itself alone.
    TwoNumbers changed(epollet);
    const auto second = ready(changed.epoll, 0, 1);
    CHECK(control(changed.epoll, epoll_ctl_mod, changed.other_than(second), epollin | epollet, 22) == 0);
    CHECK(ready(changed.epoll) == "1/22");
}

TEST(a_level_triggered_event_that_did_not_fit_comes_once_and_in_turn) {
    // Linux reports an item once per call, and puts a level-triggered one back at the end of its list.
    Table table;
    TwoNumbers level(0);
    CHECK(!ready(level.epoll, 0, 1).empty());
    CHECK(ready(level.epoll, 0, 3) == "1/1 1/2");
    const auto first = ready(level.epoll, 0, 1);
    const auto second = ready(level.epoll, 0, 1);
    CHECK((first == "1/1" && second == "1/2") || (first == "1/2" && second == "1/1"));
}

TEST(an_epoll_inside_an_epoll_reports_the_events_that_wait_for_room) {
    Table table;
    TwoNumbers inner(epollet);
    const auto outer = epoll_create();
    CHECK(control(outer, epoll_ctl_add, inner.epoll, epollin, 41) == 0);
    CHECK(!ready(inner.epoll, 0, 1).empty());
    CHECK(ready(outer) == "1/41");
    CHECK(!ready(inner.epoll, 0, 1).empty());
    CHECK(ready(inner.epoll).empty());
}

TEST(an_edge_triggered_eventfd_reports_every_write_however_many_it_holds) {
    // A semaphore that a reader takes one at a time: every write is an edge, as on Linux, also after
    // more writes than its pipe holds bytes for, and it stays writable.
    Table table;
    const auto epoll = epoll_create();
    const auto counter = eventfd(0, efd_semaphore | efd_nonblock);
    CHECK(control(epoll, epoll_ctl_add, counter, epollin | epollet, 5) == 0);
    bool written = true;
    for (int time = 0; time < 70'000; ++time) written = written && add(counter, 1) == 8;
    CHECK(written);
    CHECK(ready(epoll) == "1/5");
    CHECK(take(counter) == 1);
    CHECK(ready(epoll).empty());
    CHECK(add(counter, 1) == 8);
    CHECK(ready(epoll) == "1/5");
    CHECK(control(epoll, epoll_ctl_mod, counter, epollout, 6) == 0);
    CHECK(ready(epoll) == "4/6");
}

TEST(a_wait_reports_as_many_events_as_it_has_room_for) {
    Table table;
    const auto epoll = epoll_create();
    constexpr int files = 70;
    for (int index = 0; index < files; ++index) {
        const auto pipe = make_pipe();
        CHECK(control(epoll, epoll_ctl_add, pipe.read_end, epollin, static_cast<std::uint64_t>(index)) == 0);
        CHECK(write_byte(pipe.write_end) == 1);
    }
    EpollEvent events[100] = {};
    CHECK(epoll_wait(epoll, events, 100, 0) == files);
}

TEST(an_epoll_inside_an_epoll_reports_when_the_inner_one_has_events) {
    Table table;
    const auto inner = epoll_create(), outer = epoll_create();
    const auto pipe = make_pipe();
    CHECK(control(inner, epoll_ctl_add, pipe.read_end, epollin, 40) == 0);
    CHECK(control(outer, epoll_ctl_add, inner, epollin | epollout, 41) == 0);
    CHECK(ready(outer).empty());
    CHECK(write_byte(pipe.write_end) == 1);
    CHECK(ready(outer) == "1/41");
    CHECK(ready(inner) == "1/40");
    // A loop, and a chain deeper than Linux allows, answer ELOOP.
    CHECK(control(inner, epoll_ctl_add, outer, epollin, 42) == -eloop);
    std::int64_t chain[8];
    for (auto &epoll : chain) epoll = epoll_create();
    for (int level = 1; level < 7; ++level) CHECK(control(chain[level], epoll_ctl_add, chain[level - 1], epollin) == 0);
    CHECK(control(chain[7], epoll_ctl_add, chain[6], epollin) == -eloop);
}

TEST(an_epoll_wait_ends_after_its_timeout_or_with_the_event_that_comes) {
    Table table;
    const auto epoll = epoll_create();
    EpollEvent events[1];
    auto start = std::chrono::steady_clock::now();
    CHECK(epoll_wait(epoll, events, 1, 0) == 0);
    CHECK(epoll_wait(epoll, events, 1, 30) == 0);
    CHECK(since(start) >= 30ms);
    start = std::chrono::steady_clock::now();
    const Timespec thirty_ms{0, 30'000'000};
    CHECK(call(nr_epoll_pwait2, {unsigned_of(epoll), address(events), 1, address(&thirty_ms), 0, 8}) == 0);
    CHECK(since(start) >= 30ms);
    // Without a timeout, epoll_pwait with -1 and epoll_pwait2 with none wait for the event.
    const auto pipe = make_pipe();
    CHECK(control(epoll, epoll_ctl_add, pipe.read_end, epollin | epollet, 7) == 0);
    start = std::chrono::steady_clock::now();
    {
        const Later writer([&] { write_byte(pipe.write_end); });
        CHECK(epoll_wait(epoll, events, 1, -1) == 1 && events[0].events == epollin && events[0].data == 7);
    }
    CHECK(since(start) >= 40ms);
    start = std::chrono::steady_clock::now();
    {
        const Later writer([&] { write_byte(pipe.write_end); });
        CHECK(call(nr_epoll_pwait2, {unsigned_of(epoll), address(events), 1, 0, 0, 8}) == 1 && events[0].data == 7);
    }
    CHECK(since(start) >= 40ms);
}

TEST(epoll_pwaits_mask_holds_during_the_wait_only) {
    Table table;
    const auto epoll = epoll_create();
    EpollEvent events[1];
    auto &state = apkrt::signals::current();
    // A pending signal the thread blocks, which the mask lets in, ends the wait at once. As with
    // rt_sigsuspend, the handler's frame returns to the thread's own mask, the temporary one holds
    // until the delivery.
    state.mask.store(sigusr1_bit);
    state.pending.fetch_or(sigusr1_bit);
    const std::uint64_t open = 0;
    CHECK(call(nr_epoll_pwait, {unsigned_of(epoll), address(events), 1, 5000, address(&open), 8}) == -eintr);
    CHECK(state.mask.load() == 0 && state.frame_mask_set && state.frame_mask == sigusr1_bit);
    state.frame_mask_set = false;
    state.mask.store(0);
    // A pending signal the mask blocks ends no wait, and the thread's own mask comes back after it.
    const std::uint64_t blocking = sigusr1_bit;
    CHECK(call(nr_epoll_pwait, {unsigned_of(epoll), address(events), 1, 20, address(&blocking), 8}) == 0);
    CHECK(state.mask.load() == 0 && !state.frame_mask_set);
    state.pending.fetch_and(~sigusr1_bit);
}

// --- ppoll and pselect6 (ADR-0027, decision 10, test 10) ------------------------------------------------

constexpr std::uint64_t nr_pselect6 = 72, nr_ppoll = 73;
constexpr std::int16_t pollin = 0x1, pollpri = 0x2, pollout = 0x4, pollrdnorm = 0x40, pollwrnorm = 0x100;

/// struct pollfd, the same on Linux and Darwin.
struct PollFd {
    std::int32_t fd;
    std::int16_t events;
    std::int16_t revents;
};

/// ppoll of `fds` without a mask, with a timeout of `milliseconds` (-1: none): "count: revents ...", in
/// hex; "error N" for -N. Each revents starts out as 0x7777, which ppoll must overwrite.
std::string polled(std::initializer_list<PollFd> list, int milliseconds = 0) {
    std::vector<PollFd> fds(list);
    for (auto &entry : fds) entry.revents = 0x7777;
    Timespec timeout{milliseconds / 1000, (milliseconds % 1000) * 1'000'000};
    const auto count = call(nr_ppoll, {address(fds.data()), fds.size(), milliseconds < 0 ? 0 : address(&timeout), 0, 8});
    if (count < 0) return "error " + std::to_string(-count);
    std::string text = std::to_string(count) + ":";
    for (const auto &entry : fds) {
        char item[16];
        std::snprintf(item, sizeof item, " %x", static_cast<unsigned>(static_cast<std::uint16_t>(entry.revents)));
        text += item;
    }
    return text;
}

PollFd entry(std::int64_t fd, std::int16_t events) {
    return {static_cast<std::int32_t>(fd), events, 0};
}

TEST(ppoll_reports_pipes_and_eventfds_as_linux_does) {
    Table table;
    const auto pipe = make_pipe();
    const auto counter = eventfd(0, efd_nonblock);
    CHECK(polled({entry(pipe.read_end, pollin), entry(counter, pollin), entry(pipe.write_end, pollout)}) == "1: 0 0 4");
    CHECK(polled({entry(pipe.read_end, pollin), entry(counter, pollin), entry(pipe.write_end, 0)}) == "0: 0 0 0");
    CHECK(add(counter, 3) == 8);
    CHECK(polled({entry(pipe.read_end, pollin), entry(counter, pollin | pollout)}) == "1: 0 5");
    CHECK(write_byte(pipe.write_end) == 1);
    CHECK(polled({entry(pipe.read_end, pollin | pollrdnorm), entry(counter, pollin | pollrdnorm)}) == "2: 41 1");
    CHECK(close_number(pipe.write_end) == 0);
    CHECK(polled({entry(pipe.read_end, pollin), entry(-1, pollin)}) == "1: 11 0");
    CHECK(read_byte(pipe.read_end) == 1);
    CHECK(polled({entry(pipe.read_end, 0), entry(pipe.write_end, pollin)}) == "2: 10 20");
    const auto broken = make_pipe();
    CHECK(close_number(broken.read_end) == 0);
    CHECK(polled({entry(broken.write_end, pollout | pollwrnorm), entry(broken.write_end, 0), entry(broken.write_end, pollin)}) ==
          "3: 10c 8 8");
    // At 2^64 - 2 an eventfd takes no write any more.
    CHECK(take(counter) == 3);
    CHECK(add(counter, UINT64_MAX - 1) == 8);
    CHECK(polled({entry(counter, pollin | pollout)}) == "1: 1");
}

TEST(ppoll_reports_files_and_devices_ready_at_once) {
    Table table;
    const Files files;
    const std::int16_t everything = pollin | pollout | pollrdnorm | pollwrnorm | pollpri;
    CHECK(polled({entry(files.file, everything), entry(files.null, everything), entry(files.directory, everything)}, 5000) ==
          "3: 145 145 145");
    CHECK(polled({entry(files.file, 0), entry(files.null, pollin)}) == "1: 0 1");
}

TEST(ppoll_answers_linuxs_errors) {
    Table table;
    const auto pipe = make_pipe();
    std::vector<PollFd> many(40000, entry(pipe.read_end, pollin));
    CHECK(call(nr_ppoll, {address(many.data()), many.size(), 0, 0, 8}) == -einval);
    const Timespec invalid{0, 1'000'000'000}, negative{-1, 0}, zero{0, 0};
    CHECK(call(nr_ppoll, {0, 0, address(&invalid), 0, 8}) == -einval);
    CHECK(call(nr_ppoll, {0, 0, address(&negative), 0, 8}) == -einval);
    const std::uint64_t mask = 0;
    CHECK(call(nr_ppoll, {0, 0, address(&zero), address(&mask), 4}) == -einval);
    CHECK(call(nr_ppoll, {0, 1, address(&zero), 0, 8}) == -efault);
    // The same number twice is reported twice.
    CHECK(write_byte(pipe.write_end) == 1);
    CHECK(polled({entry(pipe.read_end, pollin), entry(pipe.read_end, pollin)}) == "2: 1 1");
}

TEST(ppoll_waits_and_writes_the_time_left_back) {
    Table table;
    Timespec timeout{0, 30'000'000};
    auto start = std::chrono::steady_clock::now();
    CHECK(call(nr_ppoll, {0, 0, address(&timeout), 0, 8}) == 0);
    CHECK(since(start) >= 30ms);
    CHECK(timeout.seconds == 0 && timeout.nanoseconds == 0);
    const Files files;
    PollFd file = entry(files.file, pollin);
    timeout = {5, 0};
    CHECK(call(nr_ppoll, {address(&file), 1, address(&timeout), 0, 8}) == 1);
    CHECK(timeout.seconds == 4 && timeout.nanoseconds > 0);
    // A write from another thread ends the wait, and an epoll counts as readable while it has events.
    const auto pipe = make_pipe();
    const auto epoll = epoll_create();
    CHECK(control(epoll, epoll_ctl_add, pipe.read_end, epollin) == 0);
    start = std::chrono::steady_clock::now();
    {
        const Later writer([&] { write_byte(pipe.write_end); });
        CHECK(polled({entry(epoll, pollin | pollout)}, -1) == "1: 1");
    }
    CHECK(since(start) >= 40ms);
    CHECK(polled({entry(epoll, pollin | pollrdnorm | pollout), entry(pipe.read_end, pollin)}) == "2: 41 1");
}

/// An fd_set of Linux arm64: 1,024 bits in 64-bit words.
struct FdSet {
    std::uint64_t words[16] = {};
    void set(std::int64_t fd) { words[fd / 64] |= std::uint64_t{1} << (fd % 64); }
    bool has(std::int64_t fd) const { return (words[fd / 64] >> (fd % 64)) & 1; }
};

TEST(pselect6_reports_its_sets_as_linux_does) {
    Table table;
    const Files files;
    const auto pipe = make_pipe();
    CHECK(write_byte(pipe.write_end) == 1);
    FdSet reading, writing, exceptional;
    for (const auto fd : {pipe.read_end, files.file}) reading.set(fd);
    for (const auto fd : {pipe.read_end, pipe.write_end, files.file}) writing.set(fd);
    for (const auto fd : {pipe.read_end, files.file}) exceptional.set(fd);
    Timespec timeout{5, 0};
    const auto highest = std::max({pipe.read_end, pipe.write_end, files.file});
    CHECK(call(nr_pselect6, {unsigned_of(highest + 1), address(&reading), address(&writing), address(&exceptional),
                             address(&timeout), 0}) == 4);
    CHECK(reading.has(pipe.read_end) && reading.has(files.file));
    CHECK(!writing.has(pipe.read_end) && writing.has(pipe.write_end) && writing.has(files.file));
    CHECK(!exceptional.has(pipe.read_end) && !exceptional.has(files.file));
    CHECK(timeout.seconds == 4 && timeout.nanoseconds > 0);

    // A pipe whose writer is gone is readable, and one whose reader is gone readable and writable.
    const auto ended = make_pipe(), broken = make_pipe();
    CHECK(close_number(ended.write_end) == 0 && close_number(broken.read_end) == 0);
    reading = {};
    writing = {};
    for (const auto fd : {ended.read_end, broken.write_end}) {
        reading.set(fd);
        writing.set(fd);
    }
    const Timespec zero{0, 0};
    CHECK(call(nr_pselect6, {64, address(&reading), address(&writing), 0, address(&zero), 0}) == 3);
    CHECK(reading.has(ended.read_end) && !writing.has(ended.read_end));
    CHECK(reading.has(broken.write_end) && writing.has(broken.write_end));

    // A number that is not open answers EBADF, and the sets stay as they were.
    reading = {};
    reading.set(ended.write_end);
    CHECK(call(nr_pselect6, {64, address(&reading), 0, 0, address(&zero), 0}) == -ebadf);
    CHECK(reading.has(ended.write_end));
    CHECK(call(nr_pselect6, {unsigned_of(-1), 0, 0, 0, address(&zero), 0}) == -einval);
    const Timespec invalid{0, 1'000'000'000};
    CHECK(call(nr_pselect6, {0, 0, 0, 0, address(&invalid), 0}) == -einval);
    const std::uint64_t mask = 0;
    const struct {
        std::uint64_t set, size;
    } short_mask{address(&mask), 4};
    CHECK(call(nr_pselect6, {0, 0, 0, 0, address(&zero), address(&short_mask)}) == -einval);

    // Nothing ready: the time runs out, and the sets come back empty.
    const auto quiet = make_pipe();
    reading = {};
    reading.set(quiet.read_end);
    timeout = {0, 30'000'000};
    const auto start = std::chrono::steady_clock::now();
    CHECK(call(nr_pselect6, {64, address(&reading), 0, 0, address(&timeout), 0}) == 0);
    CHECK(since(start) >= 30ms);
    CHECK(!reading.has(quiet.read_end) && timeout.seconds == 0 && timeout.nanoseconds == 0);
}

TEST(pselect6_looks_no_further_than_linuxs_table) {
    // Linux's table holds 64 numbers at first, then 128 times a power of two; select leaves the bits
    // beyond it as they are (Android 15 shows it).
    Table table;
    const Files files;
    const Timespec zero{0, 0};
    FdSet reading;
    reading.set(files.file);
    reading.set(100);
    CHECK(call(nr_pselect6, {128, address(&reading), 0, 0, address(&zero), 0}) == 1);
    CHECK(reading.has(files.file) && reading.has(100));
    // Below 64 every number counts, whatever the guest has open.
    CHECK(call(nr_dup3, {unsigned_of(files.file), 20, 0}) == 20);
    reading = {};
    reading.set(20);
    reading.set(30);
    CHECK(call(nr_pselect6, {64, address(&reading), 0, 0, address(&zero), 0}) == -ebadf);
    // Number 70 makes the table 128 big, and 100 counts then.
    CHECK(call(nr_dup3, {unsigned_of(files.file), 70, 0}) == 70);
    reading = {};
    reading.set(100);
    CHECK(call(nr_pselect6, {128, address(&reading), 0, 0, address(&zero), 0}) == -ebadf);
}

TEST(ppoll_reports_an_epoll_readable_while_events_wait_for_room) {
    // Two numbers of one file in an epoll get their events from one filter: with room for one event,
    // the other waits in the epoll, which counts as readable meanwhile.
    Table table;
    const auto epoll = epoll_create();
    const auto pipe = make_pipe();
    const auto copy = call(nr_dup, {unsigned_of(pipe.read_end)});
    CHECK(control(epoll, epoll_ctl_add, pipe.read_end, epollin | epollet, 1) == 0);
    CHECK(control(epoll, epoll_ctl_add, copy, epollin | epollet, 2) == 0);
    CHECK(write_byte(pipe.write_end) == 1);
    CHECK(!ready(epoll, 0, 1).empty());
    CHECK(polled({entry(epoll, pollin)}) == "1: 1");
    CHECK(!ready(epoll, 0, 1).empty());
    CHECK(polled({entry(epoll, pollin)}) == "0: 0");
}

TEST(the_masks_of_ppoll_and_pselect6_hold_during_the_wait_only) {
    Table table;
    auto &state = apkrt::signals::current();
    const std::uint64_t open = 0, blocking = sigusr1_bit;
    const struct {
        std::uint64_t set, size;
    } open_pack{address(&open), 8}, blocking_pack{address(&blocking), 8};
    const Timespec long_wait{5, 0}, short_wait{0, 20'000'000};
    for (const bool select : {false, true}) {
        state.mask.store(sigusr1_bit);
        state.pending.fetch_or(sigusr1_bit);
        Timespec timeout = long_wait;
        const auto interrupted = select ? call(nr_pselect6, {0, 0, 0, 0, address(&timeout), address(&open_pack)})
                                        : call(nr_ppoll, {0, 0, address(&timeout), address(&open), 8});
        CHECK(interrupted == -eintr);
        CHECK(state.mask.load() == 0 && state.frame_mask_set && state.frame_mask == sigusr1_bit);
        CHECK(timeout.seconds == 4);  // the time left, written back after a signal as well
        state.frame_mask_set = false;
        state.mask.store(0);
        timeout = short_wait;
        const auto slept = select ? call(nr_pselect6, {0, 0, 0, 0, address(&timeout), address(&blocking_pack)})
                                  : call(nr_ppoll, {0, 0, address(&timeout), address(&blocking), 8});
        CHECK(slept == 0);
        CHECK(state.mask.load() == 0 && !state.frame_mask_set);
        state.pending.fetch_and(~sigusr1_bit);
    }
}

TEST(ppoll_and_pselect6_look_for_signals_before_their_time_and_epoll_pwait_after_it) {
    // Linux's do_poll and do_select: with nothing ready, a signal that the call's mask lets in ends even a
    // wait of no time with EINTR. ep_poll looks at the time first: epoll_pwait answers 0, and the
    // signal stays pending under the thread's own mask.
    Table table;
    auto &state = apkrt::signals::current();
    const std::uint64_t open = 0;
    const struct {
        std::uint64_t set, size;
    } open_pack{address(&open), 8};
    const auto epoll = epoll_create();
    EpollEvent events[1];
    for (int kind = 0; kind < 3; ++kind) {
        state.mask.store(sigusr1_bit);
        state.pending.fetch_or(sigusr1_bit);
        Timespec zero{0, 0};
        const auto result =
            kind == 0 ? call(nr_ppoll, {0, 0, address(&zero), address(&open), 8})
            : kind == 1 ? call(nr_pselect6, {0, 0, 0, 0, address(&zero), address(&open_pack)})
                        : call(nr_epoll_pwait, {unsigned_of(epoll), address(events), 1, 0, address(&open), 8});
        if (kind < 2) {
            CHECK(result == -eintr);
            CHECK(state.mask.load() == 0 && state.frame_mask_set && state.frame_mask == sigusr1_bit);
        } else {
            CHECK(result == 0);
            CHECK(state.mask.load() == sigusr1_bit && !state.frame_mask_set);
        }
        state.frame_mask_set = false;
        state.mask.store(0);
        state.pending.fetch_and(~sigusr1_bit);
    }
}

}  // namespace

int main() {
    return apk::testing::run_all();
}
