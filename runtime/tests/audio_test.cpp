// Checks the runtime's audio streams (ADR-0036, decisions 5 and 6): an open needs the host's audio side and sound
// values, and gives the hardware's rate unless one was asked for, with a burst of the host's IO buffer and a ring of
// APKRT_AUDIO_BURSTS bursts, of which the guest fills APKRT_AUDIO_FIRST_BURSTS at first; the guest's writes fill the
// buffer size and no more; the host reads the frames in order, also across the ring's end, and only while the stream
// plays; the first read after a start makes it STARTED; the commands follow AAudio's requests, a flush only while the
// stream does not play, a stop playing out what a started stream holds; a started stream that runs dry counts an
// xrun once it has played since its start; a read marks the timestamp; a disconnected stream refuses all but status
// and close; a closed stream's handle is gone for good, also for the host; at most 64 streams are open; reads
// against pauses, flushes and stops never repeat or lose a frame; a write waits for room until its deadline, and
// audio_wait for another sequence, woken by the host's reads, the guest's commands, a disconnect and the close, and
// ended by a signal; a stream thread joins the output's workgroup. The test's thread plays the guest and calls the
// dispatcher directly, as channels_test.cpp does, and plays the host through apkrt.h, with a fake host of 48 kHz and
// 5 ms IO buffers, whose output has 10 ms of latency.
#include "signals.h"
#include "syscall_frame.h"

#include "APKRuntime/apkrt.h"
#include "check.h"

#include <AudioToolbox/AudioToolbox.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <os/workgroup.h>
#include <pthread.h>
#include <time.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

// Linux: include/uapi/asm-generic/errno-base.h, errno.h.
constexpr std::int64_t eperm = 1, enoent = 2, eintr = 4, eio = 5, ebadf = 9, efault = 14, enodev = 19, einval = 22,
                       emfile = 24, epipe = 32, etimedout = 110;
constexpr std::int64_t millisecond = 1'000'000;  // ns
constexpr std::int32_t none = 10, power_saving = 11, low_latency = 12;  // AAudio's performance modes

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

// MARK: - The fake host

struct Opened {
    std::uint64_t stream;
    double sample_rate;
    std::int32_t channels;
    std::int32_t performance_mode;
};

struct FakeHost {
    std::mutex lock;
    std::vector<Opened> opened;
    std::vector<std::uint64_t> closed;
    std::map<std::uint64_t, std::int32_t> xruns_at_close;  // what apkrt_audio_xruns gave inside the close
    bool refuse = false;
    bool disconnect_in_open = false;  // a route change while the host opens
};
FakeHost fake;

