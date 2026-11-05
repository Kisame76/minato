// Checks the vsync channel (ADR-0036, decisions 1 and 2): a channel's descriptor is readable only while it
// holds a tick it asked for or a period it has not read; a tick goes only to the channels that asked since
// their last, each of which keeps only the newest; the read gives the tick with its three timelines and ends
// the readiness, timeline 0 two periods after the frame time at every rate (ADR-0041 decision 11);
// the period is the session's refresh rate until a tick's differs by more than 1 %, which wakes every channel;
// apkrt_vsync_grid gives the last tick's time with the period it left, none before the first tick and never
// two ticks' values together; the syscalls refuse descriptors of another kind; a guest waiting in epoll wakes
// at a tick; closing the descriptor ends the channel, also while another thread posts ticks. The runtime counts
// ticks and keeps the period for the process, so the tests build on each other in their order: first without
// a session, then with one. The test's thread plays the guest and calls the dispatcher directly, as
// session_test.cpp does.
//
// Then the input channel (decisions 1 and 3): IDs from 1, never reused; the events posted before the guest
// connects wait, with a byte in the pipe for them; a connect is made once, and again after a full table refused
// it; the reads give the events oldest first, each record whole, and the last ends the readiness and empties
// the pipe; a guest waiting in epoll wakes at a post; unsound events are refused; of more than 16 samples the
// oldest go; a full channel of 256 events merges the oldest pair of MOVEs with the same pointers and down
// time, and keeps gestures whole: it takes the ends of the open gesture, refuses a DOWN with the rest of its
// gesture, and ends a gesture at a POINTER_DOWN with a CANCEL; closing the descriptor ends the channel; and
// events posted from another thread arrive in order, also while channels come and go.
#include "channels.h"
#include "descriptors.h"
#include "syscall_frame.h"

#include "APKRuntime/apkrt.h"
#include "check.h"

#include <sys/ioctl.h>

#include <atomic>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <initializer_list>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

// Linux: include/uapi/asm-generic/unistd.h, fcntl.h, poll.h, eventpoll.h, errno.h.
constexpr std::uint64_t nr_epoll_create1 = 20, nr_epoll_ctl = 21, nr_epoll_pwait = 22, nr_fcntl = 25, nr_close = 57,
                        nr_pipe2 = 59, nr_read = 63, nr_write = 64, nr_ppoll = 73;
constexpr std::uint64_t f_getfd = 1, fd_cloexec = 1, epoll_ctl_add = 1;
constexpr std::int16_t pollin = 0x1, pollout = 0x4;
constexpr std::uint32_t epollin = 0x1;
constexpr std::int64_t enoent = 2, ebadf = 9, eagain = 11, efault = 14, ebusy = 16, einval = 22, emfile = 24;

