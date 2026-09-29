// Checks libaaudio (runtime/android/aaudio.cpp, ADR-0036, decision 6) on the Mac, where it is compiled with
// tests/aaudio_host (shim.h, android/log.h) and its syscalls go to the runtime's dispatcher, and a fake host of 48 kHz
// with 5 ms IO buffers takes its streams; the test reads them as the render block would. It covers what aaudio-test
// on Android cannot show: the conversions of I16, I24 and I32, a data callback of more frames than the buffer size,
// the buffer of a callback that stops, callback frames against a flush, a write that a signal ends after its
// deadline, the error callback at a disconnect and from within the open, waitForStateChange's timeout, and a stream
// after its release.
#include "signals.h"
#include "syscall_frame.h"

#include "APKRuntime/apkrt.h"
#include "check.h"

#include <aaudio/AAudio.h>
#include <mach/mach_time.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

// MARK: - What the guest build gets from bionic

extern "C" int minato_host_syscall(int number, ...) {
    va_list arguments;
    va_start(arguments, number);
    apkrt_syscall_frame frame{};
    for (int index = 0; index < 4; ++index) frame.x[index] = va_arg(arguments, std::uint64_t);
    va_end(arguments);
    frame.x[8] = static_cast<std::uint64_t>(number);
    apkrt_syscall_dispatch(&frame);
    const auto result = static_cast<std::int64_t>(frame.x[0]);
    if (result >= -4095 && result < 0) {
        // bionic's syscall(): -1 and errno; Linux's numbers where Darwin's differ
        const auto linux_error = static_cast<int>(-result);
        errno = linux_error == 110 ? ETIMEDOUT : linux_error == 11 ? EAGAIN : linux_error;
        return -1;
    }
    return static_cast<int>(result);
}

extern "C" int __android_log_print(int, const char *tag, const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    std::fprintf(stderr, "log %s: ", tag);
    const int written = std::vfprintf(stderr, format, arguments);
    std::fputc('\n', stderr);
    va_end(arguments);
    return written;
}

