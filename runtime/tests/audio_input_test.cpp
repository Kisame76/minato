// Checks the runtime's audio input streams (ADR-0064): an input open needs the host's input side and the stored
// microphone grant, refuses a direction other than 0 and 1, and fills the hardware's values as the output's open does;
// the host's captures reach the ring only while the stream is started, in order across the ring's end; audio_read
// takes them, waits until its deadline for more, and gives what a stopped stream still holds; a full ring drops its
// oldest frames and counts an xrun; a flush clears the ring; the status counts captured and read frames; an output
// stream refuses audio_read and an input stream audio_write; a revoked grant disconnects the stream; the close ends
// the waits and the host's side. The test's thread plays the guest through the dispatcher and the host through apkrt.h.
#include "syscall_frame.h"

#include "APKRuntime/apkrt.h"
#include "check.h"

#include <time.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <mutex>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

constexpr std::int64_t eperm = 1, ebadf = 9, eacces = 13, efault = 14, enodev = 19, einval = 22, epipe = 32;
constexpr std::int64_t millisecond = 1'000'000;  // ns
constexpr std::int32_t low_latency = 12;

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

std::int64_t now() {
    return static_cast<std::int64_t>(clock_gettime_nsec_np(CLOCK_UPTIME_RAW));
}

// MARK: - The fake host

struct FakeInput {
    std::mutex lock;
    std::vector<std::uint64_t> opened, closed;
    std::int32_t channels = 0;
    double rate = 0;
    bool refuse = false;
};
FakeInput fake;