constexpr std::int64_t thirty = 33'333'333, sixty = 16'666'667, ninety = 11'111'111,
                       hundred_twenty = 8'333'333;  // periods in ns

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
/// and its three timelines a period apart from target_time + period, each deadline a period before its present.
bool holds_tick(const Read &read, std::int64_t number, std::int64_t time, std::int64_t period) {
    const apkrt_vsync &vsync = read.vsync;
    const std::int64_t first = vsync.target_time + period;
    bool timelines = true;
    for (std::int64_t k = 0; k < APKRT_FRAME_TIMELINES; ++k) {
        const apkrt_frame_timeline &timeline = vsync.timelines[k];
        timelines = timelines && timeline.vsync_id == number + k &&
                    timeline.expected_present_time == first + k * period &&
                    timeline.deadline == first + (k - 1) * period;
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

TEST(before_the_first_tick_there_is_no_grid) {
    std::int64_t time = 5, period = 7;
    CHECK(apkrt_vsync_grid(&time, &period) == -1);
    CHECK(time == 5 && period == 7);
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
    // The session's rate makes no grid: that needs a tick's time.
    std::int64_t time = 5, period = 7;
    CHECK(apkrt_vsync_grid(&time, &period) == -1 && time == 5 && period == 7);
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

TEST(the_grid_is_the_last_ticks_time_with_the_period_it_left) {
    std::int64_t time = 0, period = 0;
    tick(sixty);
    CHECK(apkrt_vsync_grid(&time, &period) == 0 && time == now && period == sixty);
    tick(hundred_twenty);
    CHECK(apkrt_vsync_grid(&time, &period) == 0 && time == now && period == hundred_twenty);
    // A tick whose period differs by 1 % or less leaves the period, and gives its time.
    tick(hundred_twenty + hundred_twenty / 100);
    CHECK(apkrt_vsync_grid(&time, &period) == 0 && time == now && period == hundred_twenty);
    // A NULL pointer is skipped.
    time = period = 0;
    CHECK(apkrt_vsync_grid(nullptr, &period) == 0 && period == hundred_twenty);
    CHECK(apkrt_vsync_grid(&time, nullptr) == 0 && time == now);
}

TEST(timeline_zero_presents_two_periods_after_the_frame_time_at_every_rate) {
    // ADR-0041 decision 11, changed 2026-10-02 evening: graphics/host sends a present that near without a time, and
    // such a present reaches the screen 2 refreshes after its drawable, so no least delay moves timeline 0 out.
    // Its deadline is target_time, never the frame time (ADR-0036). The other timelines follow a period apart,
    // each deadline a period before its present.
    const auto channel = connect();
    for (const auto period : {thirty, sixty, ninety, hundred_twenty}) {
        CHECK(request(channel) == 0);
        const auto number = tick(period);
        const Read got = read(channel);
        CHECK(got.result == 1 && got.vsync.time == now && got.vsync.period == period);
        for (std::int64_t k = 0; k < APKRT_FRAME_TIMELINES; ++k) {
            const apkrt_frame_timeline &timeline = got.vsync.timelines[k];
            CHECK(timeline.vsync_id == number + k);
            CHECK(timeline.expected_present_time == now + (2 + k) * period);
            CHECK(timeline.deadline == now + (1 + k) * period);
        }
    }
    call(nr_close, {unsigned_of(channel)});
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

// The last two vsync tests, since their ticks leave the count of the ticks unknown.
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

TEST(a_grid_never_mixes_two_ticks_while_the_host_posts) {
    // The period changes at each tick, from 60 to 120 Hz and back; the times at 60 Hz are even and those at
    // 120 Hz odd, so a time with another tick's period shows.
    constexpr std::int64_t start = 4'000'000'000'000;
    apkrt_vsync_post(start, start + sixty);
    std::atomic<bool> stop{false};
    std::thread host(
        [&stop](std::int64_t time) {
            for (; !stop.load(); time += sixty) apkrt_vsync_post(time, time + (time % 2 == 0 ? sixty : hundred_twenty));
        },
        start + sixty);
    int missing = 0, mixed = 0;
    for (int round = 0; round < 100'000; ++round) {
        std::int64_t time = 0, period = 0;
        if (apkrt_vsync_grid(&time, &period) != 0) {
            ++missing;
        } else if (period != (time % 2 == 0 ? sixty : hundred_twenty)) {
            ++mixed;
        }
    }
    stop.store(true);
    host.join();
    CHECK(missing == 0 && mixed == 0);
}

// --- The input channel ----------------------------------------------------------------------------

constexpr std::int32_t down = 0, up = 1, move = 2, cancel = 3, pointer_down = 5, pointer_up = 6, index_shift = 8;
constexpr std::size_t most_events = 256, most_ends = APKRT_MOTION_POINTERS + 1;

std::int64_t input_connect(std::uint64_t id) {
    return call(APKRT_NR_INPUT_CONNECT, {id});
}

/// A new input channel and its descriptor.
struct Input {
    std::uint64_t id;
    std::int64_t fd;
};

Input open_input() {
    const auto id = apkrt_input_add();
    return {id, input_connect(id)};
}

/// The bytes in the pipe behind the channel `fd`, -1 for another descriptor.
int pipe_bytes(std::int64_t fd) {
    const auto description = apkrt::descriptors::get(static_cast<int>(fd));
    int held = -1;
    if (description != nullptr) CHECK(::ioctl(description->host(), FIONREAD, &held) == 0);
    return held;
}

/// input_read's result, and what it filled in.
struct Taken {
    std::int64_t result;
    apkrt_motion_event event;
};

Taken take(std::int64_t channel) {
    Taken taken{};
    std::memset(&taken.event, 0xa5, sizeof taken.event);  // so that a field left out shows
    taken.result = call(APKRT_NR_INPUT_READ, {unsigned_of(channel), address(&taken.event)});
    return taken;
}

/// A motion event to post. Its places tell samples and pointers apart: x is the sample's time, y the
/// pointer's ID and the touch major the pointer's index.
struct Motion {
    std::int32_t action = move;
    std::int64_t down_time = 1;
    std::vector<std::int32_t> ids{0};
    std::vector<std::int64_t> times{2};

    std::vector<apkrt_pointer_coords> places() const {
        std::vector<apkrt_pointer_coords> all;
        for (const auto time : times) {
            for (std::size_t index = 0; index < ids.size(); ++index) {
                all.push_back(
                    {static_cast<float>(time), static_cast<float>(ids[index]), 0.5f, static_cast<float>(index)});
            }
        }
        return all;
    }
};

int post(std::uint64_t id, const Motion &motion) {
    const auto places = motion.places();
    return apkrt_input_post(id, motion.action, motion.down_time, static_cast<std::uint32_t>(motion.ids.size()),
                            motion.ids.data(), static_cast<std::uint32_t>(motion.times.size()), motion.times.data(),
                            places.data());
}

/// Whether `taken` holds `motion` whole, of its samples the newest 16, and zeros after them.
bool holds(const Taken &taken, const Motion &motion) {
    const apkrt_motion_event &event = taken.event;
    const std::size_t pointers = motion.ids.size();
    const std::size_t first =
        motion.times.size() > APKRT_MOTION_SAMPLES ? motion.times.size() - APKRT_MOTION_SAMPLES : 0;
    const std::size_t samples = motion.times.size() - first;
    if (taken.result != 0 || event.action != motion.action || event.pointer_count != pointers ||
        event.sample_count != samples || event.kind != APKRT_INPUT_MOTION || event.down_time != motion.down_time) {
        return false;
    }
    const auto places = motion.places();
    for (std::size_t index = 0; index < APKRT_MOTION_POINTERS; ++index) {
        if (event.pointer_ids[index] != (index < pointers ? motion.ids[index] : 0)) return false;
    }
    for (std::size_t sample = 0; sample < APKRT_MOTION_SAMPLES; ++sample) {
        if (event.sample_times[sample] != (sample < samples ? motion.times[first + sample] : 0)) return false;
        for (std::size_t index = 0; index < APKRT_MOTION_POINTERS; ++index) {
            const apkrt_pointer_coords expected = sample < samples && index < pointers
                                                      ? places[(first + sample) * pointers + index]
                                                      : apkrt_pointer_coords{};
            if (std::memcmp(&event.coords[sample][index], &expected, sizeof expected) != 0) return false;
        }
    }
    return true;
}

/// The first input test: the IDs start here.
TEST(input_ids_count_from_one_and_are_never_reused) {
    const auto first = apkrt_input_add();
    CHECK(first == 1);
    const auto fd = input_connect(first);
    CHECK(fd >= 0);
    CHECK(call(nr_close, {unsigned_of(fd)}) == 0);
    CHECK(apkrt_input_add() == first + 1);
}

TEST(events_posted_before_the_connect_wait_and_make_the_new_descriptor_readable) {
    const auto id = apkrt_input_add();
    const Motion touch{.action = down, .down_time = 5, .ids = {0}, .times = {5}};
    CHECK(post(id, touch) == 1);
    const auto fd = input_connect(id);
    CHECK(fd >= 0);
    CHECK(call(nr_fcntl, {unsigned_of(fd), f_getfd}) == static_cast<std::int64_t>(fd_cloexec));
    // A byte for what waited, so that EPOLLET sees it too.
    CHECK(readable(fd) && pipe_bytes(fd) == 1);
    CHECK(holds(take(fd), touch));
    CHECK(!readable(fd));
    CHECK(take(fd).result == -eagain);
    call(nr_close, {unsigned_of(fd)});
}

TEST(a_connect_needs_an_id_the_host_gave_and_is_made_once) {
    CHECK(input_connect(0) == -enoent);
    CHECK(input_connect(1'000'000) == -enoent);
    const auto input = open_input();
    CHECK(input.fd >= 0);
    CHECK(input_connect(input.id) == -ebusy);
    call(nr_close, {unsigned_of(input.fd)});
    CHECK(input_connect(input.id) == -ebusy);
}

TEST(a_connect_that_a_full_table_refused_can_be_made_again) {
    const auto id = apkrt_input_add();
    const Motion touch{.action = down, .down_time = 7, .ids = {0}, .times = {7}};
    CHECK(post(id, touch) == 1);
    const auto limits = apkrt::descriptors::limits();
    CHECK(apkrt::descriptors::set_limits({0, limits.hard}) == 0);
    CHECK(input_connect(id) == -emfile);
    CHECK(apkrt::descriptors::set_limits(limits) == 0);
    const auto fd = input_connect(id);
    CHECK(fd >= 0 && readable(fd));
    CHECK(holds(take(fd), touch));
    call(nr_close, {unsigned_of(fd)});
}

TEST(reads_give_the_oldest_event_first_and_the_last_ends_the_readiness) {
    const auto input = open_input();
    const std::vector<Motion> gesture = {
        {.action = down, .down_time = 10, .ids = {0}, .times = {10}},
        {.action = pointer_down | (1 << index_shift), .down_time = 10, .ids = {0, 1}, .times = {11}},
        {.action = move, .down_time = 10, .ids = {0, 1}, .times = {12, 13, 14}},
        {.action = pointer_up | (0 << index_shift), .down_time = 10, .ids = {0, 1}, .times = {15}},
        {.action = cancel, .down_time = 10, .ids = {1}, .times = {16}},
    };
    for (const auto &motion : gesture) CHECK(post(input.id, motion) == 1);
    for (const auto &motion : gesture) {
        CHECK(readable(input.fd));
        CHECK(holds(take(input.fd), motion));
    }
    CHECK(!readable(input.fd));
    CHECK(take(input.fd).result == -eagain);
    call(nr_close, {unsigned_of(input.fd)});
}

TEST(the_input_syscalls_refuse_descriptors_of_another_kind_and_null) {
    std::int32_t ends[2] = {-1, -1};
    CHECK(call(nr_pipe2, {address(ends), 0}) == 0);
    const auto vsync = connect();
    for (const std::int64_t other : {std::int64_t{ends[0]}, vsync, std::int64_t{-1}, std::int64_t{4096}}) {
        CHECK(take(other).result == -ebadf);
    }
    const auto input = open_input();
    CHECK(call(APKRT_NR_INPUT_READ, {unsigned_of(input.fd), 0}) == -efault);
    CHECK(request(input.fd) == -ebadf);
    CHECK(read(input.fd).result == -ebadf);
    char byte = 0;
    CHECK(call(nr_read, {unsigned_of(input.fd), address(&byte), 1}) == -einval);
    CHECK(call(nr_write, {unsigned_of(input.fd), address(&byte), 1}) == -einval);
    call(nr_close, {unsigned_of(input.fd)});
    CHECK(take(input.fd).result == -ebadf);
    call(nr_close, {unsigned_of(vsync)});
    call(nr_close, {unsigned_of(ends[0])});
    call(nr_close, {unsigned_of(ends[1])});
}

TEST(posts_fail_for_an_unknown_id_and_after_the_channel_ended) {
    const Motion touch{.action = down, .down_time = 1, .ids = {0}, .times = {1}};
    CHECK(post(0, touch) == 0);
    CHECK(post(1'000'000, touch) == 0);
    const auto input = open_input();
    CHECK(post(input.id, touch) == 1);
    CHECK(call(nr_close, {unsigned_of(input.fd)}) == 0);
    CHECK(post(input.id, touch) == 0);
}

TEST(unsound_events_are_refused_and_leave_the_channel_empty) {
    const auto input = open_input();
    const std::vector<Motion> unsound = {
        {.ids = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16}},  // 17 pointers
        {.ids = {1, 0}},                                                     // IDs not rising
        {.ids = {0, 0}},
        {.ids = {32}},                                                       // IDs not 0 to 31
        {.ids = {-1}},
        {.action = 4},                                                       // actions M3 does not post
        {.action = 7},
        {.action = -1},
        {.action = 0x10000 | pointer_down | (1 << index_shift), .ids = {0, 1}},
        {.action = down | (1 << index_shift), .ids = {0}},                 // an index on an action without one
        {.action = move | (1 << index_shift), .ids = {0, 1}},
        {.action = pointer_down | (2 << index_shift), .ids = {0, 1}},      // an index past the pointers
        // As Android's reader makes them: DOWN and UP of one pointer, POINTER_DOWN and POINTER_UP with others,
        // and history only on a MOVE.
        {.action = down, .ids = {0, 1}},
        {.action = up, .ids = {0, 1}},
        {.action = pointer_down, .ids = {0}},
        {.action = pointer_up, .ids = {0}},
        {.action = down, .ids = {0}, .times = {1, 2}},
        {.action = up, .ids = {0}, .times = {1, 2}},
        {.action = cancel, .ids = {0}, .times = {1, 2}},
        {.action = pointer_down | (1 << index_shift), .ids = {0, 1}, .times = {1, 2}},
        {.action = pointer_up | (1 << index_shift), .ids = {0, 1}, .times = {1, 2}},
    };
    for (const auto &motion : unsound) CHECK(post(input.id, motion) == 0);
    // No pointer or sample, and no arrays.
    const std::int32_t ids[1] = {0};
    const std::int64_t times[1] = {2};
    const apkrt_pointer_coords places[1] = {};
    CHECK(apkrt_input_post(input.id, move, 1, 0, ids, 1, times, places) == 0);
    CHECK(apkrt_input_post(input.id, move, 1, 1, ids, 0, times, places) == 0);
    CHECK(apkrt_input_post(input.id, move, 1, 1, nullptr, 1, times, places) == 0);
    CHECK(apkrt_input_post(input.id, move, 1, 1, ids, 1, nullptr, places) == 0);
    CHECK(apkrt_input_post(input.id, move, 1, 1, ids, 1, times, nullptr) == 0);
    CHECK(!readable(input.fd) && take(input.fd).result == -eagain);
    // At the edges: 16 pointers with the IDs 16 to 31, an index at the last of them.
    Motion widest{.action = pointer_up | (15 << index_shift), .ids = {}};
    for (std::int32_t pointer = 16; pointer < 32; ++pointer) widest.ids.push_back(pointer);
    CHECK(post(input.id, widest) == 1);
    CHECK(holds(take(input.fd), widest));
    call(nr_close, {unsigned_of(input.fd)});
}

TEST(of_more_than_sixteen_samples_the_oldest_go) {
    const auto input = open_input();
    Motion many{.action = move, .down_time = 1, .ids = {0, 1}, .times = {}};
    for (std::int64_t time = 1; time <= 20; ++time) many.times.push_back(time);
    CHECK(post(input.id, many) == 1);
    CHECK(holds(take(input.fd), many));  // holds() expects the times 5 to 20
    call(nr_close, {unsigned_of(input.fd)});
}

TEST(a_full_channel_merges_the_oldest_pair_of_moves_with_the_same_pointers) {
    const auto input = open_input();
    std::vector<Motion> sent{{.action = down, .down_time = 1, .ids = {0}, .times = {1}}};
    for (std::int64_t time = 2; sent.size() < most_events; ++time) {
        sent.push_back({.action = move, .down_time = 1, .ids = {0}, .times = {time}});
    }
    for (const auto &motion : sent) CHECK(post(input.id, motion) == 1);
    const Motion next{.action = move, .down_time = 1, .ids = {0}, .times = {257}};
    CHECK(post(input.id, next) == 1);
    // The DOWN, then the first two MOVEs as one, then the others.
    CHECK(holds(take(input.fd), sent[0]));
    CHECK(holds(take(input.fd), Motion{.action = move, .down_time = 1, .ids = {0}, .times = {2, 3}}));
    for (std::size_t index = 3; index < most_events; ++index) CHECK(holds(take(input.fd), sent[index]));
    CHECK(holds(take(input.fd), next));
    CHECK(take(input.fd).result == -eagain);
    call(nr_close, {unsigned_of(input.fd)});
}

TEST(a_merge_needs_the_same_pointers_and_down_time) {
    const auto input = open_input();
    // Neighbours never have the same pointers, whether they differ in count or only in their IDs: no pair can
    // merge.
    const std::vector<std::int32_t> shapes[] = {{0}, {0, 1}, {0, 2}};
    std::vector<Motion> sent;
    for (std::int64_t time = 1; sent.size() < most_events; ++time) {
        sent.push_back({.action = move, .down_time = 1, .ids = shapes[time % 3], .times = {time}});
    }
    for (const auto &motion : sent) CHECK(post(input.id, motion) == 1);
    const std::vector<std::int32_t> last_ids = sent.back().ids;
    CHECK(post(input.id, Motion{.action = down, .down_time = 99, .ids = {0}, .times = {300}}) == 0);
    CHECK(post(input.id, Motion{.action = move, .down_time = 2, .ids = last_ids, .times = {301}}) == 0);
    CHECK(post(input.id, Motion{.action = move, .down_time = 1, .ids = {0}, .times = {302}}) == 0);
    CHECK(post(input.id, Motion{.action = move, .down_time = 1, .ids = {0, 2}, .times = {302}}) == 0);
    // A MOVE like the last merges with it.
    CHECK(post(input.id, Motion{.action = move, .down_time = 1, .ids = last_ids, .times = {303}}) == 1);
    for (std::size_t index = 0; index + 1 < most_events; ++index) CHECK(holds(take(input.fd), sent[index]));
    CHECK(holds(take(input.fd), Motion{.action = move, .down_time = 1, .ids = last_ids, .times = {256, 303}}));
    CHECK(take(input.fd).result == -eagain);
    call(nr_close, {unsigned_of(input.fd)});
}

TEST(only_two_moves_merge) {
    const Motion lift{.action = up, .down_time = 1, .ids = {0}, .times = {5}};
    {
        // A MOVE before an UP does not merge: a new MOVE finds no room.
        const auto input = open_input();
        CHECK(post(input.id, Motion{.action = move, .down_time = 1, .ids = {0}, .times = {4}}) == 1);
        for (std::size_t count = 1; count < most_events; ++count) CHECK(post(input.id, lift) == 1);
        CHECK(post(input.id, Motion{.action = move, .down_time = 1, .ids = {0}, .times = {6}}) == 0);
        call(nr_close, {unsigned_of(input.fd)});
    }
    {
        // Nor does an UP before a MOVE: the new MOVE merges with that MOVE.
        const auto input = open_input();
        for (std::size_t count = 1; count < most_events; ++count) CHECK(post(input.id, lift) == 1);
        CHECK(post(input.id, Motion{.action = move, .down_time = 1, .ids = {0}, .times = {6}}) == 1);
        CHECK(post(input.id, Motion{.action = move, .down_time = 1, .ids = {0}, .times = {7}}) == 1);
        for (std::size_t count = 1; count < most_events; ++count) CHECK(holds(take(input.fd), lift));
        CHECK(holds(take(input.fd), Motion{.action = move, .down_time = 1, .ids = {0}, .times = {6, 7}}));
        call(nr_close, {unsigned_of(input.fd)});
    }
}

/// Fills the channel `id` with an open gesture that cannot merge: a DOWN of pointer 0 at time 1, then
/// POINTER_DOWN and POINTER_UP of pointer 2 in turns, the last a POINTER_DOWN. Returns what it posted.
std::vector<Motion> fill_with_an_open_gesture(std::uint64_t id) {
    std::vector<Motion> sent{{.action = down, .down_time = 1, .ids = {0}, .times = {1}}};
    for (std::int64_t time = 2; sent.size() < most_events; ++time) {
        const std::int32_t action = (sent.size() % 2 == 1 ? pointer_down : pointer_up) | (1 << index_shift);
        sent.push_back({.action = action, .down_time = 1, .ids = {0, 2}, .times = {time}});
    }
    for (const auto &motion : sent) CHECK(post(id, motion) == 1);
    return sent;
}

TEST(a_full_channel_still_takes_the_end_of_the_open_gesture) {
    const auto input = open_input();
    const auto sent = fill_with_an_open_gesture(input.id);
    const Motion second_up{.action = pointer_up | (1 << index_shift), .down_time = 1, .ids = {0, 2}, .times = {300}};
    const Motion last_up{.action = up, .down_time = 1, .ids = {0}, .times = {301}};
    CHECK(post(input.id, second_up) == 1);
    CHECK(post(input.id, last_up) == 1);
    for (const auto &motion : sent) CHECK(holds(take(input.fd), motion));
    CHECK(holds(take(input.fd), second_up));
    CHECK(holds(take(input.fd), last_up));
    CHECK(take(input.fd).result == -eagain);
    call(nr_close, {unsigned_of(input.fd)});
}

TEST(a_full_channel_refuses_a_down_with_the_rest_of_its_gesture) {
    const auto input = open_input();
    // Taps, which never merge.
    for (std::int64_t tap = 0; tap < static_cast<std::int64_t>(most_events / 2); ++tap) {
        CHECK(post(input.id, Motion{.action = down, .down_time = tap, .ids = {0}, .times = {tap}}) == 1);
        CHECK(post(input.id, Motion{.action = up, .down_time = tap, .ids = {0}, .times = {tap}}) == 1);
    }
    constexpr std::int64_t refused = 1000;
    CHECK(post(input.id, Motion{.action = down, .down_time = refused, .ids = {0}, .times = {refused}}) == 0);
    // With room again, the rest of the gesture stays refused, its end included.
    CHECK(take(input.fd).result == 0);
    CHECK(post(input.id, Motion{.action = move, .down_time = refused, .ids = {0}, .times = {refused + 1}}) == 0);
    CHECK(post(input.id, Motion{.action = pointer_down | (1 << index_shift), .down_time = refused, .ids = {0, 1},
                                .times = {refused + 2}}) == 0);
    CHECK(post(input.id, Motion{.action = up, .down_time = refused, .ids = {0}, .times = {refused + 3}}) == 0);
    // The next gesture comes through.
    const Motion next{.action = down, .down_time = 2000, .ids = {0}, .times = {2000}};
    CHECK(post(input.id, next) == 1);
    Taken last{}, taken;
    while ((taken = take(input.fd)).result == 0) last = taken;
    CHECK(holds(last, next));
    call(nr_close, {unsigned_of(input.fd)});
}

TEST(a_pointer_down_at_a_full_channel_cancels_the_gesture) {
    const auto input = open_input();
    const auto sent = fill_with_an_open_gesture(input.id);
    // Pointer 1 comes between the two the guest has: they get a CANCEL where this event puts them.
    const Motion third{.action = pointer_down | (1 << index_shift), .down_time = 1, .ids = {0, 1, 2}, .times = {300}};
    CHECK(post(input.id, third) == 0);
    // The rest of the gesture is refused, its end included; the next gesture comes through.
    CHECK(post(input.id, Motion{.action = move, .down_time = 1, .ids = {0, 1, 2}, .times = {301}}) == 0);
    CHECK(post(input.id, Motion{.action = pointer_up | (1 << index_shift), .down_time = 1, .ids = {0, 1, 2},
                                .times = {302}}) == 0);
    CHECK(post(input.id, Motion{.action = cancel, .down_time = 1, .ids = {0, 2}, .times = {303}}) == 0);
    for (const auto &motion : sent) CHECK(holds(take(input.fd), motion));
    const Taken ended = take(input.fd);
    const auto places = third.places();
    CHECK(ended.result == 0 && ended.event.action == cancel && ended.event.pointer_count == 2);
    CHECK(ended.event.sample_count == 1 && ended.event.down_time == 1 && ended.event.sample_times[0] == 300);
    CHECK(ended.event.pointer_ids[0] == 0 && ended.event.pointer_ids[1] == 2 && ended.event.pointer_ids[2] == 0);
    CHECK(std::memcmp(&ended.event.coords[0][0], &places[0], sizeof places[0]) == 0);
    CHECK(std::memcmp(&ended.event.coords[0][1], &places[2], sizeof places[2]) == 0);
    CHECK(take(input.fd).result == -eagain);
    const Motion next{.action = down, .down_time = 400, .ids = {0}, .times = {400}};
    CHECK(post(input.id, next) == 1);
    CHECK(holds(take(input.fd), next));
    call(nr_close, {unsigned_of(input.fd)});
}

// Only a host at fault ends more than one pointer at a time beyond a full channel: 17 ends at most.
TEST(ends_beyond_a_full_channel_stop_at_seventeen) {
    const auto input = open_input();
    fill_with_an_open_gesture(input.id);
    int taken = 0;
    for (std::int64_t end = 0; end < 20; ++end) {
        taken += post(input.id, Motion{.action = cancel, .down_time = 500 + end, .ids = {0}, .times = {500 + end}});
    }
    CHECK(taken == APKRT_MOTION_POINTERS + 1);
    // Nor does the CANCEL of a POINTER_DOWN go beyond them.
    CHECK(post(input.id, Motion{.action = pointer_down | (1 << index_shift), .down_time = 1, .ids = {0, 1, 2},
                                .times = {600}}) == 0);
    std::size_t held = 0;
    while (take(input.fd).result == 0) ++held;
    CHECK(held == most_events + APKRT_MOTION_POINTERS + 1);
    call(nr_close, {unsigned_of(input.fd)});
}

TEST(a_merge_keeps_the_newest_sixteen_samples) {
    const auto input = open_input();
    Motion first{.action = move, .down_time = 1, .ids = {0}, .times = {}}, second = first, merged = first;
    for (std::int64_t time = 1; time <= 10; ++time) first.times.push_back(time);
    for (std::int64_t time = 11; time <= 20; ++time) second.times.push_back(time);
    for (std::int64_t time = 5; time <= 20; ++time) merged.times.push_back(time);
    CHECK(post(input.id, first) == 1 && post(input.id, second) == 1);
    std::vector<Motion> rest;
    for (std::int64_t time = 21; rest.size() + 2 < most_events; ++time) {
        rest.push_back({.action = move, .down_time = 1, .ids = {0}, .times = {time}});
    }
    for (const auto &motion : rest) CHECK(post(input.id, motion) == 1);
    const Motion next{.action = move, .down_time = 1, .ids = {0}, .times = {400}};
    CHECK(post(input.id, next) == 1);
    CHECK(holds(take(input.fd), merged));
    for (const auto &motion : rest) CHECK(holds(take(input.fd), motion));
    CHECK(holds(take(input.fd), next));
    call(nr_close, {unsigned_of(input.fd)});
}

TEST(a_guest_waiting_in_epoll_wakes_at_a_post) {
    const auto input = open_input();
    const auto epoll = call(nr_epoll_create1, {0});
    struct {
        std::uint32_t events;
        std::uint32_t pad;
        std::uint64_t data;
    } interest{epollin, 0, 11}, fired{};
    CHECK(call(nr_epoll_ctl, {unsigned_of(epoll), epoll_ctl_add, unsigned_of(input.fd), address(&interest)}) == 0);
    std::thread host([&input] {
        std::this_thread::sleep_for(50ms);
        post(input.id, Motion{.action = down, .down_time = 1, .ids = {0}, .times = {1}});
    });
    const auto count = call(nr_epoll_pwait, {unsigned_of(epoll), address(&fired), 1, 2000, 0, 8});
    host.join();
    CHECK(count == 1 && fired.data == 11 && (fired.events & epollin) != 0);
    CHECK(take(input.fd).result == 0);
    // Level-triggered: nothing more once it is read.
    CHECK(call(nr_epoll_pwait, {unsigned_of(epoll), address(&fired), 1, 0, 0, 8}) == 0);
    call(nr_close, {unsigned_of(epoll)});
    call(nr_close, {unsigned_of(input.fd)});
}

TEST(the_input_pipe_holds_a_byte_per_event_until_the_last_is_read) {
    const auto input = open_input();
    CHECK(pipe_bytes(input.fd) == 0);
    CHECK(post(input.id, Motion{.action = down, .down_time = 1, .ids = {0}, .times = {1}}) == 1);
    CHECK(post(input.id, Motion{.action = up, .down_time = 1, .ids = {0}, .times = {2}}) == 1);
    CHECK(pipe_bytes(input.fd) == 2);
    CHECK(take(input.fd).result == 0);
    CHECK(pipe_bytes(input.fd) == 2);
    CHECK(take(input.fd).result == 0);
    CHECK(pipe_bytes(input.fd) == 0);
    call(nr_close, {unsigned_of(input.fd)});
}

// A host thread posts MOVEs while the guest reads: the samples arrive in their order, and none is refused,
// since a full channel merges them.
TEST(events_posted_from_another_thread_arrive_in_order) {
    const auto input = open_input();
    constexpr std::int64_t last = 20'000;
    std::atomic<int> refused{0};
    std::thread host([&input, &refused] {
        for (std::int64_t time = 1; time <= last; ++time) {
            if (post(input.id, Motion{.action = move, .down_time = 1, .ids = {0}, .times = {time}}) != 1) ++refused;
        }
    });
    const auto give_up = std::chrono::steady_clock::now() + 20s;
    std::int64_t newest = 0;
    bool rising = true;
    while (newest < last && std::chrono::steady_clock::now() < give_up) {
        const Taken taken = take(input.fd);
        if (taken.result == -eagain) {
            std::this_thread::yield();  // Darwin's mutexes would let this thread take the locks again first
            continue;
        }
        if (taken.result != 0) break;
        for (std::uint32_t sample = 0; sample < taken.event.sample_count; ++sample) {
            rising = rising && taken.event.sample_times[sample] > newest;
            newest = taken.event.sample_times[sample];
        }
    }
    host.join();
    CHECK(refused.load() == 0);
    CHECK(rising && newest == last);
    CHECK(!readable(input.fd));
    call(nr_close, {unsigned_of(input.fd)});
}

// A host thread posts while the guest connects, reads and closes one channel after another: no post lands in
// a channel that has ended.
TEST(input_channels_come_and_go_while_the_host_posts) {
    std::atomic<std::uint64_t> current{0};
    std::atomic<bool> stop{false};
    std::thread host([&current, &stop] {
        for (std::int64_t time = 1; !stop.load(); ++time) {
            post(current.load(), Motion{.action = move, .down_time = 1, .ids = {0}, .times = {time}});
        }
    });
    int failed = 0;
    for (int round = 0; round < 1000; ++round) {
        const auto input = open_input();
        current.store(input.id);
        const auto taken = take(input.fd).result;
        if (input.fd < 0 || (taken != 0 && taken != -eagain)) ++failed;
        call(nr_close, {unsigned_of(input.fd)});
        if (post(input.id, Motion{.action = move, .down_time = 1, .ids = {0}, .times = {1}}) != 0) ++failed;
    }
    stop.store(true);
    host.join();
    CHECK(failed == 0);
}

// ADR-0068 step 0: the channel carries kinds. A key, a text and a device record travel in the motion record's
// place, told apart by `kind` (the field that was reserved), in the order they were posted.

/// What a key post fills in, with values the round trip can tell apart.
apkrt_key_event key_of(std::int32_t action, std::int32_t key_code) {
    apkrt_key_event key{};
    key.action = action;
    key.down_time = 11;
    key.event_time = 12;
    key.key_code = key_code;
    key.scan_code = 13;
    key.meta_state = 0x41;
    key.repeat_count = 2;
    key.device_id = 7;
    key.flags = 0x8;
    key.source = 0x101;
    return key;
}

/// A record read back as its kind's struct.
template <typename Record>
Record as(const Taken &taken) {
    Record record;
    std::memcpy(&record, &taken.event, sizeof record);
    return record;
}

TEST(a_key_record_arrives_whole_and_tells_its_kind) {
    const auto input = open_input();
    const apkrt_key_event key = key_of(0, 29);
    CHECK(apkrt_input_post_key(input.id, &key) == 1);
    const auto taken = take(input.fd);
    CHECK(taken.result == 0);
    CHECK(taken.event.kind == APKRT_INPUT_KEY);
    const auto read = as<apkrt_key_event>(taken);
    CHECK(read.action == 0 && read.key_code == 29 && read.scan_code == 13 && read.meta_state == 0x41);
    CHECK(read.repeat_count == 2 && read.device_id == 7 && read.flags == 0x8 && read.source == 0x101);
    CHECK(read.down_time == 11 && read.event_time == 12);
    call(nr_close, {unsigned_of(input.fd)});
}

TEST(a_key_that_is_not_sound_is_refused) {
    const auto input = open_input();
    apkrt_key_event key = key_of(0, 29);
    CHECK(apkrt_input_post_key(input.id, nullptr) == 0);
    key.action = 2;  // AKEY_EVENT_ACTION_MULTIPLE is not a key the host posts
    CHECK(apkrt_input_post_key(input.id, &key) == 0);
    key.action = -1;
    CHECK(apkrt_input_post_key(input.id, &key) == 0);
    key.action = 1;
    key.key_code = -1;
    CHECK(apkrt_input_post_key(input.id, &key) == 0);
    CHECK(take(input.fd).result == -eagain);
    call(nr_close, {unsigned_of(input.fd)});
}

TEST(a_text_record_carries_its_bytes_and_its_serial) {
    const auto input = open_input();
    apkrt_text_event text{};
    text.op = APKRT_TEXT_COMMIT;
    text.arg0 = 1;
    text.serial = 5;
    const char bytes[] = "h\xc3\xa9llo";
    text.length = sizeof bytes - 1;
    std::memcpy(text.text, bytes, text.length);
    CHECK(apkrt_input_post_text(input.id, &text) == 1);
    const auto taken = take(input.fd);
    CHECK(taken.result == 0 && taken.event.kind == APKRT_INPUT_TEXT);
    const auto read = as<apkrt_text_event>(taken);
    CHECK(read.op == APKRT_TEXT_COMMIT && read.arg0 == 1 && read.serial == 5 && read.length == text.length);
    CHECK(std::memcmp(read.text, bytes, read.length) == 0);
    call(nr_close, {unsigned_of(input.fd)});
}

TEST(a_text_that_is_not_sound_is_refused) {
    const auto input = open_input();
    apkrt_text_event text{};
    text.op = APKRT_TEXT_COMMIT;
    text.length = APKRT_TEXT_BYTES;  // the largest text still fits
    CHECK(apkrt_input_post_text(input.id, &text) == 1);
    text.length = APKRT_TEXT_BYTES + 1;
    CHECK(apkrt_input_post_text(input.id, &text) == 0);
    text.length = 0;
    text.op = APKRT_TEXT_OPS;
    CHECK(apkrt_input_post_text(input.id, &text) == 0);
    text.op = -1;
    CHECK(apkrt_input_post_text(input.id, &text) == 0);
    CHECK(apkrt_input_post_text(input.id, nullptr) == 0);
    CHECK(take(input.fd).result == 0);
    CHECK(take(input.fd).result == -eagain);
    call(nr_close, {unsigned_of(input.fd)});
}

TEST(a_device_record_carries_its_name) {
    const auto input = open_input();
    apkrt_device_event device{};
    device.op = APKRT_DEVICE_ADDED;
    device.device_id = 1001;
    device.sources = 0x01000510;
    device.vendor_id = 0x45e;
    device.product_id = 0x2e0;
    std::strcpy(device.name, "Minato Pad");
    CHECK(apkrt_input_post_device(input.id, &device) == 1);
    const auto taken = take(input.fd);
    CHECK(taken.result == 0 && taken.event.kind == APKRT_INPUT_DEVICE);
    const auto read = as<apkrt_device_event>(taken);
    CHECK(read.op == APKRT_DEVICE_ADDED && read.device_id == 1001 && read.sources == 0x01000510);
    CHECK(read.vendor_id == 0x45e && read.product_id == 0x2e0 && std::strcmp(read.name, "Minato Pad") == 0);
    device.name[sizeof device.name - 1] = 'x';  // a name without its NUL is refused
    std::memset(device.name, 'x', sizeof device.name);
    CHECK(apkrt_input_post_device(input.id, &device) == 0);
    device.op = APKRT_DEVICE_OPS;
    device.name[0] = 0;
    CHECK(apkrt_input_post_device(input.id, &device) == 0);
    call(nr_close, {unsigned_of(input.fd)});
}

TEST(a_device_name_is_utf8_that_java_takes_as_it_is) {
    const auto input = open_input();
    apkrt_device_event device{};
    device.op = APKRT_DEVICE_ADDED;
    device.device_id = 1001;
    const auto named = [&](const char *name) {
        std::memset(device.name, 0, sizeof device.name);
        std::strcpy(device.name, name);
        return apkrt_input_post_device(input.id, &device);
    };
    CHECK(named("Tastatur \xc3\xa9 \xe6\x97\xa5\xe6\x9c\xac") == 1);  // 2- and 3-byte characters
    CHECK(named("\xf0\x9f\x98\x80") == 0);                              // U+1F600, a 4-byte form
    CHECK(named("\xc3") == 0);                                              // a cut sequence
    CHECK(named("a\xc3(") == 0);                                            // a bad continuation
    CHECK(named("\x80") == 0);                                              // a continuation byte alone
    CHECK(named("\xc0\xaf") == 0);                                         // an overlong 2-byte form
    CHECK(named("\xe0\x80\xaf") == 0);                                     // an overlong 3-byte form
    CHECK(named("\xed\xa0\x80") == 0);                                     // a surrogate, U+D800
    CHECK(named("\xe2\x82") == 0);                                          // a 3-byte lead with one byte
    CHECK(named("\xe2\x82\xac") == 1);                                     // U+20AC
    call(nr_close, {unsigned_of(input.fd)});
}

apkrt_axes_event axes_of(std::int32_t action, std::int32_t device, std::int32_t source) {
    apkrt_axes_event axes{};
    axes.action = action;
    axes.event_time = 1000;
    axes.device_id = device;
    axes.source = source;
    return axes;
}

TEST(a_pad_axes_record_arrives_whole_and_tells_its_kind) {
    const auto input = open_input();
    apkrt_axes_event axes = axes_of(2, 1001, 0x01000010);  // MOVE of a joystick
    axes.reserved0[0] = axes.reserved0[1] = 99;            // not the host's to set
    axes.axis_count = 3;
    axes.axes[0] = {0, 0.5f};     // AXIS_X
    axes.axes[1] = {1, -1.0f};    // AXIS_Y
    axes.axes[2] = {17, 0.25f};   // AXIS_LTRIGGER
    CHECK(apkrt_input_post_axes(input.id, &axes) == 1);
    const auto taken = take(input.fd);
    CHECK(taken.result == 0 && taken.event.kind == APKRT_INPUT_AXES);
    const auto read = as<apkrt_axes_event>(taken);
    CHECK(read.action == 2 && read.event_time == 1000 && read.device_id == 1001 && read.source == 0x01000010);
    CHECK(read.reserved0[0] == 0 && read.reserved0[1] == 0 && read.tool_type == 0 && read.button_state == 0);
    CHECK(read.axis_count == 3 && read.axes[0].axis == 0 && read.axes[0].value == 0.5f);
    CHECK(read.axes[1].axis == 1 && read.axes[1].value == -1.0f && read.axes[2].axis == 17 &&
          read.axes[2].value == 0.25f);
    call(nr_close, {unsigned_of(input.fd)});
}

TEST(a_mouse_axes_record_carries_its_tool_buttons_and_scroll) {
    const auto input = open_input();
    apkrt_axes_event axes = axes_of(8, 1002, 0x00002002);  // SCROLL of a mouse
    axes.tool_type = 3;
    axes.button_state = 0x1;
    axes.meta_state = 0x41;
    axes.axis_count = 3;
    axes.axes[0] = {0, 640.0f};   // AXIS_X in the window's pixels
    axes.axes[1] = {1, 1200.0f};
    axes.axes[2] = {9, -2.0f};    // AXIS_VSCROLL
    CHECK(apkrt_input_post_axes(input.id, &axes) == 1);
    const auto read = as<apkrt_axes_event>(take(input.fd));
    CHECK(read.tool_type == 3 && read.button_state == 0x1 && read.meta_state == 0x41 && read.axis_count == 3);
    CHECK(read.axes[2].axis == 9 && read.axes[2].value == -2.0f);
    // HOVER_MOVE, HOVER_ENTER, HOVER_EXIT, BUTTON_PRESS, BUTTON_RELEASE
    for (const std::int32_t action : {7, 9, 10, 11, 12}) {
        axes.action = action;
        CHECK(apkrt_input_post_axes(input.id, &axes) == 1);
    }
    call(nr_close, {unsigned_of(input.fd)});
}

TEST(axes_that_are_not_sound_are_refused) {
    const auto input = open_input();
    const apkrt_axes_event good = [] {
        apkrt_axes_event axes = axes_of(2, 1001, 0x01000010);
        axes.axis_count = 2;
        axes.axes[0] = {0, 0.5f};
        axes.axes[1] = {1, 0.5f};
        return axes;
    }();
    CHECK(apkrt_input_post_axes(input.id, nullptr) == 0);
    const auto refused = [&](auto change) {
        apkrt_axes_event axes = good;
        change(axes);
        return apkrt_input_post_axes(input.id, &axes) == 0;
    };
    CHECK(refused([](apkrt_axes_event &a) { a.action = 0; }));                // a touch action is not an axes record's
    CHECK(refused([](apkrt_axes_event &a) { a.action = 13; }));
    CHECK(refused([](apkrt_axes_event &a) { a.action = -1; }));
    CHECK(refused([](apkrt_axes_event &a) { a.device_id = 0; }));
    CHECK(refused([](apkrt_axes_event &a) { a.device_id = -1; }));
    CHECK(refused([](apkrt_axes_event &a) { a.source = 0; }));
    CHECK(refused([](apkrt_axes_event &a) { a.tool_type = 6; }));
    CHECK(refused([](apkrt_axes_event &a) { a.tool_type = -1; }));
    CHECK(refused([](apkrt_axes_event &a) { a.axis_count = APKRT_AXES_MAX + 1; }));
    CHECK(refused([](apkrt_axes_event &a) { a.axes[1].axis = 0; }));          // an axis named twice
    CHECK(refused([](apkrt_axes_event &a) { a.axes[1].axis = -1; }));
    CHECK(refused([](apkrt_axes_event &a) { a.axes[1].axis = APKRT_AXIS_LIMIT; }));
    CHECK(refused([](apkrt_axes_event &a) { a.axes[1].value = std::numeric_limits<float>::quiet_NaN(); }));
    CHECK(refused([](apkrt_axes_event &a) { a.axes[0].value = std::numeric_limits<float>::infinity(); }));
    CHECK(take(input.fd).result == -eagain);
    // The axes past axis_count are not read, so what is there does not matter; 16 axes and none are sound.
    apkrt_axes_event many = good;
    many.axes[5] = {0, std::numeric_limits<float>::quiet_NaN()};
    CHECK(apkrt_input_post_axes(input.id, &many) == 1);
    many.axis_count = APKRT_AXES_MAX;
    for (std::int32_t at = 0; at < APKRT_AXES_MAX; ++at) many.axes[at] = {at + 40, 0.0f};
    CHECK(apkrt_input_post_axes(input.id, &many) == 1);
    call(nr_close, {unsigned_of(input.fd)});
}

// A full channel does not grow for a stick: the newest MOVE of the same device, source and action is erased and the
// new report goes to the back, so that event times keep their order; a device that has no MOVE there is refused.
TEST(a_full_channel_replaces_the_newest_axes_move_of_the_same_device_at_the_back) {
    const auto input = open_input();
    const apkrt_key_event key = key_of(0, 29);
    apkrt_axes_event stick = axes_of(2, 1001, 0x01000010);
    stick.axis_count = 1;
    stick.axes[0] = {0, 0.1f};
    CHECK(apkrt_input_post_axes(input.id, &stick) == 1);
    apkrt_axes_event other = axes_of(2, 1002, 0x01000010);
    other.axis_count = 1;
    other.axes[0] = {0, 0.9f};
    CHECK(apkrt_input_post_axes(input.id, &other) == 1);
    for (std::size_t count = 2; count < most_events; ++count) CHECK(apkrt_input_post_key(input.id, &key) == 1);
    CHECK(apkrt_input_post_key(input.id, &key) == 0);  // full
    stick.axes[0].value = 0.7f;
    stick.event_time = 2000;
    CHECK(apkrt_input_post_axes(input.id, &stick) == 1);  // erases the first, goes to the back
    stick.device_id = 1003;  // no MOVE of this device is waiting
    CHECK(apkrt_input_post_axes(input.id, &stick) == 0);
    stick.device_id = 1001;
    stick.source = 0x00002002;  // nor of this source
    CHECK(apkrt_input_post_axes(input.id, &stick) == 0);
    const auto first = as<apkrt_axes_event>(take(input.fd));
    CHECK(first.device_id == 1002 && first.axes[0].value == 0.9f);
    for (std::size_t count = 2; count < most_events; ++count) CHECK(take(input.fd).event.kind == APKRT_INPUT_KEY);
    const auto last = as<apkrt_axes_event>(take(input.fd));
    CHECK(last.device_id == 1001 && last.axes[0].value == 0.7f && last.event_time == 2000);
    CHECK(take(input.fd).result == -eagain);
    call(nr_close, {unsigned_of(input.fd)});
}

// Presses, releases and scrolls are not merged: a full channel takes them in the slack the ends of a touch gesture
// have, as many as that holds.
TEST(a_full_channel_takes_axes_that_are_not_moves_in_the_slack_of_the_touch_ends) {
    const auto input = open_input();
    const apkrt_key_event key = key_of(0, 29);
    for (std::size_t count = 0; count < most_events; ++count) CHECK(apkrt_input_post_key(input.id, &key) == 1);
    apkrt_axes_event release = axes_of(12, 1002, 0x00002002);  // BUTTON_RELEASE of a mouse
    release.tool_type = 3;
    std::size_t taken = 0;
    while (apkrt_input_post_axes(input.id, &release) == 1) ++taken;
    CHECK(taken > 0 && taken == most_ends);
    release.action = 8;  // SCROLL
    CHECK(apkrt_input_post_axes(input.id, &release) == 0);
    for (std::size_t count = 0; count < most_events; ++count) CHECK(take(input.fd).event.kind == APKRT_INPUT_KEY);
    for (std::size_t count = 0; count < taken; ++count) CHECK(take(input.fd).event.kind == APKRT_INPUT_AXES);
    call(nr_close, {unsigned_of(input.fd)});
}

// A mouse or trackpad as the host reports it (apkrt_input_post_mouse): a move, a button's press or release and a
// scroll, each of which the runtime turns into the records Android 15 makes of a mouse, measured by mouse-test with a
// `uinput` mouse (hover enter and move, hover exit before the first button's DOWN and BUTTON_PRESS, MOVE for more
// buttons, BUTTON_RELEASE, UP and a hover enter after the last, BACK and FORWARD as keys, a hover move before each
// scroll).
apkrt_axes_event mouse_of(std::int32_t action, std::int32_t buttons, float x = 0, float y = 0) {
    apkrt_axes_event mouse = axes_of(action, 1002, 0x00002002);
    mouse.tool_type = 3;
    mouse.button_state = buttons;
    mouse.axis_count = 2;
    mouse.axes[0] = {0, x};
    mouse.axes[1] = {1, y};
    return mouse;
}

float axis_value(const apkrt_axes_event &axes, std::int32_t axis) {
    for (std::uint32_t at = 0; at < axes.axis_count; ++at) {
        if (axes.axes[at].axis == axis) return axes.axes[at].value;
    }
    return 0.0f;
}

bool has_axis(const apkrt_axes_event &axes, std::int32_t axis) {
    for (std::uint32_t at = 0; at < axes.axis_count; ++at) {
        if (axes.axes[at].axis == axis) return true;
    }
    return false;
}

/// What the channel holds, taken: a short word for each record, an axes record's action (HE hover enter, HM hover move,
/// HX hover exit, DOWN, MV move, UP, PRESS, REL release, SCR scroll) with "/" and its button state in hex, and for a
/// press or release "/" and its action button; a key's "kd" or "ku" and its key code. The records are in `last`.
std::string mouse_trace(std::int64_t channel, std::vector<apkrt_axes_event> *last = nullptr) {
    std::string trace;
    if (last != nullptr) last->clear();
    for (;;) {
        const auto taken = take(channel);
        if (taken.result != 0) break;
        if (!trace.empty()) trace += ' ';
        if (taken.event.kind == APKRT_INPUT_KEY) {
            apkrt_key_event key;
            std::memcpy(&key, &taken.event, sizeof key);
            trace += (key.action == 0 ? "kd" : "ku") + std::to_string(key.key_code);
            if (last != nullptr) last->push_back({});
            continue;
        }
        const auto axes = as<apkrt_axes_event>(taken);
        if (last != nullptr) last->push_back(axes);
        static const char *const names[] = {"DOWN", "UP", "MV", "?", "?", "?", "?", "HM", "SCR", "HE", "HX", "PRESS",
                                            "REL"};
        trace += names[axes.action] + std::string("/") + [&] {
            char hex[16];
            std::snprintf(hex, sizeof hex, "%x", axes.button_state);
            return std::string(hex);
        }();
        if (axes.action == 11 || axes.action == 12) trace += "/" + std::to_string(axes.reserved0[0]);
    }
    return trace;
}

TEST(a_mouse_moves_as_hover_enter_then_hover_moves_with_its_position_and_relative_axes) {
    const auto input = open_input();
    std::vector<apkrt_axes_event> got;
    apkrt_axes_event hover = mouse_of(7, 0, 30, 20);
    hover.axis_count = 4;
    hover.axes[2] = {27, 30.0f};  // AXIS_RELATIVE_X
    hover.axes[3] = {28, 20.0f};
    CHECK(apkrt_input_post_mouse(input.id, &hover) == 1);
    CHECK(mouse_trace(input.fd, &got) == "HE/0");
    CHECK(got[0].source == 0x2002 && got[0].tool_type == 3 && axis_value(got[0], 0) == 30.0f &&
          axis_value(got[0], 1) == 20.0f && axis_value(got[0], 27) == 30.0f && axis_value(got[0], 28) == 20.0f);
    hover.axes[0].value = 40;
    CHECK(apkrt_input_post_mouse(input.id, &hover) == 1);
    CHECK(mouse_trace(input.fd) == "HM/0");
    // The pointer leaves the window: a hover exit; its return is a hover enter again.
    apkrt_axes_event leave = mouse_of(10, 0);
    CHECK(apkrt_input_post_mouse(input.id, &leave) == 1);
    // (Android makes the exit from the last hover record, relative axes and all.)
    CHECK(mouse_trace(input.fd, &got) == "HX/0" && axis_value(got[0], 27) == 30.0f);
    CHECK(apkrt_input_post_mouse(input.id, &hover) == 1);
    CHECK(mouse_trace(input.fd) == "HE/0");
    call(nr_close, {unsigned_of(input.fd)});
}

TEST(a_mouse_click_is_hover_exit_down_press_then_release_up_hover_enter) {
    const auto input = open_input();
    std::vector<apkrt_axes_event> got;
    apkrt_axes_event first = mouse_of(7, 0, 5, 6);
    CHECK(apkrt_input_post_mouse(input.id, &first) == 1);
    mouse_trace(input.fd);
    apkrt_axes_event press = mouse_of(11, 1, 5, 6);
    CHECK(apkrt_input_post_mouse(input.id, &press) == 1);
    CHECK(mouse_trace(input.fd, &got) == "HX/1 DOWN/1 PRESS/1/1");
    CHECK(axis_value(got[1], 0) == 5.0f && axis_value(got[1], 1) == 6.0f);  // the last position, with no relative axes
    CHECK(!has_axis(got[1], 27) && got[1].tool_type == 3);
    // Held, a hover is the touch's MOVE.
    apkrt_axes_event drag = mouse_of(7, 1, 15, 16);
    CHECK(apkrt_input_post_mouse(input.id, &drag) == 1);
    CHECK(mouse_trace(input.fd, &got) == "MV/1" && axis_value(got[0], 0) == 15.0f);
    apkrt_axes_event release = mouse_of(12, 0);
    CHECK(apkrt_input_post_mouse(input.id, &release) == 1);
    CHECK(mouse_trace(input.fd) == "REL/0/1 UP/0 HE/0");
    // A second button while one is held: a MOVE and the press; the release of one: the release and a MOVE.
    press.button_state = 1;
    CHECK(apkrt_input_post_mouse(input.id, &press) == 1);
    mouse_trace(input.fd);
    press.button_state = 3;
    CHECK(apkrt_input_post_mouse(input.id, &press) == 1);
    CHECK(mouse_trace(input.fd) == "MV/3 PRESS/3/2");
    release.button_state = 1;
    CHECK(apkrt_input_post_mouse(input.id, &release) == 1);
    CHECK(mouse_trace(input.fd) == "REL/1/2 MV/1");
    release.button_state = 0;
    CHECK(apkrt_input_post_mouse(input.id, &release) == 1);
    CHECK(mouse_trace(input.fd) == "REL/0/1 UP/0 HE/0");
    // A release of a button that was not held says nothing.
    CHECK(apkrt_input_post_mouse(input.id, &release) == 1);
    CHECK(mouse_trace(input.fd).empty());
    call(nr_close, {unsigned_of(input.fd)});
}

TEST(a_mouses_back_and_forward_buttons_are_keys_with_a_hover_move_between) {
    const auto input = open_input();
    apkrt_axes_event hover = mouse_of(7, 0, 5, 6);
    CHECK(apkrt_input_post_mouse(input.id, &hover) == 1);
    mouse_trace(input.fd);
    apkrt_axes_event press = mouse_of(11, 8);  // BUTTON_BACK
    CHECK(apkrt_input_post_mouse(input.id, &press) == 1);
    CHECK(mouse_trace(input.fd) == "kd4 HM/8");
    apkrt_axes_event release = mouse_of(12, 0);
    CHECK(apkrt_input_post_mouse(input.id, &release) == 1);
    CHECK(mouse_trace(input.fd) == "HM/0 ku4");
    press.button_state = 0x10;  // BUTTON_FORWARD
    CHECK(apkrt_input_post_mouse(input.id, &press) == 1);
    CHECK(mouse_trace(input.fd) == "kd125 HM/10");
    CHECK(apkrt_input_post_mouse(input.id, &release) == 1);
    CHECK(mouse_trace(input.fd) == "HM/0 ku125");
    call(nr_close, {unsigned_of(input.fd)});
}

TEST(a_mouse_scroll_is_a_hover_move_and_the_scroll_with_the_wheels_axes) {
    const auto input = open_input();
    std::vector<apkrt_axes_event> got;
    apkrt_axes_event hover = mouse_of(7, 0, 5, 6);
    CHECK(apkrt_input_post_mouse(input.id, &hover) == 1);
    mouse_trace(input.fd);
    apkrt_axes_event scroll = mouse_of(8, 0);
    scroll.axis_count = 2;  // no position: the last one
    scroll.axes[0] = {9, -2.0f};   // AXIS_VSCROLL
    scroll.axes[1] = {10, 0.5f};   // AXIS_HSCROLL
    CHECK(apkrt_input_post_mouse(input.id, &scroll) == 1);
    CHECK(mouse_trace(input.fd, &got) == "HM/0 SCR/0");
    CHECK(!has_axis(got[0], 27) && axis_value(got[0], 0) == 5.0f && axis_value(got[0], 1) == 6.0f);
    CHECK(axis_value(got[1], 9) == -2.0f && axis_value(got[1], 10) == 0.5f && axis_value(got[1], 0) == 5.0f &&
          axis_value(got[1], 1) == 6.0f && got[1].tool_type == 3);
    call(nr_close, {unsigned_of(input.fd)});
}

// A mouse that goes while it hovers leaves the window first (mouse-test: the hover exit comes before the removal).
TEST(a_mouse_that_goes_while_it_hovers_leaves_the_window_first) {
    const auto input = open_input();
    apkrt_axes_event hover = mouse_of(7, 0, 5, 6);
    CHECK(apkrt_input_post_mouse(input.id, &hover) == 1);
    mouse_trace(input.fd);
    apkrt_device_event gone{};
    gone.op = APKRT_DEVICE_REMOVED;
    gone.device_id = 1002;
    gone.sources = 0x2002;
    std::strcpy(gone.name, "Mouse");
    CHECK(apkrt_input_post_device(input.id, &gone) == 1);
    CHECK(take(input.fd).event.kind == APKRT_INPUT_AXES);
    const auto device = take(input.fd);
    CHECK(device.event.kind == APKRT_INPUT_DEVICE);
    CHECK(take(input.fd).result == -eagain);
    // Not hovering any more, a second removal makes no exit.
    CHECK(apkrt_input_post_device(input.id, &gone) == 1);
    CHECK(take(input.fd).event.kind == APKRT_INPUT_DEVICE);
    CHECK(take(input.fd).result == -eagain);
    call(nr_close, {unsigned_of(input.fd)});
}

// Render scale of an input channel (ADR-0067 step 2): the host posts the visible window's pixels, the guest's window
// is `scale` times as big, so a position and a size in pixels are multiplied by the scale on its way in. Needs
// <cmath>. Insert before
// a_mouse_record_that_is_not_one_is_refused.

/// A touch at (x, y), posted to `id` and read back from `fd`; the position the guest sees.
apkrt_pointer_coords touch_at(std::uint64_t id, std::int64_t fd, float x, float y) {
    const std::int32_t ids[1] = {0};
    const std::int64_t times[1] = {2};
    const apkrt_pointer_coords where[1] = {{x, y, 0.5f, 3.0f}};
    CHECK(apkrt_input_post(id, move, 1, 1, ids, 1, times, where) == 1);
    const auto taken = take(fd);
    CHECK(taken.result == 0);
    return taken.event.coords[0][0];
}

TEST(a_render_scale_multiplies_a_touchs_position_and_size_but_not_its_pressure) {
    const auto input = open_input();
    // Scale 1 is the default and changes nothing, bit for bit.
    const auto plain = touch_at(input.id, input.fd, 123.25f, 456.5f);
    CHECK(plain.x == 123.25f && plain.y == 456.5f);
    CHECK(apkrt_input_set_scale(input.id, 0.5) == 1);
    const auto half = touch_at(input.id, input.fd, 500.0f, 1000.0f);
    CHECK(half.x == 250.0f && half.y == 500.0f);
    // The touch size is in pixels too and follows; the pressure is not a length.
    CHECK(half.pressure == 0.5f && half.touch_major == 1.5f);
    CHECK(apkrt_input_set_scale(input.id, 0.25) == 1);
    const auto quarter = touch_at(input.id, input.fd, 400.0f, 80.0f);
    CHECK(quarter.x == 100.0f && quarter.y == 20.0f);
    CHECK(apkrt_input_set_scale(input.id, 1.0) == 1);
    const auto back = touch_at(input.id, input.fd, 123.25f, 456.5f);
    CHECK(back.x == 123.25f && back.y == 456.5f);
    call(nr_close, {unsigned_of(input.fd)});
}

TEST(a_render_scale_is_taken_at_the_post_not_at_the_read) {
    const auto input = open_input();
    CHECK(apkrt_input_set_scale(input.id, 0.5) == 1);
    const std::int32_t ids[1] = {0};
    const std::int64_t times[1] = {2};
    const apkrt_pointer_coords where[1] = {{600.0f, 800.0f, 0.5f, 0.0f}};
    CHECK(apkrt_input_post(input.id, move, 1, 1, ids, 1, times, where) == 1);
    CHECK(apkrt_input_set_scale(input.id, 1.0) == 1);  // changes the next post only
    const auto taken = take(input.fd);
    CHECK(taken.event.coords[0][0].x == 300.0f && taken.event.coords[0][0].y == 400.0f);
    call(nr_close, {unsigned_of(input.fd)});
}

TEST(a_render_scale_only_takes_a_known_id_and_a_scale_from_a_quarter_to_one) {
    const auto input = open_input();
    CHECK(apkrt_input_set_scale(0, 0.5) == 0);
    CHECK(apkrt_input_set_scale(1'000'000, 0.5) == 0);
    for (const double bad : {0.0, 0.2499, 1.0001, 2.0, -0.5, std::nan(""), HUGE_VAL, -HUGE_VAL}) {
        CHECK(apkrt_input_set_scale(input.id, bad) == 0);
    }
    // A refused scale leaves the one set before.
    CHECK(apkrt_input_set_scale(input.id, 0.5) == 1);
    CHECK(apkrt_input_set_scale(input.id, 3.0) == 0);
    const auto still = touch_at(input.id, input.fd, 100.0f, 100.0f);
    CHECK(still.x == 50.0f && still.y == 50.0f);
    call(nr_close, {unsigned_of(input.fd)});
    // After the channel ended, as for the posts.
    CHECK(apkrt_input_set_scale(input.id, 0.5) == 0);
}

TEST(a_render_scale_belongs_to_one_channel) {
    const auto scaled = open_input();
    const auto other = open_input();
    CHECK(apkrt_input_set_scale(scaled.id, 0.5) == 1);
    const auto a = touch_at(other.id, other.fd, 100.0f, 200.0f);
    CHECK(a.x == 100.0f && a.y == 200.0f);
    const auto b = touch_at(scaled.id, scaled.fd, 100.0f, 200.0f);
    CHECK(b.x == 50.0f && b.y == 100.0f);
    call(nr_close, {unsigned_of(scaled.fd)});
    call(nr_close, {unsigned_of(other.fd)});
}

TEST(a_render_scale_multiplies_a_mouses_position_and_keeps_its_scroll_and_relative_axes) {
    const auto input = open_input();
    CHECK(apkrt_input_set_scale(input.id, 0.5) == 1);
    std::vector<apkrt_axes_event> got;
    apkrt_axes_event hover = mouse_of(7, 0, 600, 400);
    hover.axis_count = 4;
    hover.axes[2] = {27, 30.0f};  // AXIS_RELATIVE_X
    hover.axes[3] = {28, 20.0f};
    CHECK(apkrt_input_post_mouse(input.id, &hover) == 1);
    CHECK(mouse_trace(input.fd, &got) == "HE/0");
    CHECK(axis_value(got[0], 0) == 300.0f && axis_value(got[0], 1) == 200.0f);
    CHECK(axis_value(got[0], 27) == 30.0f && axis_value(got[0], 28) == 20.0f);
    // The position a record has none of is the last one, already in the guest's pixels, not scaled twice.
    apkrt_axes_event press = mouse_of(11, 1);
    press.axis_count = 0;
    CHECK(apkrt_input_post_mouse(input.id, &press) == 1);
    CHECK(mouse_trace(input.fd, &got) == "HX/1 DOWN/1 PRESS/1/1");
    CHECK(axis_value(got[1], 0) == 300.0f && axis_value(got[1], 1) == 200.0f);
    apkrt_axes_event release = mouse_of(12, 0);
    release.axis_count = 0;
    CHECK(apkrt_input_post_mouse(input.id, &release) == 1);
    mouse_trace(input.fd);
    // A scroll: the hover move before it is at the scaled position, the wheel is in lines.
    apkrt_axes_event scroll = mouse_of(8, 0, 800, 100);
    scroll.axis_count = 3;
    scroll.axes[2] = {9, -2.0f};  // AXIS_VSCROLL
    CHECK(apkrt_input_post_mouse(input.id, &scroll) == 1);
    CHECK(mouse_trace(input.fd, &got).find("SCR/0") != std::string::npos);
    CHECK(axis_value(got.back(), 0) == 400.0f && axis_value(got.back(), 1) == 50.0f);
    CHECK(axis_value(got.back(), 9) == -2.0f);
    call(nr_close, {unsigned_of(input.fd)});
}

TEST(a_render_scale_leaves_keys_texts_and_a_pads_axes_alone) {
    const auto input = open_input();
    CHECK(apkrt_input_set_scale(input.id, 0.5) == 1);
    apkrt_axes_event pad = axes_of(2, 1001, 0x01000010);  // a joystick: AXIS_X and AXIS_Y are the stick, not a place
    pad.axis_count = 2;
    pad.axes[0] = {0, 0.5f};
    pad.axes[1] = {1, -1.0f};
    CHECK(apkrt_input_post_axes(input.id, &pad) == 1);
    const auto read = as<apkrt_axes_event>(take(input.fd));
    CHECK(read.axes[0].value == 0.5f && read.axes[1].value == -1.0f);
    apkrt_key_event key{};
    key.action = 0;
    key.key_code = 29;
    CHECK(apkrt_input_post_key(input.id, &key) == 1);
    const auto taken = take(input.fd);
    CHECK(taken.result == 0 && taken.event.kind == APKRT_INPUT_KEY);
    call(nr_close, {unsigned_of(input.fd)});
}

TEST(a_mouse_record_that_is_not_one_is_refused) {
    const auto input = open_input();
    CHECK(apkrt_input_post_mouse(input.id, nullptr) == 0);
    const auto refused = [&](auto change) {
        apkrt_axes_event mouse = mouse_of(7, 0, 1, 1);
        change(mouse);
        return apkrt_input_post_mouse(input.id, &mouse) == 0;
    };
    CHECK(refused([](apkrt_axes_event &m) { m.action = 2; }));   // the guest makes MOVE, DOWN and UP itself
    CHECK(refused([](apkrt_axes_event &m) { m.action = 0; }));
    CHECK(refused([](apkrt_axes_event &m) { m.action = 9; }));
    CHECK(refused([](apkrt_axes_event &m) { m.action = 13; }));
    CHECK(refused([](apkrt_axes_event &m) { m.device_id = 0; }));
    CHECK(refused([](apkrt_axes_event &m) { m.source = 0x01000010; }));  // not a mouse
    CHECK(refused([](apkrt_axes_event &m) { m.tool_type = 6; }));
    CHECK(refused([](apkrt_axes_event &m) { m.axis_count = APKRT_AXES_MAX + 1; }));
    CHECK(refused([](apkrt_axes_event &m) { m.axes[0].value = std::numeric_limits<float>::infinity(); }));
    CHECK(refused([](apkrt_axes_event &m) { m.axes[1].axis = 0; }));  // named twice
    CHECK(refused([](apkrt_axes_event &m) { m.button_state = 0x1000; }));  // a button the runtime has no word for
    CHECK(mouse_trace(input.fd).empty());
    call(nr_close, {unsigned_of(input.fd)});
}

// A press makes three records at once: the channel takes all of them or none, and keeps the state it had.
TEST(a_full_channel_takes_a_mouse_press_whole_or_not_at_all) {
    const auto input = open_input();
    apkrt_axes_event hover = mouse_of(7, 0, 5, 6);
    CHECK(apkrt_input_post_mouse(input.id, &hover) == 1);
    const apkrt_key_event key = key_of(0, 29);
    for (std::size_t count = 1; count < most_events; ++count) CHECK(apkrt_input_post_key(input.id, &key) == 1);
    apkrt_axes_event filler = mouse_of(12, 0);  // releases, which take the slack of the touch ends
    filler.device_id = 1003;
    for (std::size_t count = 0; count < most_ends - 2; ++count) CHECK(apkrt_input_post_axes(input.id, &filler) == 1);
    apkrt_axes_event press = mouse_of(11, 1, 5, 6);
    CHECK(apkrt_input_post_mouse(input.id, &press) == 0);  // three would not fit in two
    CHECK(take(input.fd).event.kind == APKRT_INPUT_AXES);  // the hover enter
    for (std::size_t count = 1; count < most_events; ++count) CHECK(take(input.fd).event.kind == APKRT_INPUT_KEY);
    for (std::size_t count = 0; count < most_ends - 2; ++count) CHECK(take(input.fd).event.kind == APKRT_INPUT_AXES);
    CHECK(take(input.fd).result == -eagain);
    CHECK(apkrt_input_post_mouse(input.id, &press) == 1);  // the refused press had changed nothing
    CHECK(mouse_trace(input.fd) == "HX/1 DOWN/1 PRESS/1/1");
    call(nr_close, {unsigned_of(input.fd)});
}

TEST(records_of_all_kinds_arrive_in_the_order_they_were_posted) {
    const auto input = open_input();
    const apkrt_key_event key = key_of(0, 29);
    apkrt_text_event text{};
    text.op = APKRT_TEXT_FINISH_COMPOSING;
    CHECK(post(input.id, Motion{.action = down, .down_time = 1, .ids = {0}, .times = {1}}) == 1);
    CHECK(apkrt_input_post_key(input.id, &key) == 1);
    CHECK(apkrt_input_post_text(input.id, &text) == 1);
    CHECK(post(input.id, Motion{.action = up, .down_time = 1, .ids = {0}, .times = {2}}) == 1);
    CHECK(pipe_bytes(input.fd) == 4);
    CHECK(take(input.fd).event.kind == APKRT_INPUT_MOTION);
    CHECK(take(input.fd).event.kind == APKRT_INPUT_KEY);
    CHECK(take(input.fd).event.kind == APKRT_INPUT_TEXT);
    const auto last = take(input.fd);
    CHECK(last.event.kind == APKRT_INPUT_MOTION && last.event.action == up);
    CHECK(pipe_bytes(input.fd) == 0);
    call(nr_close, {unsigned_of(input.fd)});
}

// A key lies between two MOVEs of one gesture: a full channel does not merge them over it, and the gesture's MOVEs
// keep their samples.
TEST(a_full_channel_does_not_merge_moves_over_a_key) {
    const auto input = open_input();
    const apkrt_key_event key = key_of(0, 29);
    CHECK(post(input.id, Motion{.action = down, .down_time = 1, .ids = {0}, .times = {1}}) == 1);
    CHECK(post(input.id, Motion{.action = move, .down_time = 1, .ids = {0}, .times = {2}}) == 1);
    CHECK(apkrt_input_post_key(input.id, &key) == 1);
    for (std::int64_t time = 3; time < static_cast<std::int64_t>(most_events); ++time) {
        CHECK(apkrt_input_post_key(input.id, &key) == 1);
    }
    CHECK(post(input.id, Motion{.action = move, .down_time = 1, .ids = {0}, .times = {300}}) == 0);
    CHECK(take(input.fd).event.action == down);
    const auto moved = take(input.fd);
    CHECK(moved.event.kind == APKRT_INPUT_MOTION && moved.event.sample_count == 1 && moved.event.sample_times[0] == 2);
    call(nr_close, {unsigned_of(input.fd)});
}

// A channel full of keys refuses more at 256: the 17 beyond are the open gesture's ends, which keys do not take.
TEST(a_channel_full_of_keys_refuses_at_256_and_keeps_the_ends_of_a_gesture) {
    const auto input = open_input();
    const apkrt_key_event key = key_of(1, 30);
    CHECK(post(input.id, Motion{.action = down, .down_time = 1, .ids = {0}, .times = {1}}) == 1);
    for (std::size_t count = 1; count < most_events; ++count) CHECK(apkrt_input_post_key(input.id, &key) == 1);
    CHECK(apkrt_input_post_key(input.id, &key) == 0);
    CHECK(post(input.id, Motion{.action = up, .down_time = 1, .ids = {0}, .times = {9}}) == 1);
    CHECK(take(input.fd).event.action == down);
    CHECK(apkrt_input_post_key(input.id, &key) == 0);  // 256 again with the UP: still full
    call(nr_close, {unsigned_of(input.fd)});
}

TEST(a_closed_queue_refuses_every_kind) {
    const auto input = open_input();
    call(nr_close, {unsigned_of(input.fd)});
    const apkrt_key_event key = key_of(0, 29);
    apkrt_text_event text{};
    apkrt_device_event device{};
    CHECK(apkrt_input_post_key(input.id, &key) == 0);
    CHECK(apkrt_input_post_text(input.id, &text) == 0);
    CHECK(apkrt_input_post_device(input.id, &device) == 0);
    CHECK(apkrt_input_post_key(input.id + 100, &key) == 0);
}

}  // namespace

int main() {
    apkrt::descriptors::reset();
    apkrt::descriptors::adopt_standard_descriptors();
    return apk::testing::run_all();
}