namespace {

using namespace std::chrono_literals;

// MARK: - The fake host

struct FakeHost {
    std::mutex lock;
    std::vector<std::uint64_t> opened;
    bool disconnect_in_open = false;
};
FakeHost fake;

int fake_open(void *, std::uint64_t stream, double, std::int32_t, std::int32_t, apkrt_audio_hardware *hardware) {
    const std::lock_guard hold(fake.lock);
    fake.opened.push_back(stream);
    *hardware = {48'000, 0.005};
    apkrt_audio_set_output(nullptr, 10'000'000);
    if (fake.disconnect_in_open) apkrt_audio_disconnected(stream);
    return 0;
}

void fake_close(void *, std::uint64_t) {}

void use_fake_host() {
    {
        const std::lock_guard hold(fake.lock);
        fake.opened.clear();
        fake.disconnect_in_open = false;
    }
    const apkrt_audio_host host{nullptr, fake_open, fake_close};
    CHECK(apkrt_set_audio_host(&host) == 0);
}

/// The runtime's handle of the stream the host opened last.
std::uint64_t last_handle() {
    const std::lock_guard hold(fake.lock);
    return fake.opened.empty() ? 0 : fake.opened.back();
}

// MARK: - Streams

struct Options {
    aaudio_format_t format = AAUDIO_FORMAT_PCM_FLOAT;
    std::int32_t channels = 1;
    std::int32_t frames_per_callback = 0;
    AAudioStream_dataCallback data = nullptr;
    AAudioStream_errorCallback error = nullptr;
    void *user = nullptr;
};

AAudioStream *open_stream(const Options &options) {
    AAudioStreamBuilder *builder = nullptr;
    CHECK(AAudio_createStreamBuilder(&builder) == AAUDIO_OK);
    AAudioStreamBuilder_setFormat(builder, options.format);
    AAudioStreamBuilder_setChannelCount(builder, options.channels);
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    if (options.frames_per_callback > 0) AAudioStreamBuilder_setFramesPerDataCallback(builder, options.frames_per_callback);
    if (options.data != nullptr) AAudioStreamBuilder_setDataCallback(builder, options.data, options.user);
    if (options.error != nullptr) AAudioStreamBuilder_setErrorCallback(builder, options.error, options.user);
    AAudioStream *stream = nullptr;
    CHECK(AAudioStreamBuilder_openStream(builder, &stream) == AAUDIO_OK);
    AAudioStreamBuilder_delete(builder);
    return stream;
}

/// The render block's read of `frames` frames of `channels` channels.
std::vector<float> render(std::uint64_t handle, std::int64_t frames, std::int32_t channels = 1) {
    std::vector<float> samples(static_cast<std::size_t>(frames * channels));
    const auto read = apkrt_audio_read(handle, samples.data(), frames, mach_absolute_time());
    samples.resize(static_cast<std::size_t>(read * channels));
    return samples;
}

template <typename Done>
bool within(std::chrono::milliseconds limit, Done done) {
    const auto end = std::chrono::steady_clock::now() + limit;
    while (!done()) {
        if (std::chrono::steady_clock::now() >= end) return false;
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

/// Whether the ring holds as many frames as the buffer size lets it, within a second.
bool fills_up(AAudioStream *stream) {
    return within(1'000ms, [&] {
        return AAudioStream_getFramesWritten(stream) - AAudioStream_getFramesRead(stream) ==
               AAudioStream_getBufferSizeInFrames(stream);
    });
}

/// A data callback whose frames all hold the number of its call, float mono; it returns what `results` says for
/// the call, CONTINUE after them.
struct Numbered {
    std::atomic<int> calls{0};
    std::atomic<int> errors{0};
    std::atomic<aaudio_result_t> error{AAUDIO_OK};
    std::vector<aaudio_data_callback_result_t> results;
    AAudioStream *seen = nullptr;          // the app's variable, as the error callback finds it
    AAudioStream *const *opened = nullptr;
    aaudio_result_t close_from_callback = AAUDIO_OK;
};

aaudio_data_callback_result_t numbered(AAudioStream *, void *user, void *data, int32_t frames) {
    auto &numbers = *static_cast<Numbered *>(user);
    const int call = numbers.calls.fetch_add(1) + 1;
    auto *samples = static_cast<float *>(data);
    for (int32_t frame = 0; frame < frames; ++frame) samples[frame] = static_cast<float>(call);
    const auto index = static_cast<std::size_t>(call - 1);
    return index < numbers.results.size() ? numbers.results[index] : AAUDIO_CALLBACK_RESULT_CONTINUE;
}

void count_errors(AAudioStream *stream, void *user, aaudio_result_t error) {
    auto &numbers = *static_cast<Numbered *>(user);
    numbers.error.store(error);
    if (numbers.opened != nullptr) {
        numbers.seen = *numbers.opened;
        numbers.close_from_callback = AAudioStream_close(stream);
    }
    numbers.errors.fetch_add(1);
}

// MARK: - Conversions

template <typename Sample>
void check_conversion(aaudio_format_t format, const std::vector<Sample> &samples, const std::vector<float> &expected) {
    use_fake_host();
    AAudioStream *stream = open_stream({.format = format, .channels = 2});
    const auto frames = static_cast<std::int32_t>(expected.size() / 2);
    CHECK(AAudioStream_write(stream, samples.data(), frames, 0) == frames);
    CHECK(AAudioStream_requestStart(stream) == AAUDIO_OK);
    CHECK(render(last_handle(), frames, 2) == expected);
    CHECK(AAudioStream_close(stream) == AAUDIO_OK);
}

TEST(i16_frames_become_floats_scaled_by_2_to_the_minus_15) {
    check_conversion<std::int16_t>(AAUDIO_FORMAT_PCM_I16, {0, 16384, -32768, 32767, -1, 1, -16384, 8192},
                                   {0.0f, 0.5f, -1.0f, 32767.0f / 32768, -1.0f / 32768, 1.0f / 32768, -0.5f, 0.25f});
}

TEST(i24_packed_frames_become_floats_scaled_by_2_to_the_minus_23) {
    // little-endian: 0, 0x400000, 0x800000, 0x7fffff, 0xffffff, 1, 0xc00000, 0x200000
    const std::vector<std::uint8_t> bytes = {0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x80, 0xff, 0xff, 0x7f,
                                             0xff, 0xff, 0xff, 0x01, 0x00, 0x00, 0x00, 0x00, 0xc0, 0x00, 0x00, 0x20};
    check_conversion<std::uint8_t>(AAUDIO_FORMAT_PCM_I24_PACKED, bytes,
                                   {0.0f, 0.5f, -1.0f, 8388607.0f / 8388608, -1.0f / 8388608, 1.0f / 8388608, -0.5f,
                                    0.25f});
}

TEST(i32_frames_become_floats_scaled_by_2_to_the_minus_31) {
    check_conversion<std::int32_t>(AAUDIO_FORMAT_PCM_I32,
                                   {0, 0x40000000, INT32_MIN, INT32_MAX, -1, 1, -0x40000000, 0x20000000},
                                   {0.0f, 0.5f, -1.0f, 1.0f, -1.0f / 2147483648.0f, 1.0f / 2147483648.0f, -0.5f, 0.25f});
}

TEST(float_frames_stay_as_they_are) {
    check_conversion<float>(AAUDIO_FORMAT_PCM_FLOAT, {0.0f, 0.5f, -1.0f, 1.0f, -0.25f, 0.25f, 0.125f, -0.125f},
                            {0.0f, 0.5f, -1.0f, 1.0f, -0.25f, 0.25f, 0.125f, -0.125f});
}

// MARK: - The data callback

TEST(a_callback_of_more_frames_than_the_buffer_size_keeps_the_buffer_full) {
    use_fake_host();
    Numbered numbers;
    AAudioStream *stream = open_stream({.frames_per_callback = 512, .data = numbered, .user = &numbers});
    CHECK(AAudioStream_getBufferSizeInFrames(stream) == 480);
    CHECK(AAudioStream_requestStart(stream) == AAUDIO_OK);
    bool full = true;
    for (int period = 0; period < 12 && full; ++period) {
        full = fills_up(stream);
        render(last_handle(), 240);
    }
    CHECK(full);
    CHECK(AAudioStream_getXRunCount(stream) == 0);
    CHECK(AAudioStream_close(stream) == AAUDIO_OK);
}

TEST(the_frames_of_a_callback_that_stops_the_stream_are_not_played) {
    for (const aaudio_data_callback_result_t stop : {aaudio_data_callback_result_t{AAUDIO_CALLBACK_RESULT_STOP}, 42}) {
        use_fake_host();
        Numbered numbers;
        numbers.results = {AAUDIO_CALLBACK_RESULT_CONTINUE, AAUDIO_CALLBACK_RESULT_CONTINUE, stop};
        AAudioStream *stream = open_stream({.data = numbered, .user = &numbers});
        CHECK(AAudioStream_requestStart(stream) == AAUDIO_OK);
        bool third = false;
        CHECK(within(1'000ms, [&] {
            const auto samples = render(last_handle(), 240);
            for (const float sample : samples) third = third || sample == 3.0f;
            return AAudioStream_getState(stream) == AAUDIO_STREAM_STATE_STOPPED && samples.empty();
        }));
        CHECK(!third);
        std::this_thread::sleep_for(20ms);
        CHECK(numbers.calls.load() == 3);  // never called again
        CHECK(AAudioStream_close(stream) == AAUDIO_OK);
    }
}

TEST(a_flush_drops_the_callback_frames_that_were_not_in_the_buffer_yet) {
    use_fake_host();
    Numbered numbers;
    AAudioStream *stream = open_stream({.frames_per_callback = 512, .data = numbered, .user = &numbers});
    CHECK(AAudioStream_requestStart(stream) == AAUDIO_OK);
    for (int period = 0; period < 5; ++period) {
        CHECK(fills_up(stream));
        render(last_handle(), 240);
    }
    CHECK(fills_up(stream));
    CHECK(AAudioStream_requestPause(stream) == AAUDIO_OK);
    CHECK(AAudioStream_requestFlush(stream) == AAUDIO_OK);
    const int calls_before = numbers.calls.load();
    CHECK(AAudioStream_requestStart(stream) == AAUDIO_OK);
    CHECK(fills_up(stream));
    bool fresh = true;
    for (const float sample : render(last_handle(), 480)) fresh = fresh && sample > static_cast<float>(calls_before);
    CHECK(fresh);
    CHECK(AAudioStream_close(stream) == AAUDIO_OK);
}

// MARK: - Writes, states and errors

TEST(a_write_that_a_signal_keeps_waking_until_its_deadline_gives_what_it_wrote) {
    use_fake_host();
    AAudioStream *stream = open_stream({});
    std::vector<float> samples(480);
    CHECK(AAudioStream_write(stream, samples.data(), 480, 0) == 480);  // the buffer is full
    auto &state = apkrt::signals::current();
    constexpr int sigusr1 = 10;
    state.pending.fetch_or(apkrt::signals::bit(sigusr1));  // a handler without SA_RESTART: EINTR, again and again
    CHECK(AAudioStream_write(stream, samples.data(), 10, 20'000'000) == 0);
    state.pending.fetch_and(~apkrt::signals::bit(sigusr1));
    CHECK(AAudioStream_close(stream) == AAUDIO_OK);
}

TEST(a_disconnect_calls_the_error_callback_once_and_ends_the_data_callbacks) {
    use_fake_host();
    Numbered numbers;
    AAudioStream *stream = open_stream({.data = numbered, .error = count_errors, .user = &numbers});
    CHECK(AAudioStream_requestStart(stream) == AAUDIO_OK);
    CHECK(within(1'000ms, [&] { return numbers.calls.load() > 0; }));
    apkrt_audio_disconnected(last_handle());
    CHECK(within(1'000ms, [&] { return numbers.errors.load() == 1; }));
    CHECK(numbers.error.load() == AAUDIO_ERROR_DISCONNECTED);
    const int calls = numbers.calls.load();
    std::this_thread::sleep_for(20ms);
    CHECK(numbers.calls.load() == calls);
    CHECK(numbers.errors.load() == 1);
    CHECK(AAudioStream_getState(stream) == AAUDIO_STREAM_STATE_DISCONNECTED);
    CHECK(AAudioStream_requestStart(stream) == AAUDIO_ERROR_DISCONNECTED);
    CHECK(AAudioStream_close(stream) == AAUDIO_OK);
}

TEST(an_error_callback_comes_after_the_open_and_cannot_close_its_stream) {
    use_fake_host();
    {
        const std::lock_guard hold(fake.lock);
        fake.disconnect_in_open = true;
    }
    Numbered numbers;
    AAudioStream *stream = nullptr;
    numbers.opened = &stream;
    AAudioStreamBuilder *builder = nullptr;
    CHECK(AAudio_createStreamBuilder(&builder) == AAUDIO_OK);
    AAudioStreamBuilder_setErrorCallback(builder, count_errors, &numbers);
    CHECK(AAudioStreamBuilder_openStream(builder, &stream) == AAUDIO_OK);
    AAudioStreamBuilder_delete(builder);
    CHECK(within(1'000ms, [&] { return numbers.errors.load() == 1; }));
    CHECK(numbers.seen == stream);
    CHECK(numbers.close_from_callback == AAUDIO_ERROR_INVALID_STATE);
    CHECK(AAudioStream_close(stream) == AAUDIO_OK);
}

TEST(a_timestamp_comes_once_a_started_stream_has_played_a_frame) {
    use_fake_host();
    AAudioStream *stream = open_stream({});
    std::int64_t position = -1, time = -1;
    CHECK(AAudioStream_getTimestamp(stream, CLOCK_MONOTONIC, &position, &time) == AAUDIO_ERROR_INVALID_STATE);
    CHECK(AAudioStream_requestStart(stream) == AAUDIO_OK);
    CHECK(AAudioStream_getTimestamp(stream, CLOCK_MONOTONIC, &position, &time) == AAUDIO_ERROR_INVALID_STATE);
    CHECK(render(last_handle(), 100).empty());  // STARTED, but nothing heard yet
    CHECK(AAudioStream_getState(stream) == AAUDIO_STREAM_STATE_STARTED);
    CHECK(AAudioStream_getTimestamp(stream, CLOCK_MONOTONIC, &position, &time) == AAUDIO_ERROR_INVALID_STATE);
    std::vector<float> samples(240);
    CHECK(AAudioStream_write(stream, samples.data(), 240, 0) == 240);
    CHECK(render(last_handle(), 100).size() == 100);  // its first frame heard
    CHECK(AAudioStream_getTimestamp(stream, CLOCK_MONOTONIC, &position, &time) == AAUDIO_OK);
    CHECK(position == 0 && time > 0);
    CHECK(AAudioStream_close(stream) == AAUDIO_OK);
}

TEST(wait_for_state_change_times_out_on_a_state_that_holds) {
    use_fake_host();
    AAudioStream *stream = open_stream({});
    aaudio_stream_state_t next = AAUDIO_STREAM_STATE_UNINITIALIZED;
    const auto start = std::chrono::steady_clock::now();
    CHECK(AAudioStream_waitForStateChange(stream, AAUDIO_STREAM_STATE_OPEN, &next, 20'000'000) ==
          AAUDIO_ERROR_TIMEOUT);
    CHECK(next == AAUDIO_STREAM_STATE_OPEN);
    CHECK(std::chrono::steady_clock::now() - start >= 20ms);
    CHECK(AAudioStream_waitForStateChange(stream, AAUDIO_STREAM_STATE_STARTED, &next, 0) == AAUDIO_OK);
    CHECK(next == AAUDIO_STREAM_STATE_OPEN);
    CHECK(AAudioStream_close(stream) == AAUDIO_OK);
}

TEST(a_released_stream_keeps_its_values_and_refuses_the_rest) {
    use_fake_host();
    AAudioStream *stream = open_stream({.channels = 2});
    std::vector<float> samples(2 * 100);
    CHECK(AAudioStream_write(stream, samples.data(), 100, 0) == 100);
    CHECK(AAudioStream_release(stream) == AAUDIO_OK);
    CHECK(AAudioStream_getState(stream) == AAUDIO_STREAM_STATE_CLOSING);
    CHECK(AAudioStream_getFramesWritten(stream) == 100 && AAudioStream_getChannelCount(stream) == 2);
    CHECK(AAudioStream_requestStart(stream) == AAUDIO_ERROR_INVALID_STATE);
    CHECK(AAudioStream_write(stream, samples.data(), 10, 0) == AAUDIO_ERROR_INVALID_STATE);
    CHECK(AAudioStream_close(stream) == AAUDIO_OK);
}

TEST(an_open_refuses_what_aaudio_refuses_and_input) {
    use_fake_host();
    const auto open_with = [](auto set) {
        AAudioStreamBuilder *builder = nullptr;
        AAudio_createStreamBuilder(&builder);
        set(builder);
        AAudioStream *stream = nullptr;
        const aaudio_result_t result = AAudioStreamBuilder_openStream(builder, &stream);
        AAudioStreamBuilder_delete(builder);
        if (stream != nullptr) AAudioStream_close(stream);
        return result;
    };
    CHECK(open_with([](AAudioStreamBuilder *b) { AAudioStreamBuilder_setSampleRate(b, 7'999); }) ==
          AAUDIO_ERROR_INVALID_RATE);
    CHECK(open_with([](AAudioStreamBuilder *b) { AAudioStreamBuilder_setFormat(b, 99); }) == AAUDIO_ERROR_INVALID_FORMAT);
    CHECK(open_with([](AAudioStreamBuilder *b) { AAudioStreamBuilder_setChannelCount(b, 9); }) ==
          AAUDIO_ERROR_OUT_OF_RANGE);
    CHECK(open_with([](AAudioStreamBuilder *b) { AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_INPUT); }) ==
          AAUDIO_ERROR_UNAVAILABLE);
    CHECK(open_with([](AAudioStreamBuilder *b) { AAudioStreamBuilder_setChannelMask(b, AAUDIO_CHANNEL_STEREO); }) ==
          AAUDIO_OK);
}

}  // namespace

int main() {
    return apk::testing::run_all();
}
