// Checks the vsync channel (ADR-0036, decisions 1 and 2): a channel's descriptor is readable only while it
// holds a tick it asked for or a period it has not read; a tick goes only to the channels that asked since
// their last, each of which keeps only the newest; the read gives the tick with its three timelines and ends
// the readiness; the period is the session's refresh rate until a tick's differs by more than 1 %, which
// wakes every channel; the syscalls refuse descriptors of another kind; a guest waiting in epoll wakes at a
// tick; closing the descriptor ends the channel, also while another thread posts ticks. The runtime counts
// ticks and keeps the period for the process, so the tests build on each other in their order: first without
// a session, then with one. The test's thread plays the guest and calls the dispatcher directly, as
// session_test.cpp does.
#include "channels.h"
#include "descriptors.h"
#include "syscall_frame.h"

#include "APKRuntime/apkrt.h"
#include "check.h"

#include <sys/ioctl.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <thread>

namespace {

using namespace std::chrono_literals;

// Linux: include/uapi/asm-generic/unistd.h, fcntl.h, poll.h, eventpoll.h, errno.h.
constexpr std::uint64_t nr_epoll_create1 = 20, nr_epoll_ctl = 21, nr_epoll_pwait = 22, nr_fcntl = 25, nr_close = 57,
                        nr_pipe2 = 59, nr_read = 63, nr_write = 64, nr_ppoll = 73;
constexpr std::uint64_t f_getfd = 1, fd_cloexec = 1, epoll_ctl_add = 1;
constexpr std::int16_t pollin = 0x1, pollout = 0x4;
constexpr std::uint32_t epollin = 0x1;
constexpr std::int64_t ebadf = 9, efault = 14, einval = 22, emfile = 24;

constexpr std::int64_t sixty = 16'666'667, hundred_twenty = 8'333'333;  // periods in ns

std::int64_t call(std::uint64_t number, std::initializer_list<std::uint64_t> arguments = {}) {
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

std::int64_t connect() {
    return call(APKRT_NR_VSYNC_CONNECT);
}

std::int64_t request(std::int64_t channel) {
    return call(APKRT_NR_VSYNC_REQUEST, {unsigned_of(channel)});
}

/// vsync_read's result, and what it filled in.
struct Read {
    std::int64_t result;
    apkrt_vsync vsync;
};

Read read(std::int64_t channel) {
    Read read{};
    std::memset(&read.vsync, 0xa5, sizeof read.vsync);  // so that a field left out shows
    read.result = call(APKRT_NR_VSYNC_READ, {unsigned_of(channel), address(&read.vsync)});
    return read;
}

/// What ppoll reports for `fd` at once, as its revents.
std::int16_t revents(std::int64_t fd) {
    struct {
        std::int32_t fd;
        std::int16_t events, revents;
    } entry{static_cast<std::int32_t>(fd), pollin | pollout, 0};
    struct {
        std::int64_t seconds, nanoseconds;
    } none{0, 0};
    CHECK(call(nr_ppoll, {address(&entry), 1, address(&none), 0, 8}) >= 0);
    return entry.revents;
}

bool readable(std::int64_t fd) {
    return (revents(fd) & pollin) != 0;
}

/// The ticks posted so far: the number of the next one is one more.
std::int64_t posted = 0;
std::int64_t now = 1'000'000'000'000;  // a guest time in ns, which each tick moves on

/// Posts a tick with the period `period` and returns its number.
std::int64_t tick(std::int64_t period = sixty) {
    now += period;
    apkrt_vsync_post(now, now + period);
    return ++posted;
}

/// Whether `read` holds the tick with the number `number`, which began at `time`, with the period `period`
/// and its three timelines.
bool holds_tick(const Read &read, std::int64_t number, std::int64_t time, std::int64_t period) {
    const apkrt_vsync &vsync = read.vsync;
    bool timelines = true;
    for (std::int64_t k = 0; k < APKRT_FRAME_TIMELINES; ++k) {
        const apkrt_frame_timeline &timeline = vsync.timelines[k];
        timelines = timelines && timeline.vsync_id == number + k &&
                    timeline.expected_present_time == vsync.target_time + (k + 1) * period &&
                    timeline.deadline == vsync.target_time + k * period;
    }
    return read.result == 1 && vsync.time == time && vsync.target_time == time + period && vsync.period == period &&
           vsync.preferred_timeline == 0 && vsync.reserved == 0 && timelines;
}

// --- Without a session --------------------------------------------------------------------------

TEST(a_new_channel_is_a_descriptor_that_closes_on_exec_and_is_not_readable) {
    const auto before = apkrt::channels::vsync_count();
    const auto channel = connect();
    CHECK(channel >= 0);
    CHECK(call(nr_fcntl, {unsigned_of(channel), f_getfd}) == static_cast<std::int64_t>(fd_cloexec));
    CHECK(apkrt::channels::vsync_count() == before + 1);
    CHECK(revents(channel) == 0);
    CHECK(call(nr_close, {unsigned_of(channel)}) == 0);
    CHECK(apkrt::channels::vsync_count() == before);
}

TEST(before_any_tick_and_session_a_read_gives_nothing) {
    const auto channel = connect();
    const Read nothing = read(channel);
    CHECK(nothing.result == 0);
    CHECK(nothing.vsync.time == 0 && nothing.vsync.target_time == 0 && nothing.vsync.period == 0);
    CHECK(nothing.vsync.preferred_timeline == 0 && nothing.vsync.reserved == 0);
    for (const auto &timeline : nothing.vsync.timelines) {
        CHECK(timeline.vsync_id == 0 && timeline.expected_present_time == 0 && timeline.deadline == 0);
    }
    call(nr_close, {unsigned_of(channel)});
}

TEST(the_syscalls_refuse_descriptors_of_another_kind_and_null) {
    std::int32_t ends[2] = {-1, -1};
    CHECK(call(nr_pipe2, {address(ends), 0}) == 0);
    for (const std::int64_t other : {std::int64_t{ends[0]}, std::int64_t{-1}, std::int64_t{4096}}) {
        CHECK(request(other) == -ebadf);
        CHECK(read(other).result == -ebadf);
    }
    const auto channel = connect();
    CHECK(call(APKRT_NR_VSYNC_READ, {unsigned_of(channel), 0}) == -efault);
    // Only the private syscalls read a channel.
    char byte = 0;
    CHECK(call(nr_read, {unsigned_of(channel), address(&byte), 1}) == -einval);
    CHECK(call(nr_write, {unsigned_of(channel), address(&byte), 1}) == -einval);
    call(nr_close, {unsigned_of(channel)});
    CHECK(request(channel) == -ebadf);
    call(nr_close, {unsigned_of(ends[0])});
    call(nr_close, {unsigned_of(ends[1])});
}

TEST(a_full_table_refuses_a_new_channel) {
    const auto limits = apkrt::descriptors::limits();
    const auto before = apkrt::channels::vsync_count();
    // The lowest free number is at most 3 here: a soft limit of 0 leaves no room.
    CHECK(apkrt::descriptors::set_limits({0, limits.hard}) == 0);
    CHECK(connect() == -emfile);
    CHECK(apkrt::descriptors::set_limits(limits) == 0);
    CHECK(apkrt::channels::vsync_count() == before);
}

// --- With a session at 120 Hz --------------------------------------------------------------------

void on_end(void *, int) {}

TEST(before_the_first_tick_the_period_is_the_sessions_refresh_rate) {
    apkrt_session session{};
    session.end = on_end;
    session.configuration.scale = 3;
    session.configuration.refresh_rate = 120;
    CHECK(apkrt_session_begin(&session) == 0);
    const auto channel = connect();
    const Read before = read(channel);
    CHECK(before.result == 0 && before.vsync.time == 0 && before.vsync.period == hundred_twenty);
    CHECK(!readable(channel));
    call(nr_close, {unsigned_of(channel)});
}

TEST(the_first_tick_at_another_period_makes_it_the_period_and_wakes_every_channel) {
    const auto asked = connect(), idle = connect();
    CHECK(request(asked) == 0);
    const auto number = tick(sixty);
    CHECK(readable(asked) && readable(idle));
    CHECK(holds_tick(read(asked), number, now, sixty));
    const Read woken = read(idle);
    CHECK(woken.result == 0 && woken.vsync.period == sixty);
    CHECK(!readable(asked) && !readable(idle));
    call(nr_close, {unsigned_of(asked)});
    call(nr_close, {unsigned_of(idle)});
}

TEST(a_tick_goes_only_to_a_channel_that_asked_and_a_read_ends_its_readiness) {
    const auto asked = connect(), idle = connect();
    CHECK(request(asked) == 0);
    const auto number = tick();
    const auto time = now;
    CHECK(readable(asked));
    CHECK(!readable(idle));
    CHECK(holds_tick(read(asked), number, time, sixty));
    CHECK(!readable(asked));
    // A tick nobody asked for reaches nobody.
    tick();
    CHECK(!readable(asked) && !readable(idle));
    const Read none = read(asked);
    CHECK(none.result == 0 && none.vsync.period == sixty);
    // The times are the last tick's the channel got.
    CHECK(none.vsync.time == time && none.vsync.timelines[0].vsync_id == number);
    CHECK(read(idle).result == 0);
    call(nr_close, {unsigned_of(asked)});
    call(nr_close, {unsigned_of(idle)});
}

TEST(a_channel_keeps_only_the_newest_tick) {
    const auto channel = connect();
    CHECK(request(channel) == 0);
    tick();
    CHECK(request(channel) == 0);
    const auto newest = tick();
    CHECK(holds_tick(read(channel), newest, now, sixty));
    CHECK(read(channel).result == 0);
    call(nr_close, {unsigned_of(channel)});
}

TEST(asking_twice_before_a_tick_gets_one_tick) {
    const auto channel = connect();
    CHECK(request(channel) == 0);
    CHECK(request(channel) == 0);
    const auto first = tick();
    const auto time = now;
    tick();
    CHECK(holds_tick(read(channel), first, time, sixty));
    CHECK(!readable(channel));
    call(nr_close, {unsigned_of(channel)});
}

TEST(a_period_change_of_one_percent_or_less_changes_nothing) {
    const auto channel = connect();
    tick(sixty + sixty / 100);  // 1 %
    tick(sixty - sixty / 100);
    CHECK(!readable(channel));
    CHECK(read(channel).vsync.period == sixty);
    // The timelines keep the period too.
    CHECK(request(channel) == 0);
    const auto number = tick(sixty + sixty / 200);
    const Read got = read(channel);
    CHECK(got.result == 1 && got.vsync.timelines[0].vsync_id == number && got.vsync.period == sixty);
    CHECK(got.vsync.target_time == now + sixty + sixty / 200);
    CHECK(got.vsync.timelines[0].expected_present_time == got.vsync.target_time + sixty);
    call(nr_close, {unsigned_of(channel)});
}

TEST(a_change_counts_from_just_over_one_percent) {
    const auto channel = connect();
    constexpr std::int64_t hundred = 10'000'000;  // 100 Hz, whose 1 % is a whole number
    tick(hundred);
    CHECK(read(channel).vsync.period == hundred);
    tick(hundred + hundred / 100);
    tick(hundred - hundred / 100);
    CHECK(!readable(channel) && read(channel).vsync.period == hundred);
    tick(hundred + hundred / 100 + 1);
    CHECK(readable(channel) && read(channel).vsync.period == hundred + hundred / 100 + 1);
    tick(sixty);
    call(nr_close, {unsigned_of(channel)});
}

TEST(a_period_change_of_more_than_one_percent_wakes_every_channel_once) {
    const auto asked = connect(), idle = connect();
    CHECK(request(asked) == 0);
    const auto number = tick(hundred_twenty);
    CHECK(holds_tick(read(asked), number, now, hundred_twenty));
    const Read woken = read(idle);
    CHECK(woken.result == 0 && woken.vsync.period == hundred_twenty);
    CHECK(!readable(idle));
    // Two changes before a read: the read gives the last period.
    tick(sixty);
    tick(hundred_twenty);
    CHECK(readable(idle));
    CHECK(read(idle).vsync.period == hundred_twenty);
    // Once read, a tick at the same period wakes nobody.
    tick(hundred_twenty);
    CHECK(!readable(idle));
    call(nr_close, {unsigned_of(asked)});
    call(nr_close, {unsigned_of(idle)});
}

TEST(a_tick_without_a_positive_period_changes_no_period) {
    const auto channel = connect();
    const auto before = read(channel).vsync.period;
    CHECK(request(channel) == 0);
    now += before;
    apkrt_vsync_post(now, now);  // a target that is not after the time
    const auto number = ++posted;
    const Read got = read(channel);
    CHECK(got.result == 1 && got.vsync.time == now && got.vsync.period == before);
    CHECK(got.vsync.timelines[0].vsync_id == number);
    apkrt_vsync_post(now + 10, now);
    ++posted;
    CHECK(!readable(channel) && read(channel).vsync.period == before);
    call(nr_close, {unsigned_of(channel)});
}

TEST(a_guest_waiting_in_epoll_wakes_at_the_tick_it_asked_for) {
    const auto channel = connect();
    const auto epoll = call(nr_epoll_create1, {0});
    struct {
        std::uint32_t events;
        std::uint32_t pad;
        std::uint64_t data;
    } interest{epollin, 0, 7}, fired{};
    CHECK(call(nr_epoll_ctl, {unsigned_of(epoll), epoll_ctl_add, unsigned_of(channel), address(&interest)}) == 0);
    CHECK(request(channel) == 0);
    std::thread host([] {
        std::this_thread::sleep_for(50ms);
        tick(hundred_twenty);
    });
    const auto started = std::chrono::steady_clock::now();
    const auto count = call(nr_epoll_pwait, {unsigned_of(epoll), address(&fired), 1, 2000, 0, 8});
    host.join();
    CHECK(count == 1 && fired.data == 7 && (fired.events & epollin) != 0);
    CHECK(std::chrono::steady_clock::now() - started < 1500ms);
    CHECK(read(channel).result == 1);
    // Level-triggered: nothing more once it is read.
    CHECK(call(nr_epoll_pwait, {unsigned_of(epoll), address(&fired), 1, 0, 0, 8}) == 0);
    call(nr_close, {unsigned_of(epoll)});
    call(nr_close, {unsigned_of(channel)});
}

TEST(a_guest_waiting_in_epoll_wakes_at_a_period_change) {
    const auto channel = connect();
    const auto epoll = call(nr_epoll_create1, {0});
    struct {
        std::uint32_t events;
        std::uint32_t pad;
        std::uint64_t data;
    } interest{epollin, 0, 9}, fired{};
    CHECK(call(nr_epoll_ctl, {unsigned_of(epoll), epoll_ctl_add, unsigned_of(channel), address(&interest)}) == 0);
    std::thread host([] {
        std::this_thread::sleep_for(50ms);
        tick(sixty);
    });
    const auto count = call(nr_epoll_pwait, {unsigned_of(epoll), address(&fired), 1, 2000, 0, 8});
    host.join();
    CHECK(count == 1 && fired.data == 9);
    const Read woken = read(channel);
    CHECK(woken.result == 0 && woken.vsync.period == sixty);
    call(nr_close, {unsigned_of(epoll)});
    call(nr_close, {unsigned_of(channel)});
}

TEST(the_pipe_holds_bytes_exactly_while_something_is_unread) {
    const auto channel = connect();
    const auto bytes = [channel] {
        int held = -1;
        CHECK(::ioctl(apkrt::descriptors::get(static_cast<int>(channel))->host(), FIONREAD, &held) == 0);
        return held;
    };
    CHECK(bytes() == 0);
    CHECK(request(channel) == 0);
    tick();
    CHECK(bytes() == 1);
    // A tick and then a period change before the read: one byte each, so that EPOLLET sees both.
    tick(hundred_twenty);
    CHECK(bytes() == 2);
    CHECK(read(channel).result == 1);
    CHECK(bytes() == 0);
    call(nr_close, {unsigned_of(channel)});
}

TEST(closing_a_channel_ends_it_and_the_others_go_on) {
    const auto before = apkrt::channels::vsync_count();
    const auto closed = connect(), open = connect();
    CHECK(request(closed) == 0 && request(open) == 0);
    CHECK(call(nr_close, {unsigned_of(closed)}) == 0);
    CHECK(apkrt::channels::vsync_count() == before + 1);
    const auto number = tick(hundred_twenty);
    CHECK(holds_tick(read(open), number, now, hundred_twenty));
    call(nr_close, {unsigned_of(open)});
    CHECK(apkrt::channels::vsync_count() == before);
}

// Last, since its ticks leave the count of the ticks unknown.
TEST(ticks_go_on_while_channels_come_and_go) {
    const auto before = apkrt::channels::vsync_count();
    std::atomic<bool> stop{false};
    std::thread host([&stop] {
        for (std::int64_t time = 2'000'000'000'000; !stop.load(); time += sixty) apkrt_vsync_post(time, time + sixty);
    });
    int failed = 0;
    for (int round = 0; round < 2000; ++round) {
        const auto channel = connect();
        if (channel < 0 || request(channel) != 0 || read(channel).result < 0) ++failed;
        call(nr_close, {unsigned_of(channel)});
    }
    stop.store(true);
    host.join();
    CHECK(failed == 0);
    CHECK(apkrt::channels::vsync_count() == before);
}

}  // namespace

int main() {
    apkrt::descriptors::reset();
    apkrt::descriptors::adopt_standard_descriptors();
    return apk::testing::run_all();
}