int fake_open(void *, std::uint64_t stream, double sample_rate, std::int32_t channels, std::int32_t performance_mode,
              apkrt_audio_hardware *hardware) {
    const std::lock_guard hold(fake.lock);
    if (fake.refuse) return -1;
    fake.opened.push_back({stream, sample_rate, channels, performance_mode});
    *hardware = {48'000, 0.005};
    apkrt_audio_set_output(nullptr, 10'000'000);  // as the App's engine starts, from within the open
    if (fake.disconnect_in_open) apkrt_audio_disconnected(stream);
    return 0;
}

void fake_close(void *, std::uint64_t stream) {
    const std::lock_guard hold(fake.lock);
    fake.closed.push_back(stream);
    fake.xruns_at_close[stream] = apkrt_audio_xruns(stream);
}

/// A fresh fake host, set as the host's audio side.
void use_fake_host() {
    {
        const std::lock_guard hold(fake.lock);
        fake.opened.clear();
        fake.closed.clear();
        fake.xruns_at_close.clear();
        fake.refuse = false;
        fake.disconnect_in_open = false;
    }
    const apkrt_audio_host host{nullptr, fake_open, fake_close};
    CHECK(apkrt_set_audio_host(&host) == 0);
}

std::size_t closes_of(std::uint64_t stream) {
    const std::lock_guard hold(fake.lock);
    std::size_t count = 0;
    for (const auto closed : fake.closed) count += closed == stream;
    return count;
}

// MARK: - The guest's calls

/// audio_open's result, and the configuration it gave.
struct Open {
    std::int64_t result;
    apkrt_audio_config config;
};

Open open_stream(std::int32_t rate = 0, std::int32_t channels = 2, std::int32_t mode = low_latency) {
    Open open{0, {rate, channels, mode, -1, -1, -1, -1, 0}};
    open.result = call(APKRT_NR_AUDIO_OPEN, {address(&open.config)});
    return open;
}

std::uint64_t opened(std::int32_t rate = 0, std::int32_t channels = 2) {
    const Open open = open_stream(rate, channels);
    CHECK(open.result > 0);
    return unsigned_of(open.result);
}

std::int64_t close_stream(std::uint64_t stream) {
    return call(APKRT_NR_AUDIO_CLOSE, {stream});
}

std::int64_t control(std::uint64_t stream, apkrt_audio_command command, std::int64_t argument = 0) {
    return call(APKRT_NR_AUDIO_CONTROL, {stream, static_cast<std::uint64_t>(command), unsigned_of(argument)});
}

apkrt_audio_status status_of(std::uint64_t stream) {
    apkrt_audio_status status{};
    status.state = -1;
    CHECK(call(APKRT_NR_AUDIO_STATUS, {stream, address(&status)}) == 0);
    return status;
}

/// The guest's frames, stereo: frame n holds 2n and 2n + 1, exact in float below 2^23 frames.
struct Frames {
    std::int64_t next = 0;

    std::vector<float> take(std::int64_t count) {
        std::vector<float> samples;
        for (std::int64_t frame = 0; frame < count; ++frame, ++next) {
            samples.push_back(static_cast<float>(2 * next));
            samples.push_back(static_cast<float>(2 * next + 1));
        }
        return samples;
    }
};

/// The guest's CLOCK_MONOTONIC, the clock of the deadlines, in ns.
std::int64_t now() {
    return static_cast<std::int64_t>(clock_gettime_nsec_np(CLOCK_UPTIME_RAW));
}

/// audio_write of `count` new frames that waits until `deadline`; the frames not taken are given back.
std::int64_t write_until(std::uint64_t stream, Frames &frames, std::int64_t count, std::int64_t deadline) {
    auto samples = frames.take(count);
    const auto taken =
        call(APKRT_NR_AUDIO_WRITE, {stream, address(samples.data()), unsigned_of(count), unsigned_of(deadline)});
    frames.next -= count - (taken > 0 ? taken : 0);
    return taken;
}

/// audio_write of `count` new frames without a wait.
std::int64_t write(std::uint64_t stream, Frames &frames, std::int64_t count) {
    return write_until(stream, frames, count, 0);
}

std::int64_t wait_for_change(std::uint64_t stream, std::uint32_t sequence, std::int64_t deadline) {
    return call(APKRT_NR_AUDIO_WAIT, {stream, sequence, unsigned_of(deadline)});
}

/// The host's read of up to `frames` frames, and what it gave.
struct Read {
    std::int64_t count;
    std::vector<float> samples;
};

Read host_read(std::uint64_t stream, std::int64_t frames, std::uint64_t host_time = 0) {
    Read read{0, std::vector<float>(static_cast<std::size_t>(2 * frames), -1.0f)};
    read.count = apkrt_audio_read(stream, read.samples.data(), frames, host_time);
    return read;
}

/// Whether `read` gave the stereo frames from `first` on, `count` of them.
bool gave(const Read &read, std::int64_t first, std::int64_t count) {
    if (read.count != count) return false;
    for (std::int64_t frame = 0; frame < count; ++frame) {
        if (read.samples[static_cast<std::size_t>(2 * frame)] != static_cast<float>(2 * (first + frame))) return false;
        if (read.samples[static_cast<std::size_t>(2 * frame + 1)] != static_cast<float>(2 * (first + frame) + 1)) {
            return false;
        }
    }
    return true;
}

std::int64_t nanoseconds_of_host_time(std::uint64_t host_time) {
    mach_timebase_info_data_t base{};
    mach_timebase_info(&base);
    return static_cast<std::int64_t>(host_time * base.numer / base.denom);
}

// MARK: - Opening

TEST(without_an_audio_host_an_open_fails) {
    CHECK(apkrt_set_audio_host(nullptr) == 0);
    CHECK(open_stream().result == -enodev);
}

TEST(the_host_needs_both_functions) {
    const apkrt_audio_host no_open{nullptr, nullptr, fake_close};
    const apkrt_audio_host no_close{nullptr, fake_open, nullptr};
    CHECK(apkrt_set_audio_host(&no_open) == -1);
    CHECK(apkrt_set_audio_host(&no_close) == -1);
}

TEST(an_open_refuses_what_is_not_sound) {
    use_fake_host();
    CHECK(call(APKRT_NR_AUDIO_OPEN, {0}) == -efault);
    for (const std::int32_t channels : {0, APKRT_AUDIO_CHANNELS + 1, -1}) CHECK(open_stream(0, channels).result == -einval);
    for (const std::int32_t rate : {-1, 7'999, 1'600'001}) CHECK(open_stream(rate).result == -einval);
    for (const std::int32_t mode : {0, 9, 13}) CHECK(open_stream(0, 2, mode).result == -einval);
    const std::lock_guard hold(fake.lock);
    CHECK(fake.opened.empty());
}

TEST(an_open_gives_the_hardware_s_rate_and_bursts_of_its_io_buffer) {
    use_fake_host();
    const Open open = open_stream(0, 2, low_latency);
    CHECK(open.result > 0);
    CHECK(open.config.sample_rate == 48'000 && open.config.hardware_rate == 48'000);
    CHECK(open.config.frames_per_burst == 240);
    CHECK(open.config.capacity == APKRT_AUDIO_BURSTS * 240 && open.config.buffer_size == APKRT_AUDIO_FIRST_BURSTS * 240);
    {
        const std::lock_guard hold(fake.lock);
        CHECK(fake.opened.size() == 1);
        if (!fake.opened.empty()) {
            const auto &host = fake.opened.back();
            CHECK(host.stream == unsigned_of(open.result));
            CHECK(host.sample_rate == 0 && host.channels == 2 && host.performance_mode == low_latency);
        }
    }
    const auto status = status_of(unsigned_of(open.result));
    CHECK(status.state == APKRT_AUDIO_OPEN);
    CHECK(status.frames_written == 0 && status.frames_read == 0 && status.xruns == 0);
    CHECK(status.buffer_size == APKRT_AUDIO_FIRST_BURSTS * 240 && status.position == -1);
    CHECK(close_stream(unsigned_of(open.result)) == 0);
}

TEST(a_requested_rate_is_kept_with_its_burst) {
    use_fake_host();
    for (const std::int32_t mode : {none, power_saving}) {
        const Open open = open_stream(44'100, 1, mode);
        CHECK(open.result > 0);
        CHECK(open.config.sample_rate == 44'100 && open.config.hardware_rate == 48'000);
        CHECK(open.config.frames_per_burst == 221);  // 220.5 frames in 5 ms, rounded
        CHECK(open.config.capacity == APKRT_AUDIO_BURSTS * 221);
        CHECK(open.config.buffer_size == APKRT_AUDIO_FIRST_BURSTS * 221);
        {
            const std::lock_guard hold(fake.lock);
            CHECK(!fake.opened.empty() && fake.opened.back().sample_rate == 44'100 && fake.opened.back().channels == 1);
            CHECK(!fake.opened.empty() && fake.opened.back().performance_mode == mode);
        }
        CHECK(close_stream(unsigned_of(open.result)) == 0);
    }
}

TEST(sixty_four_streams_are_open_at_most_and_a_refused_open_takes_none) {
    use_fake_host();
    {
        const std::lock_guard hold(fake.lock);
        fake.refuse = true;
    }
    CHECK(open_stream().result == -eio);
    {
        const std::lock_guard hold(fake.lock);
        CHECK(fake.closed.empty());  // no close for an open the host refused
        fake.refuse = false;
    }
    std::vector<std::uint64_t> streams;
    for (int index = 0; index < 64; ++index) streams.push_back(opened());
    CHECK(open_stream().result == -emfile);
    for (const auto stream : streams) CHECK(close_stream(stream) == 0);
    for (const auto stream : streams) CHECK(closes_of(stream) == 1);
    const auto again = opened();
    CHECK(close_stream(again) == 0);
}

TEST(a_disconnect_while_the_host_opens_the_stream_holds) {
    use_fake_host();
    {
        const std::lock_guard hold(fake.lock);
        fake.disconnect_in_open = true;
    }
    const auto stream = opened();
    CHECK(status_of(stream).state == APKRT_AUDIO_DISCONNECTED);
    CHECK(control(stream, APKRT_AUDIO_START) == -epipe);
    CHECK(close_stream(stream) == 0);
    CHECK(closes_of(stream) == 1);
}

TEST(a_stream_closes_on_the_host_that_opened_it) {
    use_fake_host();
    const auto stream = opened();
    CHECK(apkrt_set_audio_host(nullptr) == 0);
    CHECK(close_stream(stream) == 0);
    CHECK(closes_of(stream) == 1);
}

// MARK: - Writing and reading

TEST(a_write_fills_the_buffer_size_and_no_more) {
    use_fake_host();
    const auto stream = opened();
    Frames frames;
    CHECK(write(stream, frames, 1'000) == 480);
    CHECK(write(stream, frames, 10) == 0);
    CHECK(status_of(stream).frames_written == 480);
    std::vector<float> samples(2);
    CHECK(call(APKRT_NR_AUDIO_WRITE, {stream, 0, 1, 0}) == -efault);
    CHECK(call(APKRT_NR_AUDIO_WRITE, {stream, address(samples.data()), unsigned_of(-1), 0}) == -einval);
    CHECK(call(APKRT_NR_AUDIO_WRITE, {stream, 0, 0, 0}) == 0);
    CHECK(close_stream(stream) == 0);
}

TEST(a_stream_that_does_not_play_reads_nothing) {
    use_fake_host();
    const auto stream = opened();
    Frames frames;
    CHECK(write(stream, frames, 480) == 480);
    CHECK(host_read(stream, 240).count == 0);
    const auto status = status_of(stream);
    CHECK(status.state == APKRT_AUDIO_OPEN && status.frames_read == 0 && status.xruns == 0);
    CHECK(close_stream(stream) == 0);
}

TEST(the_first_read_after_a_start_makes_it_started_and_the_frames_come_in_order) {
    use_fake_host();
    const auto stream = opened();
    Frames frames;
    CHECK(write(stream, frames, 480) == 480);
    CHECK(control(stream, APKRT_AUDIO_START) == 0);
    CHECK(status_of(stream).state == APKRT_AUDIO_STARTING);
    CHECK(gave(host_read(stream, 100), 0, 100));
    CHECK(status_of(stream).state == APKRT_AUDIO_STARTED);
    CHECK(gave(host_read(stream, 140), 100, 140));
    CHECK(write(stream, frames, 1'000) == 240);
    CHECK(gave(host_read(stream, 480), 240, 480));
    const auto status = status_of(stream);
    CHECK(status.frames_written == 720 && status.frames_read == 720 && status.xruns == 0);
    CHECK(host_read(stream, 0).count == 0);
    CHECK(apkrt_audio_read(stream, nullptr, 10, 0) == 0);
    CHECK(close_stream(stream) == 0);
}

TEST(a_start_reads_as_started_also_before_any_frame) {
    use_fake_host();
    const auto stream = opened();
    CHECK(control(stream, APKRT_AUDIO_START) == 0);
    CHECK(host_read(stream, 240).count == 0);
    const auto status = status_of(stream);
    CHECK(status.state == APKRT_AUDIO_STARTED && status.xruns == 0);
    CHECK(close_stream(stream) == 0);
}

TEST(a_started_stream_that_runs_dry_counts_an_xrun_once_it_has_played_since_its_start) {
    use_fake_host();
    const auto stream = opened();
    Frames frames;
    CHECK(control(stream, APKRT_AUDIO_START) == 0);
    CHECK(host_read(stream, 240).count == 0);  // nothing played yet: no xrun
    CHECK(write(stream, frames, 240) == 240);
    CHECK(gave(host_read(stream, 240), 0, 240));
    CHECK(status_of(stream).xruns == 0);
    CHECK(host_read(stream, 240).count == 0);
    CHECK(status_of(stream).xruns == 1 && apkrt_audio_xruns(stream) == 1);
    CHECK(write(stream, frames, 100) == 100);
    CHECK(gave(host_read(stream, 240), 240, 100));  // fewer than asked
    CHECK(status_of(stream).xruns == 2);
    CHECK(control(stream, APKRT_AUDIO_PAUSE) == 0);
    CHECK(host_read(stream, 240).count == 0);  // paused: no xrun
    CHECK(control(stream, APKRT_AUDIO_START) == 0);
    CHECK(host_read(stream, 240).count == 0);  // nothing played since the start: no xrun
    CHECK(status_of(stream).xruns == 2 && apkrt_audio_xruns(stream) == 2);
    CHECK(close_stream(stream) == 0);
}

TEST(the_ring_keeps_the_frames_whole_across_its_end) {
    use_fake_host();
    const auto stream = opened();
    Frames frames;
    CHECK(control(stream, APKRT_AUDIO_BUFFER_SIZE, APKRT_AUDIO_BURSTS * 240) == APKRT_AUDIO_BURSTS * 240);
    CHECK(control(stream, APKRT_AUDIO_START) == 0);
    std::int64_t read = 0;
    bool whole = true;
    while (read < 6 * APKRT_AUDIO_BURSTS * 240) {
        CHECK(write(stream, frames, 700) == 700);
        for (std::int64_t left = 700; left > 0;) {
            const auto chunk = left < 333 ? left : 333;
            whole = whole && gave(host_read(stream, chunk), read, chunk);
            read += chunk;
            left -= chunk;
        }
    }
    CHECK(whole);
    CHECK(status_of(stream).xruns == 0);
    CHECK(close_stream(stream) == 0);
}

TEST(the_buffer_size_is_at_least_a_burst_and_at_most_the_capacity) {
    use_fake_host();
    const auto stream = opened();
    Frames frames;
    CHECK(control(stream, APKRT_AUDIO_BUFFER_SIZE, 1) == 240);
    CHECK(control(stream, APKRT_AUDIO_BUFFER_SIZE, 100'000) == APKRT_AUDIO_BURSTS * 240);
    CHECK(control(stream, APKRT_AUDIO_BUFFER_SIZE, -5) == 240);
    CHECK(control(stream, APKRT_AUDIO_BUFFER_SIZE, 500) == 500);
    CHECK(status_of(stream).buffer_size == 500);
    CHECK(write(stream, frames, 1'000) == 500);
    CHECK(close_stream(stream) == 0);
}

// MARK: - The commands

TEST(a_pause_stops_the_reads_and_keeps_the_frames) {
    use_fake_host();
    const auto stream = opened();
    Frames frames;
    CHECK(write(stream, frames, 480) == 480);
    CHECK(control(stream, APKRT_AUDIO_START) == 0);
    CHECK(gave(host_read(stream, 240), 0, 240));
    CHECK(control(stream, APKRT_AUDIO_PAUSE) == 0);
    CHECK(status_of(stream).state == APKRT_AUDIO_PAUSED);
    CHECK(host_read(stream, 240).count == 0);
    CHECK(control(stream, APKRT_AUDIO_PAUSE) == 0);
    CHECK(control(stream, APKRT_AUDIO_START) == 0);
    CHECK(gave(host_read(stream, 240), 240, 240));
    CHECK(status_of(stream).xruns == 0);
    CHECK(close_stream(stream) == 0);
}

TEST(a_flush_works_only_while_the_stream_does_not_play_and_drops_the_unread_frames) {
    use_fake_host();
    const auto stream = opened();
    Frames frames;
    CHECK(write(stream, frames, 480) == 480);
    CHECK(control(stream, APKRT_AUDIO_START) == 0);
    CHECK(control(stream, APKRT_AUDIO_FLUSH) == -eperm);  // STARTING
    CHECK(gave(host_read(stream, 100), 0, 100));
    CHECK(control(stream, APKRT_AUDIO_FLUSH) == -eperm);  // STARTED
    CHECK(control(stream, APKRT_AUDIO_PAUSE) == 0);
    CHECK(control(stream, APKRT_AUDIO_FLUSH) == 0);
    auto status = status_of(stream);
    CHECK(status.state == APKRT_AUDIO_FLUSHED && status.frames_read == 480 && status.frames_written == 480);
    CHECK(control(stream, APKRT_AUDIO_FLUSH) == 0);
    CHECK(write(stream, frames, 480) == 480);
    CHECK(control(stream, APKRT_AUDIO_START) == 0);
    CHECK(gave(host_read(stream, 240), 480, 240));  // the new frames, none of the flushed
    CHECK(control(stream, APKRT_AUDIO_STOP) == 0);
    CHECK(control(stream, APKRT_AUDIO_FLUSH) == 0);  // STOPPED: the rest goes
    status = status_of(stream);
    CHECK(status.state == APKRT_AUDIO_FLUSHED && status.frames_read == 960);
    CHECK(host_read(stream, 240).count == 0);
    CHECK(close_stream(stream) == 0);

    const auto fresh = opened();
    CHECK(write(fresh, frames, 100) == 100);
    CHECK(control(fresh, APKRT_AUDIO_FLUSH) == 0);  // OPEN
    CHECK(status_of(fresh).frames_read == 100);
    CHECK(close_stream(fresh) == 0);
}

TEST(a_stop_lets_a_started_stream_play_out) {
    use_fake_host();
    const auto stream = opened();
    Frames frames;
    CHECK(write(stream, frames, 480) == 480);
    CHECK(control(stream, APKRT_AUDIO_START) == 0);
    CHECK(gave(host_read(stream, 240), 0, 240));
    CHECK(control(stream, APKRT_AUDIO_STOP) == 0);
    CHECK(status_of(stream).state == APKRT_AUDIO_STOPPED);
    CHECK(gave(host_read(stream, 300), 240, 240));
    CHECK(host_read(stream, 240).count == 0);
    CHECK(status_of(stream).xruns == 0);
    CHECK(write(stream, frames, 240) == 240);
    CHECK(host_read(stream, 240).count == 0);  // played out: stopped for good
    CHECK(control(stream, APKRT_AUDIO_STOP) == 0);
    CHECK(control(stream, APKRT_AUDIO_START) == 0);
    CHECK(gave(host_read(stream, 240), 480, 240));
    CHECK(close_stream(stream) == 0);
}

TEST(a_stop_plays_out_only_what_the_stream_held) {
    use_fake_host();
    const auto stream = opened();
    Frames frames;
    CHECK(write(stream, frames, 480) == 480);
    CHECK(control(stream, APKRT_AUDIO_START) == 0);
    CHECK(gave(host_read(stream, 240), 0, 240));
    CHECK(control(stream, APKRT_AUDIO_STOP) == 0);
    CHECK(write(stream, frames, 240) == 240);        // after the stop: it waits for the next start
    CHECK(gave(host_read(stream, 300), 240, 240));   // the frames held at the stop, and no more
    CHECK(host_read(stream, 240).count == 0);
    CHECK(write(stream, frames, 1'000) == 240);      // the room the play out left, and no more while stopped
    CHECK(host_read(stream, 240).count == 0);
    CHECK(control(stream, APKRT_AUDIO_START) == 0);
    CHECK(gave(host_read(stream, 240), 480, 240));
    CHECK(close_stream(stream) == 0);
}

TEST(a_stopped_stream_that_still_plays_out_starts_and_pauses_again) {
    use_fake_host();
    const auto stream = opened();
    Frames frames;
    CHECK(write(stream, frames, 480) == 480);
    CHECK(control(stream, APKRT_AUDIO_START) == 0);
    CHECK(gave(host_read(stream, 100), 0, 100));
    CHECK(control(stream, APKRT_AUDIO_STOP) == 0);  // it plays out the other 380 frames
    CHECK(control(stream, APKRT_AUDIO_START) == 0);
    CHECK(status_of(stream).state == APKRT_AUDIO_STARTING);
    CHECK(gave(host_read(stream, 100), 100, 100));
    CHECK(control(stream, APKRT_AUDIO_STOP) == 0);
    CHECK(control(stream, APKRT_AUDIO_PAUSE) == 0);
    CHECK(status_of(stream).state == APKRT_AUDIO_PAUSED);
    CHECK(host_read(stream, 100).count == 0);  // the pause ends the playing out, and the frames stay
    CHECK(control(stream, APKRT_AUDIO_START) == 0);
    CHECK(gave(host_read(stream, 280), 200, 280));
    CHECK(close_stream(stream) == 0);
}

TEST(a_stop_keeps_the_frames_of_a_stream_that_did_not_play) {
    use_fake_host();
    const auto stream = opened();
    Frames frames;
    CHECK(write(stream, frames, 480) == 480);
    CHECK(control(stream, APKRT_AUDIO_STOP) == 0);
    CHECK(host_read(stream, 240).count == 0);
    CHECK(control(stream, APKRT_AUDIO_START) == 0);
    CHECK(gave(host_read(stream, 240), 0, 240));
    CHECK(control(stream, APKRT_AUDIO_PAUSE) == 0);
    CHECK(control(stream, APKRT_AUDIO_STOP) == 0);
    CHECK(host_read(stream, 240).count == 0);
    CHECK(close_stream(stream) == 0);
}

TEST(the_commands_answer_as_aaudio_s_requests) {
    use_fake_host();
    const auto stream = opened();
    const auto state = [&] { return status_of(stream).state; };
    CHECK(control(stream, APKRT_AUDIO_PAUSE) == 0 && state() == APKRT_AUDIO_PAUSED);  // from OPEN
    CHECK(control(stream, APKRT_AUDIO_STOP) == 0 && state() == APKRT_AUDIO_STOPPED);
    CHECK(control(stream, APKRT_AUDIO_PAUSE) == 0 && state() == APKRT_AUDIO_PAUSED);  // from STOPPED
    CHECK(control(stream, APKRT_AUDIO_FLUSH) == 0 && state() == APKRT_AUDIO_FLUSHED);
    CHECK(control(stream, APKRT_AUDIO_PAUSE) == 0 && state() == APKRT_AUDIO_PAUSED);  // from FLUSHED
    CHECK(control(stream, APKRT_AUDIO_START) == 0 && state() == APKRT_AUDIO_STARTING);
    CHECK(control(stream, APKRT_AUDIO_START) == 0 && state() == APKRT_AUDIO_STARTING);
    CHECK(host_read(stream, 240).count == 0 && state() == APKRT_AUDIO_STARTED);
    CHECK(control(stream, APKRT_AUDIO_START) == 0 && state() == APKRT_AUDIO_STARTED);
    CHECK(control(stream, APKRT_AUDIO_STOP) == 0 && state() == APKRT_AUDIO_STOPPED);
    CHECK(control(stream, APKRT_AUDIO_FLUSH) == 0 && state() == APKRT_AUDIO_FLUSHED);
    CHECK(control(stream, APKRT_AUDIO_STOP) == 0 && state() == APKRT_AUDIO_STOPPED);  // from FLUSHED
    CHECK(control(stream, APKRT_AUDIO_START) == 0 && state() == APKRT_AUDIO_STARTING);
    CHECK(control(stream, APKRT_AUDIO_STOP) == 0 && state() == APKRT_AUDIO_STOPPED);  // from STARTING
    for (const std::uint64_t command : {std::uint64_t{0}, std::uint64_t{6}}) {
        CHECK(call(APKRT_NR_AUDIO_CONTROL, {stream, command, 0}) == -einval);
    }
    CHECK(close_stream(stream) == 0);
}

TEST(the_sequence_changes_at_reads_that_take_frames_and_at_changes_of_the_state) {
    use_fake_host();
    const auto stream = opened();
    Frames frames;
    const auto sequence = [&] { return status_of(stream).sequence; };
    auto seen = sequence();
    CHECK(write(stream, frames, 480) == 480);
    CHECK(sequence() == seen);  // a write changes nothing the guest waits for
    CHECK(control(stream, APKRT_AUDIO_START) == 0);
    CHECK(sequence() != seen);
    seen = sequence();
    CHECK(host_read(stream, 240).count == 240);
    CHECK(sequence() != seen);
    CHECK(control(stream, APKRT_AUDIO_PAUSE) == 0);
    seen = sequence();
    CHECK(host_read(stream, 240).count == 0);
    CHECK(sequence() == seen);
    CHECK(control(stream, APKRT_AUDIO_PAUSE) == 0);
    CHECK(sequence() == seen);  // no change of the state
    CHECK(close_stream(stream) == 0);
}

TEST(a_read_marks_the_timestamp_of_its_first_frame) {
    use_fake_host();
    const auto stream = opened();
    Frames frames;
    CHECK(write(stream, frames, 480) == 480);
    CHECK(control(stream, APKRT_AUDIO_START) == 0);
    const std::uint64_t first = mach_absolute_time();
    CHECK(host_read(stream, 240, first).count == 240);
    auto status = status_of(stream);
    CHECK(status.position == 0 && status.time == nanoseconds_of_host_time(first) + 10'000'000);
    const std::uint64_t second = first + 120'000;
    CHECK(host_read(stream, 240, second).count == 240);
    status = status_of(stream);
    CHECK(status.position == 240 && status.time == nanoseconds_of_host_time(second) + 10'000'000);
    CHECK(host_read(stream, 240, second + 120'000).count == 0);  // took nothing: the timestamp stays
    CHECK(status_of(stream).position == 240);
    apkrt_audio_set_output(nullptr, 30'000'000);  // a new route
    CHECK(write(stream, frames, 240) == 240);
    const std::uint64_t third = second + 240'000;
    CHECK(host_read(stream, 240, third).count == 240);
    status = status_of(stream);
    CHECK(status.position == 480 && status.time == nanoseconds_of_host_time(third) + 30'000'000);
    CHECK(close_stream(stream) == 0);
}

// MARK: - Disconnect and close

TEST(a_disconnected_stream_refuses_all_but_status_and_close) {
    use_fake_host();
    const auto stream = opened();
    Frames frames;
    CHECK(write(stream, frames, 240) == 240);
    CHECK(control(stream, APKRT_AUDIO_START) == 0);
    const auto seen = status_of(stream).sequence;
    apkrt_audio_disconnected(stream);
    const auto status = status_of(stream);
    CHECK(status.state == APKRT_AUDIO_DISCONNECTED && status.sequence != seen);
    CHECK(write(stream, frames, 10) == -epipe);
    for (const auto command : {APKRT_AUDIO_START, APKRT_AUDIO_PAUSE, APKRT_AUDIO_FLUSH, APKRT_AUDIO_STOP}) {
        CHECK(control(stream, command) == -epipe);
    }
    CHECK(host_read(stream, 240).count == 0);
    CHECK(close_stream(stream) == 0);
    CHECK(closes_of(stream) == 1);
    apkrt_audio_disconnected(stream);  // closed: nothing happens
    apkrt_audio_disconnected(0);
}

TEST(a_closed_stream_is_gone_for_good_also_for_the_host) {
    use_fake_host();
    const auto stream = opened();
    Frames frames;
    CHECK(write(stream, frames, 480) == 480);
    CHECK(control(stream, APKRT_AUDIO_START) == 0);
    CHECK(host_read(stream, 240).count == 240);
    CHECK(close_stream(stream) == 0);
    CHECK(closes_of(stream) == 1);
    apkrt_audio_status status{};
    CHECK(call(APKRT_NR_AUDIO_STATUS, {stream, address(&status)}) == -ebadf);
    CHECK(call(APKRT_NR_AUDIO_STATUS, {stream, 0}) == -ebadf);
    CHECK(write(stream, frames, 10) == -ebadf);
    CHECK(control(stream, APKRT_AUDIO_START) == -ebadf);
    CHECK(call(APKRT_NR_AUDIO_WAIT, {stream, 0, 0}) == -ebadf);
    CHECK(close_stream(stream) == -ebadf);
    CHECK(host_read(stream, 240).count == 0 && apkrt_audio_xruns(stream) == 0);
    const auto next = opened();
    CHECK(next != stream);
    CHECK(host_read(stream, 240).count == 0);
    CHECK(close_stream(next) == 0);
    for (const std::uint64_t unknown : {std::uint64_t{0}, std::uint64_t{1} << 40, ~std::uint64_t{0}}) {
        CHECK(close_stream(unknown) == -ebadf);
        CHECK(host_read(unknown, 240).count == 0);
    }
    CHECK(closes_of(stream) == 1);
}

TEST(a_status_needs_somewhere_to_go) {
    use_fake_host();
    const auto stream = opened();
    CHECK(call(APKRT_NR_AUDIO_STATUS, {stream, 0}) == -efault);
    CHECK(close_stream(stream) == 0);
}

TEST(the_host_s_close_still_gets_the_stream_s_xruns) {
    use_fake_host();
    const auto stream = opened();
    Frames frames;
    CHECK(control(stream, APKRT_AUDIO_START) == 0);
    CHECK(write(stream, frames, 240) == 240);
    CHECK(host_read(stream, 240).count == 240);
    CHECK(host_read(stream, 240).count == 0);
    CHECK(close_stream(stream) == 0);
    const std::lock_guard hold(fake.lock);
    CHECK(fake.xruns_at_close.count(stream) == 1 && fake.xruns_at_close[stream] == 1);
}

// MARK: - Waiting

TEST(a_write_waits_for_room_until_its_deadline_and_gives_what_it_took) {
    use_fake_host();
    const auto stream = opened();
    Frames frames;
    auto start = now();
    CHECK(write_until(stream, frames, 1'000, start + 30 * millisecond) == 480);  // nothing plays: no room comes
    CHECK(now() - start >= 30 * millisecond);
    start = now();
    CHECK(write_until(stream, frames, 10, start + 20 * millisecond) == 0);
    CHECK(now() - start >= 20 * millisecond);
    for (const std::int64_t passed : {std::int64_t{1}, std::int64_t{-1}, now() - millisecond}) {
        CHECK(write_until(stream, frames, 10, passed) == 0);  // passed, -1 too: no wait
    }
    CHECK(close_stream(stream) == 0);
}

TEST(a_blocking_write_takes_the_rest_as_the_host_reads) {
    use_fake_host();
    const auto stream = opened();
    CHECK(control(stream, APKRT_AUDIO_START) == 0);
    std::atomic<bool> done{false};
    std::atomic<std::int64_t> read{0};
    std::atomic<bool> in_order{true};
    std::thread render([&] {
        std::vector<float> samples(2 * 120);
        std::int64_t next = 0;
        while (!done.load()) {
            std::this_thread::sleep_for(2ms);
            const auto count = apkrt_audio_read(stream, samples.data(), 120, mach_absolute_time());
            for (std::int64_t frame = 0; frame < count; ++frame, ++next) {
                if (samples[static_cast<std::size_t>(2 * frame)] != static_cast<float>(2 * next)) in_order.store(false);
            }
            read.fetch_add(count);
        }
    });
    Frames frames;
    CHECK(write_until(stream, frames, 2'400, now() + 5'000 * millisecond) == 2'400);
    done.store(true);
    render.join();
    CHECK(in_order.load());
    CHECK(read.load() >= 2'400 - 480);  // all but what the buffer may still hold
    CHECK(close_stream(stream) == 0);
}

TEST(a_bigger_buffer_size_wakes_a_waiting_write) {
    use_fake_host();
    const auto stream = opened();
    Frames frames;
    CHECK(write(stream, frames, 480) == 480);  // the buffer is full
    std::atomic<std::int64_t> written{-1};
    std::thread writer([&] {
        Frames more{480};
        written.store(write_until(stream, more, 240, now() + 5'000 * millisecond));
    });
    std::this_thread::sleep_for(20ms);
    const auto start = now();
    CHECK(control(stream, APKRT_AUDIO_BUFFER_SIZE, 720) == 720);
    writer.join();
    CHECK(written.load() == 240);
    CHECK(now() - start < 1'000 * millisecond);
    CHECK(close_stream(stream) == 0);
}

TEST(a_wait_ends_when_the_sequence_is_another_or_at_its_deadline) {
    use_fake_host();
    const auto stream = opened();
    const auto seen = status_of(stream).sequence;
    CHECK(wait_for_change(stream, seen - 1, 0) == 0);  // another already
    const auto start = now();
    CHECK(wait_for_change(stream, seen, start + 20 * millisecond) == -etimedout);
    CHECK(now() - start >= 20 * millisecond);
    CHECK(wait_for_change(stream, seen, 0) == -etimedout);
    CHECK(wait_for_change(stream, seen, -1) == -etimedout);
    std::thread starter([&] {
        std::this_thread::sleep_for(20ms);
        control(stream, APKRT_AUDIO_START);
    });
    CHECK(wait_for_change(stream, seen, now() + 5'000 * millisecond) == 0);
    starter.join();
    CHECK(status_of(stream).state == APKRT_AUDIO_STARTING);
    CHECK(close_stream(stream) == 0);
}

TEST(a_read_that_takes_frames_wakes_a_waiting_guest) {
    use_fake_host();
    const auto stream = opened();
    Frames frames;
    CHECK(write(stream, frames, 480) == 480);
    CHECK(control(stream, APKRT_AUDIO_START) == 0);
    CHECK(host_read(stream, 240).count == 240);
    const auto seen = status_of(stream).sequence;
    std::atomic<std::int64_t> result{1};
    std::thread waiter([&] { result.store(wait_for_change(stream, seen, now() + 5'000 * millisecond)); });
    std::this_thread::sleep_for(20ms);  // the waiter sleeps by then
    CHECK(result.load() == 1);
    const auto start = now();
    CHECK(host_read(stream, 240).count == 240);
    waiter.join();
    CHECK(result.load() == 0);
    CHECK(now() - start < 1'000 * millisecond);  // the read woke it, not its deadline
    CHECK(close_stream(stream) == 0);
}

TEST(a_close_wakes_its_waiting_guests_with_ebadf) {
    use_fake_host();
    const auto stream = opened();
    Frames frames;
    CHECK(write(stream, frames, 480) == 480);  // the buffer is full
    const auto seen = status_of(stream).sequence;
    std::atomic<std::int64_t> written{1}, waited{1};
    std::thread writer([&] {
        Frames more{480};
        written.store(write_until(stream, more, 10, now() + 5'000 * millisecond));
    });
    std::thread waiter([&] { waited.store(wait_for_change(stream, seen, now() + 5'000 * millisecond)); });
    std::this_thread::sleep_for(20ms);
    const auto start = now();
    CHECK(close_stream(stream) == 0);
    writer.join();
    waiter.join();
    CHECK(written.load() == -ebadf && waited.load() == -ebadf);
    CHECK(now() - start < 1'000 * millisecond);
    CHECK(closes_of(stream) == 1);
}

TEST(a_disconnect_ends_a_waiting_write_with_what_it_took) {
    use_fake_host();
    const auto full = opened();
    Frames frames;
    CHECK(write(full, frames, 480) == 480);
    std::atomic<std::int64_t> without_room{1}, with_room{1};
    std::thread writer([&] {
        Frames more{480};
        without_room.store(write_until(full, more, 10, now() + 5'000 * millisecond));
    });
    std::this_thread::sleep_for(20ms);
    auto start = now();
    apkrt_audio_disconnected(full);
    writer.join();
    CHECK(without_room.load() == -epipe);
    CHECK(now() - start < 1'000 * millisecond);
    CHECK(close_stream(full) == 0);

    const auto empty = opened();
    std::thread partial([&] {
        Frames more;
        with_room.store(write_until(empty, more, 1'000, now() + 5'000 * millisecond));
    });
    std::this_thread::sleep_for(20ms);
    start = now();
    apkrt_audio_disconnected(empty);
    partial.join();
    CHECK(with_room.load() == 480);
    CHECK(now() - start < 1'000 * millisecond);
    CHECK(close_stream(empty) == 0);
}

TEST(a_signal_ends_a_wait_with_eintr_unless_frames_were_taken) {
    use_fake_host();
    const auto stream = opened();
    auto &state = apkrt::signals::current();
    constexpr int sigusr1 = 10;
    state.pending.fetch_or(apkrt::signals::bit(sigusr1));
    Frames frames;
    const auto seen = status_of(stream).sequence;
    CHECK(write_until(stream, frames, 1'000, now() + 5'000 * millisecond) == 480);
    CHECK(write_until(stream, frames, 10, now() + 5'000 * millisecond) == -eintr);
    CHECK(wait_for_change(stream, seen, now() + 5'000 * millisecond) == -eintr);
    CHECK(wait_for_change(stream, seen - 1, now() + 5'000 * millisecond) == 0);
    state.mask.fetch_or(apkrt::signals::bit(sigusr1));  // a blocked signal does not end it
    CHECK(wait_for_change(stream, seen, now() + 20 * millisecond) == -etimedout);
    state.mask.fetch_and(~apkrt::signals::bit(sigusr1));
    state.pending.fetch_and(~apkrt::signals::bit(sigusr1));
    CHECK(close_stream(stream) == 0);
}

TEST(more_guests_wait_than_a_stream_has_places_for_and_all_wake) {
    use_fake_host();
    const auto stream = opened();
    const auto seen = status_of(stream).sequence;
    std::atomic<int> woken{0};
    std::vector<std::thread> waiters;
    for (int index = 0; index < 40; ++index) {
        waiters.emplace_back([&] {
            if (wait_for_change(stream, seen, now() + 5'000 * millisecond) == 0) woken.fetch_add(1);
        });
    }
    std::this_thread::sleep_for(50ms);
    const auto start = now();
    CHECK(control(stream, APKRT_AUDIO_START) == 0);
    for (auto &waiter : waiters) waiter.join();
    CHECK(woken.load() == 40);
    CHECK(now() - start < 1'000 * millisecond);
    CHECK(close_stream(stream) == 0);
}

// MARK: - The host against the guest

TEST(reads_against_pauses_flushes_and_stops_never_repeat_or_lose_a_frame) {
    use_fake_host();
    const auto stream = opened();
    CHECK(control(stream, APKRT_AUDIO_BUFFER_SIZE, APKRT_AUDIO_BURSTS * 240) == APKRT_AUDIO_BURSTS * 240);
    std::atomic<bool> done{false};
    std::atomic<std::int64_t> reads{0};
    std::atomic<bool> in_order{true};
    std::vector<std::int64_t> jumps;  // the render's: the frames that did not follow the one before
    std::thread render([&] {
        std::int64_t last = -1;
        std::vector<float> samples(2 * 97);
        while (!done.load()) {
            const auto count = apkrt_audio_read(stream, samples.data(), 97, mach_absolute_time());
            for (std::int64_t frame = 0; frame < count; ++frame) {
                const float left = samples[static_cast<std::size_t>(2 * frame)];
                const auto index = static_cast<std::int64_t>(left) / 2;
                if (index <= last || samples[static_cast<std::size_t>(2 * frame + 1)] != left + 1) in_order.store(false);
                if (index != last + 1) jumps.push_back(index);
                last = index;
            }
            reads.fetch_add(1);
        }
    });
    Frames frames;
    bool flushed_all = true;
    std::vector<std::int64_t> flushed_to;  // where each flush left the stream: the next frame it plays
    for (int round = 0; round < 3'000; ++round) {
        write(stream, frames, 300);
        switch (round % 7) {
            case 0: control(stream, APKRT_AUDIO_START); break;
            case 3:
                control(stream, APKRT_AUDIO_PAUSE);
                if (control(stream, APKRT_AUDIO_FLUSH) == 0) {
                    const auto status = status_of(stream);
                    flushed_all = flushed_all && status.frames_read == status.frames_written;
                    flushed_to.push_back(status.frames_read);
                }
                break;
            case 5: control(stream, APKRT_AUDIO_STOP); break;
            default: break;
        }
        if (round % 50 == 0) std::this_thread::yield();
    }
    done.store(true);
    render.join();
    CHECK(in_order.load());
    CHECK(flushed_all);
    bool only_at_flushes = true;  // a frame is lost only where a flush dropped it
    for (const auto jump : jumps) {
        only_at_flushes = only_at_flushes && std::find(flushed_to.begin(), flushed_to.end(), jump) != flushed_to.end();
    }
    CHECK(only_at_flushes);
    CHECK(reads.load() > 0);
    const auto status = status_of(stream);
    CHECK(status.frames_read <= status.frames_written);
    CHECK(close_stream(stream) == 0);
}

TEST(streams_close_and_open_again_while_the_host_reads_and_guests_wait) {
    use_fake_host();
    std::atomic<std::uint64_t> current{0};
    std::atomic<bool> done{false};
    std::thread render([&] {  // reads whatever stream is current, also one that closes under it
        std::vector<float> samples(2 * 64);
        while (!done.load()) apkrt_audio_read(current.load(), samples.data(), 64, mach_absolute_time());
    });
    bool answers = true;
    for (int round = 0; round < 300; ++round) {
        const auto stream = opened();
        current.store(stream);
        Frames frames;
        write(stream, frames, 480);
        control(stream, APKRT_AUDIO_START);
        std::atomic<std::int64_t> waited{1}, written{1};
        std::thread waiter([&] {
            apkrt_audio_status seen{};
            call(APKRT_NR_AUDIO_STATUS, {stream, address(&seen)});  // closed already: the wait answers -EBADF
            waited.store(wait_for_change(stream, seen.sequence, now() + 5'000 * millisecond));
        });
        std::thread writer([&] {
            Frames more{480};
            written.store(write_until(stream, more, 2'000, now() + 5'000 * millisecond));
        });
        if (round % 2 == 0) std::this_thread::yield();
        CHECK(close_stream(stream) == 0);
        waiter.join();
        writer.join();
        answers = answers && (waited.load() == 0 || waited.load() == -ebadf) &&
                  (written.load() >= 0 || written.load() == -ebadf);
    }
    done.store(true);
    render.join();
    CHECK(answers);
    const std::lock_guard hold(fake.lock);
    CHECK(fake.closed.size() == 300 && fake.opened.size() == 300);
}

// MARK: - The workgroup

TEST(a_join_without_the_output_s_workgroup_answers_enoent) {
    use_fake_host();
    const auto stream = opened();
    apkrt_audio_set_output(nullptr, 10'000'000);
    CHECK(call(APKRT_NR_AUDIO_JOIN, {stream, 1}) == -enoent);
    CHECK(call(APKRT_NR_AUDIO_JOIN, {1u << 20, 1}) == -ebadf);
    CHECK(close_stream(stream) == 0);
}

TEST(a_stream_thread_joins_the_workgroup_of_the_default_output) {
    AudioComponentDescription description{kAudioUnitType_Output, kAudioUnitSubType_DefaultOutput,
                                          kAudioUnitManufacturer_Apple, 0, 0};
    AudioComponent component = AudioComponentFindNext(nullptr, &description);
    AudioUnit unit = nullptr;
    if (component == nullptr || AudioComponentInstanceNew(component, &unit) != noErr || AudioUnitInitialize(unit) != noErr) {
        std::printf("note: no default output unit, the join is not checked\n");
        if (unit != nullptr) AudioComponentInstanceDispose(unit);
        return;
    }
    use_fake_host();
    const auto stream = opened();
    apkrt_audio_set_output(unit, 10'000'000);
    os_workgroup_t group = nullptr;  // the test's own reference, to see where the thread is
    UInt32 size = sizeof group;
    if (AudioUnitGetProperty(unit, kAudioOutputUnitProperty_OSWorkgroup, kAudioUnitScope_Global, 0, &group, &size) !=
        noErr) {
        group = nullptr;
    }
    std::int64_t joined = 0, again = 0, left = 0, elsewhere = 0;
    bool held = false, free_again = false, real_time = false;
    mach_timebase_info_data_t base{};
    mach_timebase_info(&base);
    const double burst_ticks = 0.005 * 1e9 * base.denom / base.numer;  // 240 frames at 48 kHz
    std::thread thread([&] {
        joined = call(APKRT_NR_AUDIO_JOIN, {stream, 1});
        thread_time_constraint_policy_data_t policy{};
        mach_msg_type_number_t count = THREAD_TIME_CONSTRAINT_POLICY_COUNT;
        boolean_t get_default = FALSE;
        real_time = thread_policy_get(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY,
                                      reinterpret_cast<thread_policy_t>(&policy), &count, &get_default) == KERN_SUCCESS &&
                    !get_default && std::abs(static_cast<double>(policy.period) - burst_ticks) <= 1;
        os_workgroup_join_token_s token{};
        held = group != nullptr && os_workgroup_join(group, &token) == EALREADY;  // the runtime's join holds it
        again = call(APKRT_NR_AUDIO_JOIN, {stream, 1});
        left = call(APKRT_NR_AUDIO_JOIN, {stream, 0});
        free_again = group != nullptr && os_workgroup_join(group, &token) == 0;
        if (free_again) os_workgroup_leave(group, &token);
    });
    thread.join();
    std::thread other([&] { elsewhere = call(APKRT_NR_AUDIO_JOIN, {stream, 1}); });  // ends joined: it leaves
    other.join();
    if (joined == -enoent || group == nullptr) {
        std::printf("note: the default output has no workgroup, the join is not checked\n");
    } else {
        CHECK(joined == 0 && held && again == 0 && left == 0 && free_again && elsewhere == 0);
        CHECK(real_time);  // with a time constraint of the burst, as the output's own IO thread
    }
    if (group != nullptr) os_release(group);
    apkrt_audio_set_output(nullptr, 10'000'000);
    CHECK(close_stream(stream) == 0);
    AudioUnitUninitialize(unit);
    AudioComponentInstanceDispose(unit);
}

}  // namespace

int main() {
    return apk::testing::run_all();
}