int fake_open(void *, std::uint64_t stream, double sample_rate, std::int32_t channels, std::int32_t,
              apkrt_audio_hardware *hardware) {
    const std::lock_guard hold(fake.lock);
    if (fake.refuse) return -1;
    fake.opened.push_back(stream);
    fake.channels = channels;
    fake.rate = sample_rate;
    *hardware = {48'000, 0.005};
    return 0;
}

void fake_close(void *, std::uint64_t stream) {
    const std::lock_guard hold(fake.lock);
    fake.closed.push_back(stream);
}

void use_fake_input(bool granted = true) {
    {
        const std::lock_guard hold(fake.lock);
        fake.opened.clear();
        fake.closed.clear();
        fake.refuse = false;
    }
    const apkrt_audio_input_host host{nullptr, fake_open, fake_close};
    CHECK(apkrt_set_audio_input_host(&host) == 0);
    apkrt_permission_answer(0, APKRT_PERMISSION_MICROPHONE,
                            granted ? APKRT_PERMISSION_GRANTED : APKRT_PERMISSION_DENIED);
}

std::size_t closes_of(std::uint64_t stream) {
    const std::lock_guard hold(fake.lock);
    std::size_t count = 0;
    for (const auto closed : fake.closed) count += closed == stream;
    return count;
}

// MARK: - The guest's calls

struct Open {
    std::int64_t result;
    apkrt_audio_config config;
};

Open open_stream(std::int32_t direction = 1, std::int32_t channels = 1, std::int32_t rate = 0) {
    Open open{0, {rate, channels, low_latency, -1, -1, -1, -1, direction, 0}};
    open.result = call(APKRT_NR_AUDIO_OPEN, {address(&open.config)});
    return open;
}

std::uint64_t opened(std::int32_t channels = 1) {
    const Open open = open_stream(1, channels);
    CHECK(open.result > 0);
    return unsigned_of(open.result);
}

std::int64_t control(std::uint64_t stream, apkrt_audio_command command) {
    return call(APKRT_NR_AUDIO_CONTROL, {stream, static_cast<std::uint64_t>(command), 0});
}

apkrt_audio_status status_of(std::uint64_t stream) {
    apkrt_audio_status status{};
    status.state = -1;
    CHECK(call(APKRT_NR_AUDIO_STATUS, {stream, address(&status)}) == 0);
    return status;
}

/// Mono frames: frame n holds n, exact in float below 2^23.
std::vector<float> ramp(std::int64_t first, std::int64_t count) {
    std::vector<float> samples;
    for (std::int64_t frame = 0; frame < count; ++frame) samples.push_back(static_cast<float>(first + frame));
    return samples;
}

std::int64_t capture(std::uint64_t stream, std::int64_t first, std::int64_t count) {
    const auto samples = ramp(first, count);
    return apkrt_audio_input_write(stream, samples.data(), count, 0);
}

struct Taken {
    std::int64_t count;
    std::vector<float> samples;
};

Taken read_until(std::uint64_t stream, std::int64_t frames, std::int64_t deadline = 0) {
    Taken taken{0, std::vector<float>(static_cast<std::size_t>(frames), -1.0f)};
    taken.count = call(APKRT_NR_AUDIO_READ,
                       {stream, address(taken.samples.data()), unsigned_of(frames), unsigned_of(deadline)});
    return taken;
}

bool is_ramp(const Taken &taken, std::int64_t first, std::int64_t count) {
    if (taken.count != count) return false;
    for (std::int64_t frame = 0; frame < count; ++frame) {
        if (taken.samples[static_cast<std::size_t>(frame)] != static_cast<float>(first + frame)) return false;
    }
    return true;
}

// MARK: - Opening

TEST(without_an_input_host_an_input_open_answers_enodev) {
    CHECK(apkrt_set_audio_input_host(nullptr) == 0);
    apkrt_permission_answer(0, APKRT_PERMISSION_MICROPHONE, APKRT_PERMISSION_GRANTED);
    CHECK(open_stream().result == -enodev);
}

TEST(the_input_host_needs_both_functions) {
    const apkrt_audio_input_host no_open{nullptr, nullptr, fake_close};
    const apkrt_audio_input_host no_close{nullptr, fake_open, nullptr};
    CHECK(apkrt_set_audio_input_host(&no_open) == -1);
    CHECK(apkrt_set_audio_input_host(&no_close) == -1);
}

TEST(an_input_open_needs_the_microphone_grant) {
    use_fake_input(false);
    CHECK(open_stream().result == -eacces);
    apkrt_permission_answer(0, APKRT_PERMISSION_MICROPHONE, APKRT_PERMISSION_GRANTED);
    CHECK(open_stream().result > 0);
    const std::lock_guard hold(fake.lock);
    CHECK(fake.opened.size() == 1);
}

TEST(a_direction_other_than_zero_and_one_is_refused) {
    use_fake_input();
    CHECK(open_stream(2).result == -einval);
    CHECK(open_stream(-1).result == -einval);
    const std::lock_guard hold(fake.lock);
    CHECK(fake.opened.empty());
}

TEST(a_host_that_cannot_capture_gives_eio) {
    use_fake_input();
    fake.refuse = true;
    CHECK(open_stream().result == -5);
}

TEST(an_input_open_gives_the_hardware_s_values_and_the_asked_shape_to_the_host) {
    use_fake_input();
    const Open open = open_stream(1, 2, 16'000);
    CHECK(open.result > 0);
    CHECK(open.config.sample_rate == 16'000 && open.config.hardware_rate == 48'000);
    CHECK(open.config.frames_per_burst == 80);  // 5 ms at 16 kHz
    CHECK(open.config.capacity == APKRT_AUDIO_INPUT_BURSTS * 80 && open.config.buffer_size == open.config.capacity);
    {
        const std::lock_guard hold(fake.lock);
        CHECK(fake.channels == 2 && fake.rate == 16'000);
    }
    CHECK(status_of(unsigned_of(open.result)).state == APKRT_AUDIO_OPEN);
    CHECK(call(APKRT_NR_AUDIO_CLOSE, {unsigned_of(open.result)}) == 0);
}

TEST(an_input_ring_holds_the_frames_the_guest_asked_for_rounded_up_to_a_burst) {
    use_fake_input();
    apkrt_audio_config config{16'000, 1, low_latency, -1, -1, 16'000 * 2, -1, 1, 0};  // two seconds
    const auto result = call(APKRT_NR_AUDIO_OPEN, {address(&config)});
    CHECK(result > 0);
    CHECK(config.capacity == 32'000 && config.buffer_size == 32'000);  // 400 bursts of 80
    CHECK(call(APKRT_NR_AUDIO_CLOSE, {unsigned_of(result)}) == 0);
    config = {16'000, 1, low_latency, -1, -1, 1'000'000, -1, 1, 0};  // more than ten seconds: ten
    const auto big = call(APKRT_NR_AUDIO_OPEN, {address(&config)});
    CHECK(big > 0 && config.capacity == 160'000);
    CHECK(call(APKRT_NR_AUDIO_CLOSE, {unsigned_of(big)}) == 0);
}

// MARK: - Capturing and reading

TEST(captures_reach_the_ring_only_while_the_stream_plays_and_come_out_in_order) {
    use_fake_input();
    const auto stream = opened();
    CHECK(capture(stream, 0, 100) == 0);  // not started: nothing is kept
    CHECK(control(stream, APKRT_AUDIO_START) == 0);
    CHECK(status_of(stream).state == APKRT_AUDIO_STARTING);
    CHECK(capture(stream, 0, 100) == 100);
    CHECK(status_of(stream).state == APKRT_AUDIO_STARTED);  // the first capture after the start
    CHECK(capture(stream, 100, 60) == 60);
    CHECK(is_ramp(read_until(stream, 80), 0, 80));
    CHECK(is_ramp(read_until(stream, 100), 80, 80));  // what there is, without a wait
    CHECK(read_until(stream, 10).count == 0);
    const auto status = status_of(stream);
    CHECK(status.frames_written == 160 && status.frames_read == 160 && status.xruns == 0);
    CHECK(call(APKRT_NR_AUDIO_CLOSE, {stream}) == 0);
}

TEST(a_read_waits_until_its_deadline_for_the_rest) {
    use_fake_input();
    const auto stream = opened();
    CHECK(control(stream, APKRT_AUDIO_START) == 0);
    CHECK(capture(stream, 0, 10) == 10);
    std::thread host([&] {
        std::this_thread::sleep_for(50ms);
        capture(stream, 10, 30);
    });
    const std::int64_t before = now();
    const auto taken = read_until(stream, 40, before + 2'000 * millisecond);
    CHECK(is_ramp(taken, 0, 40));
    CHECK(now() - before >= 40 * millisecond);
    host.join();
    const std::int64_t start = now();
    const auto none = read_until(stream, 5, start + 30 * millisecond);  // nothing comes: the deadline ends it
    CHECK(none.count == 0);
    CHECK(now() - start >= 25 * millisecond);
    CHECK(call(APKRT_NR_AUDIO_CLOSE, {stream}) == 0);
}

TEST(a_full_ring_drops_its_oldest_frames_and_counts_an_xrun) {
    use_fake_input();
    const Open open = open_stream(1, 1);
    const auto stream = unsigned_of(open.result);
    const std::int64_t capacity = open.config.capacity;
    CHECK(control(stream, APKRT_AUDIO_START) == 0);
    CHECK(capture(stream, 0, capacity) == capacity);
    CHECK(status_of(stream).xruns == 0);
    CHECK(capture(stream, capacity, 100) == 100);  // 100 frames over: the 100 oldest go
    CHECK(status_of(stream).xruns == 1);
    CHECK(is_ramp(read_until(stream, capacity), 100, capacity));
    CHECK(call(APKRT_NR_AUDIO_CLOSE, {stream}) == 0);
}

TEST(a_flush_clears_the_ring_and_a_stop_keeps_what_was_captured) {
    use_fake_input();
    const auto stream = opened();
    CHECK(control(stream, APKRT_AUDIO_START) == 0);
    CHECK(capture(stream, 0, 50) == 50);
    CHECK(control(stream, APKRT_AUDIO_FLUSH) == -eperm);  // a flush only while the stream does not play
    CHECK(control(stream, APKRT_AUDIO_PAUSE) == 0);
    CHECK(capture(stream, 50, 10) == 0);  // paused: nothing is kept
    CHECK(control(stream, APKRT_AUDIO_FLUSH) == 0);
    CHECK(read_until(stream, 10).count == 0);
    CHECK(status_of(stream).state == APKRT_AUDIO_FLUSHED);
    CHECK(control(stream, APKRT_AUDIO_START) == 0);
    CHECK(capture(stream, 100, 20) == 20);
    CHECK(control(stream, APKRT_AUDIO_STOP) == 0);
    CHECK(capture(stream, 120, 20) == 0);
    const std::int64_t before = now();
    CHECK(is_ramp(read_until(stream, 50, before + 2'000 * millisecond), 100, 20));  // no wait on a stopped stream
    CHECK(now() - before < 500 * millisecond);
    CHECK(call(APKRT_NR_AUDIO_CLOSE, {stream}) == 0);
}

TEST(a_read_on_an_output_stream_and_a_write_on_an_input_stream_are_refused) {
    use_fake_input();
    const apkrt_audio_host output{nullptr,
                                  [](void *, std::uint64_t, double, std::int32_t, std::int32_t,
                                     apkrt_audio_hardware *hardware) {
                                      *hardware = {48'000, 0.005};
                                      return 0;
                                  },
                                  [](void *, std::uint64_t) {}};
    CHECK(apkrt_set_audio_host(&output) == 0);
    const Open out = open_stream(0, 2);
    CHECK(out.result > 0);
    float buffer[4] = {};
    CHECK(call(APKRT_NR_AUDIO_READ, {unsigned_of(out.result), address(buffer), 2, 0}) == -eperm);
    const auto input = opened();
    CHECK(call(APKRT_NR_AUDIO_WRITE, {input, address(buffer), 2, 0}) == -eperm);
    CHECK(call(APKRT_NR_AUDIO_CLOSE, {input}) == 0);
    CHECK(call(APKRT_NR_AUDIO_CLOSE, {unsigned_of(out.result)}) == 0);
    CHECK(apkrt_set_audio_host(nullptr) == 0);
}

TEST(read_refuses_bad_arguments) {
    use_fake_input();
    const auto stream = opened();
    float buffer[4] = {};
    CHECK(call(APKRT_NR_AUDIO_READ, {stream, 0, 4, 0}) == -efault);
    CHECK(call(APKRT_NR_AUDIO_READ, {stream, address(buffer), unsigned_of(-1), 0}) == -einval);
    CHECK(call(APKRT_NR_AUDIO_READ, {stream, 0, 0, 0}) == 0);
    CHECK(call(APKRT_NR_AUDIO_READ, {1u << 20, address(buffer), 4, 0}) == -ebadf);
    CHECK(call(APKRT_NR_AUDIO_CLOSE, {stream}) == 0);
    CHECK(call(APKRT_NR_AUDIO_READ, {stream, address(buffer), 4, 0}) == -ebadf);
}

// MARK: - Revocation and the close

TEST(a_revoked_grant_disconnects_the_stream_and_ends_a_wait) {
    use_fake_input();
    const auto stream = opened();
    CHECK(control(stream, APKRT_AUDIO_START) == 0);
    CHECK(capture(stream, 0, 20) == 20);
    std::thread host([&] {
        std::this_thread::sleep_for(50ms);
        apkrt_permission_answer(0, APKRT_PERMISSION_MICROPHONE, APKRT_PERMISSION_DENIED);
    });
    const auto first = read_until(stream, 20);
    CHECK(is_ramp(first, 0, 20));
    const std::int64_t before = now();
    CHECK(read_until(stream, 10, before + 5'000 * millisecond).count == -epipe);  // woken by the revoke
    CHECK(now() - before < 2'000 * millisecond);
    host.join();
    CHECK(closes_of(stream) == 1);  // the host's input ends with the grant, before the guest closes
    CHECK(status_of(stream).state == APKRT_AUDIO_DISCONNECTED);
    CHECK(capture(stream, 20, 10) == 0);
    CHECK(control(stream, APKRT_AUDIO_START) == -epipe);
    CHECK(call(APKRT_NR_AUDIO_CLOSE, {stream}) == 0);
    CHECK(closes_of(stream) == 1);
}

TEST(the_close_ends_a_wait_and_the_host_closes_once) {
    use_fake_input();
    const auto stream = opened();
    CHECK(control(stream, APKRT_AUDIO_START) == 0);
    std::thread guest_closes([&] {
        std::this_thread::sleep_for(50ms);
        CHECK(call(APKRT_NR_AUDIO_CLOSE, {stream}) == 0);
    });
    CHECK(read_until(stream, 10, now() + 5'000 * millisecond).count == -ebadf);
    guest_closes.join();
    CHECK(closes_of(stream) == 1);
    CHECK(capture(stream, 0, 10) == 0);  // harmless for a closed stream
}

}  // namespace

int main() {
    return apk::testing::run_all();
}
