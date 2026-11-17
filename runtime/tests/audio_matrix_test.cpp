// Measures what the guest's output paths hand the host (the gap-sort entry for a game heard at two or four times its
// speed, choppy and scratchy): libaaudio (runtime/android/aaudio.cpp) and libOpenSLES (opensles.cpp) on the Mac,
// built with tests/aaudio_host as aaudio_library_test and opensl_library_test are. Every case plays a known signal,
// the left channel a 440 Hz sine of amplitude 0.5 and the right one a 660 Hz sine of 0.25, N frames of it, and the
// fake host reads the stream as a render block does, the frames of 5 ms at the stream's rate every 5 ms. The cases are
// the paths AAudio data callback, AAudio blocking write and OpenSL ES buffer queue, the formats float, I16 and, for
// OpenSL ES, 8-bit unsigned and 16-bit, one, two and six channels, and the rates 8000, 11025, 22050, 24000, 44100
// and 48000 Hz. A case passes when
//   - the host was asked for the rate and the channels of the app,
//   - the frames the host read, less those that are all zeros, are exactly the signal in the format's own conversion
//     to float: nothing dropped, repeated, reordered, swapped between channels or read at another width,
//   - the zeros between the signal's first and last frame are those of the signal itself: no gap, and
//   - the signal's tone, counted by zero crossings over the frames at the stream's rate, is the tone.
// The clocks that mixers pace themselves by are measured too: a stream's frames written and read, its timestamp's
// position against its time (AAudio), and the position of an OpenSL ES player must advance at the rate, once per
// frame. A last case writes from a producer that is late and bursty (40 ms chunks with jitter) and shows what the host
// reads then. Every case logs its path, format, channels, rate, frames, xruns and tone.
#include "check.h"
#include "host.h"

#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
#include <aaudio/AAudio.h>

#include <mach/mach_time.h>
#include <time.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iterator>
#include <semaphore>
#include <string>
#include <thread>
#include <vector>

extern "C" int minato_host_syscall(int number, ...);

namespace {

using namespace audio_host;
using namespace std::chrono_literals;

constexpr double kLeftHz = 440.0, kRightHz = 660.0;
constexpr double kPi = 3.14159265358979323846;

// MARK: - The signal

enum class Format { f32, i16, u8 };

const char *name_of(Format format) {
    return format == Format::f32 ? "float" : format == Format::i16 ? "i16" : "u8";
}

/// The app's frames in the format's bytes, and the floats the host must read for them.
struct Signal {
    std::vector<std::uint8_t> bytes;
    std::vector<float> floats;
    std::int64_t frames = 0;
    std::int32_t channels = 0;
    std::size_t frame_bytes = 0;
};

float tone(int channel, std::int64_t frame, double rate) {
    const double hz = channel == 0 ? kLeftHz : channel == 1 ? kRightHz : 220.0 * (channel + 1);
    const double amplitude = channel == 0 ? 0.5 : 0.25;
    return static_cast<float>(amplitude * std::sin(2 * kPi * hz * static_cast<double>(frame) / rate));
}

Signal make_signal(Format format, std::int32_t channels, double rate, std::int64_t frames) {
    Signal signal;
    signal.frames = frames;
    signal.channels = channels;
    const std::size_t width = format == Format::f32 ? 4 : format == Format::i16 ? 2 : 1;
    signal.frame_bytes = width * static_cast<std::size_t>(channels);
    signal.bytes.resize(signal.frame_bytes * static_cast<std::size_t>(frames));
    signal.floats.resize(static_cast<std::size_t>(frames * channels));
    for (std::int64_t frame = 0; frame < frames; ++frame) {
        for (std::int32_t channel = 0; channel < channels; ++channel) {
            const float value = tone(channel, frame, rate);
            const auto at = static_cast<std::size_t>(frame * channels + channel);
            std::uint8_t *out = signal.bytes.data() + at * width;
            switch (format) {
                case Format::f32:
                    std::memcpy(out, &value, 4);
                    signal.floats[at] = value;
                    break;
                case Format::i16: {
                    const auto sample = static_cast<std::int16_t>(std::lround(value * 32767.0f));
                    std::memcpy(out, &sample, 2);
                    signal.floats[at] = static_cast<float>(sample) * (1.0f / 32768.0f);
                    break;
                }
                case Format::u8: {
                    const auto sample = static_cast<std::uint8_t>(std::lround(value * 127.0f) + 128);
                    *out = sample;
                    signal.floats[at] = (static_cast<float>(sample) - 128.0f) * (1.0f / 128.0f);
                    break;
                }
            }
        }
    }
    return signal;
}

// MARK: - The host's reads

/// Reads one stream as a render block does, in real time, until stopped.
class Consumer {
public:
    Consumer(std::uint64_t handle, double rate, std::int32_t channels, std::chrono::milliseconds period = 5ms)
        : handle_(handle), rate_(rate), channels_(channels), period_(period), thread_([this] { run(); }) {}
    ~Consumer() { stop(); }

    std::vector<float> stop() {
        if (thread_.joinable()) {
            stop_.store(true);
            thread_.join();
        }
        return std::move(samples_);
    }

private:
    void run() {
        double owed = 0;
        auto next = std::chrono::steady_clock::now();
        while (!stop_.load()) {
            next += period_;
            std::this_thread::sleep_until(next);
            owed += rate_ * static_cast<double>(period_.count()) / 1000.0;
            const auto frames = static_cast<std::int64_t>(owed);
            owed -= static_cast<double>(frames);
            const auto read = render(handle_, frames, channels_);
            samples_.insert(samples_.end(), read.begin(), read.end());
        }
    }

    std::uint64_t handle_;
    double rate_;
    std::int32_t channels_;
    std::chrono::milliseconds period_;
    std::atomic<bool> stop_{false};
    std::vector<float> samples_;
    std::thread thread_;
};

bool all_zero(const float *frame, std::int32_t channels) {
    for (std::int32_t channel = 0; channel < channels; ++channel) {
        if (frame[channel] != 0.0f) return false;
    }
    return true;
}

struct Heard {
    std::int64_t signal_frames = 0;  // the frames that are not all zeros
    std::int64_t gaps = 0;           // all-zero frames between the first signal frame and the last
    bool exact = false;              // the signal frames are the signal's, in order
    double left_hz = 0;
};

/// `read` against `signal`: the all-zero frames of the signal itself are dropped from both before they are compared.
Heard analyze(const std::vector<float> &read, const Signal &signal, double rate) {
    Heard heard;
    const auto channels = static_cast<std::size_t>(signal.channels);
    std::vector<float> got, expected;
    std::int64_t first = -1, last = -1, index = 0;
    for (std::size_t at = 0; at + channels <= read.size(); at += channels, ++index) {
        if (all_zero(&read[at], signal.channels)) continue;
        if (first < 0) first = index;
        last = index;
        got.insert(got.end(), read.begin() + static_cast<std::ptrdiff_t>(at),
                   read.begin() + static_cast<std::ptrdiff_t>(at + channels));
    }
    std::int64_t zero_frames_of_signal = 0;
    for (std::size_t at = 0; at + channels <= signal.floats.size(); at += channels) {
        if (all_zero(&signal.floats[at], signal.channels)) {
            ++zero_frames_of_signal;
            continue;
        }
        expected.insert(expected.end(), signal.floats.begin() + static_cast<std::ptrdiff_t>(at),
                        signal.floats.begin() + static_cast<std::ptrdiff_t>(at + channels));
    }
    heard.signal_frames = static_cast<std::int64_t>(got.size() / channels);
    heard.exact = got == expected;
    if (first >= 0) {
        index = 0;
        for (std::size_t at = 0; at + channels <= read.size(); at += channels, ++index) {
            if (index > first && index < last && all_zero(&read[at], signal.channels)) ++heard.gaps;
        }
        // The signal's own zeros inside it (its first frame is one) are not gaps.
        heard.gaps = std::max<std::int64_t>(0, heard.gaps - std::max<std::int64_t>(0, zero_frames_of_signal - 1));
        std::int64_t crossings = 0;
        float before = 0;
        for (std::size_t at = 0; at + channels <= got.size(); at += channels) {
            if (before < 0 && got[at] >= 0) ++crossings;
            before = got[at];
        }
        const double seconds = static_cast<double>(heard.signal_frames) / rate;
        heard.left_hz = seconds > 0 ? static_cast<double>(crossings) / seconds : 0;
    }
    return heard;
}

int xruns_of(std::uint64_t handle) {
    apkrt_audio_status status{};
    if (minato_host_syscall(APKRT_NR_AUDIO_STATUS, handle, reinterpret_cast<std::uint64_t>(&status)) != 0) return -1;
    return status.xruns;
}

struct Case {
    const char *path;
    Format format;
    std::int32_t channels;
    double rate;
    std::int64_t frames() const { return static_cast<std::int64_t>(rate * 0.4); }
};

/// What one case checks once its stream has played out; the producer's `written` is the frames it handed over.
void verify(const Case &c, const Signal &signal_in, const Opened &opened, const std::vector<float> &read, int xruns,
            bool bursty = false) {
    // An OpenSL ES source of one channel plays on a stream of two (opensles.cpp), the same signal on both.
    Signal signal = signal_in;
    if (opened.channels == 2 && c.channels == 1) {
        signal.channels = 2;
        signal.floats.clear();
        for (const float value : signal_in.floats) {
            signal.floats.push_back(value);
            signal.floats.push_back(value);
        }
    }
    const std::int32_t stream_channels = signal.channels;
    const Heard heard = analyze(read, signal, c.rate);
    // The signal starts with a frame of zeros (sin 0), so the all-zero count of the signal is at least 1.
    std::int64_t zeros_in_signal = 0;
    for (std::size_t at = 0; at + static_cast<std::size_t>(stream_channels) <= signal.floats.size();
         at += static_cast<std::size_t>(stream_channels)) {
        zeros_in_signal += all_zero(&signal.floats[at], stream_channels) ? 1 : 0;
    }
    const std::int64_t expected_frames = signal.frames - zeros_in_signal;
    const bool rate_ok = opened.sample_rate == c.rate && opened.channels == stream_channels;
    const bool tone_ok = std::fabs(heard.left_hz - kLeftHz) <= kLeftHz * 0.02 || bursty;
    const bool whole = heard.signal_frames == expected_frames && heard.exact;
    const bool no_gaps = bursty || heard.gaps == 0;
    std::fprintf(stderr,
                 "audio-matrix: %-14s %-5s %dch %5.0f Hz: asked %.0f Hz %dch, %lld of %lld frames, exact %d, gaps "
                 "%lld, xruns %d, tone %.1f Hz\n",
                 c.path, name_of(c.format), c.channels, c.rate, opened.sample_rate, opened.channels,
                 static_cast<long long>(heard.signal_frames), static_cast<long long>(expected_frames), heard.exact,
                 static_cast<long long>(heard.gaps), xruns, heard.left_hz);
    CHECK(rate_ok);
    CHECK(whole);
    CHECK(no_gaps);
    CHECK(tone_ok);
}

// MARK: - AAudio

AAudioStream *open_aaudio(const Case &c, AAudioStream_dataCallback callback, void *user) {
    AAudioStreamBuilder *builder = nullptr;
    CHECK(AAudio_createStreamBuilder(&builder) == AAUDIO_OK);
    AAudioStreamBuilder_setSampleRate(builder, static_cast<std::int32_t>(c.rate));
    AAudioStreamBuilder_setChannelCount(builder, c.channels);
    AAudioStreamBuilder_setFormat(builder, c.format == Format::f32 ? AAUDIO_FORMAT_PCM_FLOAT : AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_NONE);
    if (callback != nullptr) AAudioStreamBuilder_setDataCallback(builder, callback, user);
    AAudioStream *stream = nullptr;
    CHECK(AAudioStreamBuilder_openStream(builder, &stream) == AAUDIO_OK);
    AAudioStreamBuilder_delete(builder);
    return stream;
}

struct CallbackFeed {
    const Signal *signal;
    std::atomic<std::int64_t> given{0};
};

aaudio_data_callback_result_t feed(AAudioStream *, void *user, void *audio, int32_t frames) {
    auto &state = *static_cast<CallbackFeed *>(user);
    const Signal &signal = *state.signal;
    const std::int64_t at = state.given.load();
    const std::int64_t take = std::max<std::int64_t>(0, std::min<std::int64_t>(frames, signal.frames - at));
    auto *out = static_cast<std::uint8_t *>(audio);
    std::memcpy(out, signal.bytes.data() + at * static_cast<std::int64_t>(signal.frame_bytes),
                static_cast<std::size_t>(take) * signal.frame_bytes);
    std::memset(out + static_cast<std::size_t>(take) * signal.frame_bytes, 0,
                static_cast<std::size_t>(frames - take) * signal.frame_bytes);
    state.given.store(at + take);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

void play_out(AAudioStream *stream, std::uint64_t handle, std::int64_t frames) {
    // Until the host has read every frame written, and a little after, for a tail of zeros.
    CHECK(within(5'000ms, [&] { return AAudioStream_getFramesRead(stream) >= frames; }));
    (void)handle;
    std::this_thread::sleep_for(100ms);
}

void run_aaudio_callback(const Case &c) {
    use_fake_host();
    const Signal signal = make_signal(c.format, c.channels, c.rate, c.frames());
    CallbackFeed state{&signal};
    AAudioStream *stream = open_aaudio(c, feed, &state);
    const Opened opened = last_opened();
    Consumer consumer(opened.handle, c.rate, c.channels);
    CHECK(AAudioStream_requestStart(stream) == AAUDIO_OK);
    CHECK(within(5'000ms, [&] { return state.given.load() >= signal.frames; }));
    play_out(stream, opened.handle, signal.frames);
    const int xruns = xruns_of(opened.handle);
    const std::vector<float> read = consumer.stop();
    CHECK(AAudioStream_close(stream) == AAUDIO_OK);
    verify(c, signal, opened, read, xruns);
}

void run_aaudio_write(const Case &c) {
    use_fake_host();
    const Signal signal = make_signal(c.format, c.channels, c.rate, c.frames());
    AAudioStream *stream = open_aaudio(c, nullptr, nullptr);
    const Opened opened = last_opened();
    Consumer consumer(opened.handle, c.rate, c.channels);
    CHECK(AAudioStream_requestStart(stream) == AAUDIO_OK);
    std::int64_t at = 0;
    while (at < signal.frames) {
        const auto chunk = static_cast<std::int32_t>(std::min<std::int64_t>(256, signal.frames - at));
        const auto result = AAudioStream_write(
            stream, signal.bytes.data() + at * static_cast<std::int64_t>(signal.frame_bytes), chunk, 1'000'000'000);
        CHECK(result >= 0);
        if (result <= 0) break;
        at += result;
    }
    CHECK(AAudioStream_getFramesWritten(stream) == signal.frames);
    play_out(stream, opened.handle, signal.frames);
    CHECK(AAudioStream_getFramesRead(stream) == signal.frames);
    const int xruns = xruns_of(opened.handle);
    const std::vector<float> read = consumer.stop();
    CHECK(AAudioStream_close(stream) == AAUDIO_OK);
    verify(c, signal, opened, read, xruns);
}

// MARK: - OpenSL ES

struct SlEngine {
    SLObjectItf object = nullptr;
    SLEngineItf engine = nullptr;
    SLObjectItf mix = nullptr;
    SlEngine() {
        CHECK(slCreateEngine(&object, 0, nullptr, 0, nullptr, nullptr) == SL_RESULT_SUCCESS);
        CHECK((*object)->Realize(object, SL_BOOLEAN_FALSE) == SL_RESULT_SUCCESS);
        CHECK((*object)->GetInterface(object, SL_IID_ENGINE, &engine) == SL_RESULT_SUCCESS);
        // SDL's openslES_CreateEngine: the output mix with an optional VOLUME interface.
        const SLInterfaceID ids[1] = {SL_IID_VOLUME};
        const SLboolean required[1] = {SL_BOOLEAN_FALSE};
        CHECK((*engine)->CreateOutputMix(engine, &mix, 1, ids, required) == SL_RESULT_SUCCESS);
        CHECK((*mix)->Realize(mix, SL_BOOLEAN_FALSE) == SL_RESULT_SUCCESS);
    }
    ~SlEngine() {
        (*mix)->Destroy(mix);
        (*object)->Destroy(object);
    }
};

struct SlFeed {
    const Signal *signal = nullptr;
    SLAndroidSimpleBufferQueueItf queue = nullptr;
    std::atomic<std::int64_t> enqueued{0};
    std::int64_t chunk = 0;
};

void sl_enqueue_next(SlFeed &state) {
    const std::int64_t at = state.enqueued.load();
    if (at >= state.signal->frames) return;
    const std::int64_t take = std::min(state.chunk, state.signal->frames - at);
    state.enqueued.store(at + take);
    const std::int64_t frame_bytes = static_cast<std::int64_t>(state.signal->frame_bytes);
    CHECK((*state.queue)->Enqueue(state.queue, state.signal->bytes.data() + at * frame_bytes,
                                  static_cast<SLuint32>(take * frame_bytes)) == SL_RESULT_SUCCESS);
}

void sl_refill(SLAndroidSimpleBufferQueueItf, void *user) {
    sl_enqueue_next(*static_cast<SlFeed *>(user));
}

void run_opensl(const Case &c) {
    use_fake_host();
    const Signal signal = make_signal(c.format, c.channels, c.rate, c.frames());
    SlEngine engine;
    const SLuint32 bits = c.format == Format::u8 ? 8 : 16;
    SLDataLocator_AndroidSimpleBufferQueue queue_locator{SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, 3};
    SLDataFormat_PCM format{SL_DATAFORMAT_PCM, static_cast<SLuint32>(c.channels),
                            static_cast<SLuint32>(c.rate * 1000), bits, bits, 0, SL_BYTEORDER_LITTLEENDIAN};
    SLDataSource source{&queue_locator, &format};
    SLDataLocator_OutputMix mix_locator{SL_DATALOCATOR_OUTPUTMIX, engine.mix};
    SLDataSink sink{&mix_locator, nullptr};
    SLInterfaceID ids[] = {SL_IID_ANDROIDSIMPLEBUFFERQUEUE};
    SLboolean required[] = {SL_BOOLEAN_TRUE};
    SLObjectItf player = nullptr;
    CHECK((*engine.engine)->CreateAudioPlayer(engine.engine, &player, &source, &sink, 1, ids, required) ==
          SL_RESULT_SUCCESS);
    CHECK((*player)->Realize(player, SL_BOOLEAN_FALSE) == SL_RESULT_SUCCESS);
    const Opened opened = last_opened();
    SLPlayItf play = nullptr;
    SlFeed state;
    state.signal = &signal;
    state.chunk = static_cast<std::int64_t>(c.rate * 0.02);
    CHECK((*player)->GetInterface(player, SL_IID_PLAY, &play) == SL_RESULT_SUCCESS);
    CHECK((*player)->GetInterface(player, SL_IID_ANDROIDSIMPLEBUFFERQUEUE, &state.queue) == SL_RESULT_SUCCESS);
    CHECK((*state.queue)->RegisterCallback(state.queue, sl_refill, &state) == SL_RESULT_SUCCESS);
    Consumer consumer(opened.handle, c.rate, opened.channels);
    sl_enqueue_next(state);
    sl_enqueue_next(state);
    CHECK((*play)->SetPlayState(play, SL_PLAYSTATE_PLAYING) == SL_RESULT_SUCCESS);
    // Mid-play (the signal lasts 400 ms) the position is the time heard, once per frame at the rate: 250 ms later it
    // reads about 250 ms. At the end of the queue it starts again from 0, as Android's does at HEADATEND.
    std::this_thread::sleep_for(250ms);
    SLmillisecond position = 0;
    CHECK((*play)->GetPosition(play, &position) == SL_RESULT_SUCCESS);
    std::fprintf(stderr, "audio-matrix: opensl position %u ms 250 ms after the start\n", position);
    CHECK(position >= 190 && position <= 320);
    CHECK(within(5'000ms, [&] {
        SLAndroidSimpleBufferQueueState queue_state{99, 99};
        (*state.queue)->GetState(state.queue, &queue_state);
        return state.enqueued.load() >= signal.frames && queue_state.count == 0;
    }));
    std::this_thread::sleep_for(100ms);
    const int xruns = xruns_of(opened.handle);
    const std::vector<float> read = consumer.stop();
    (*player)->Destroy(player);
    verify(c, signal, opened, read, xruns);
}

// MARK: - The matrix

constexpr double kRates[] = {8000, 11025, 22050, 24000, 44100, 48000};

// Every case plays in real time, so each path takes the pairs, not the whole product: every rate with both formats,
// the channel count alternating so that each rate has both counts and each format has both counts over the rates.
// `play` runs one case of a path.
template <typename Play>
void every_pair(const char *path, Format first, Format second, Play play) {
    for (std::size_t index = 0; index < std::size(kRates); ++index) {
        const std::int32_t channels = index % 2 == 0 ? 1 : 2;
        play(Case{path, first, channels, kRates[index]});
        play(Case{path, second, 3 - channels, kRates[index]});
    }
}

TEST(aaudio_data_callback_hands_the_host_the_signal_at_every_rate_format_and_channel_count) {
    every_pair("aaudio-callback", Format::f32, Format::i16, [](const Case &c) { run_aaudio_callback(c); });
    run_aaudio_callback({"aaudio-callback", Format::f32, 6, 24000});
}

TEST(aaudio_blocking_write_hands_the_host_the_signal_at_every_rate_format_and_channel_count) {
    every_pair("aaudio-write", Format::f32, Format::i16, [](const Case &c) { run_aaudio_write(c); });
    run_aaudio_write({"aaudio-write", Format::f32, 6, 48000});
}

TEST(opensl_buffer_queue_hands_the_host_the_signal_at_every_rate_format_and_channel_count) {
    every_pair("opensl-queue", Format::u8, Format::i16, [](const Case &c) { run_opensl(c); });
}

// MARK: - The clocks

TEST(aaudio_clocks_advance_at_the_rate_one_per_frame) {
    // Each rate once, the channel count alternating: the clocks do not depend on the count, and a case takes 3.3 s.
    struct ClockCase {
        double rate;
        std::int32_t channels;
    };
    for (const ClockCase clock : {ClockCase{11025, 1}, ClockCase{24000, 2}, ClockCase{44100, 1}}) {
        const double rate = clock.rate;
        const std::int32_t channels = clock.channels;
        use_fake_host();
        const Case c{"aaudio-clock", Format::f32, channels, rate};
        // The signal outlasts the measurement window below, so the clock keeps advancing across the whole window.
        const Signal signal = make_signal(c.format, channels, rate, static_cast<std::int64_t>(rate * 4));
        CallbackFeed state{&signal};
        AAudioStream *stream = open_aaudio(c, feed, &state);
        const Opened opened = last_opened();
        Consumer consumer(opened.handle, rate, channels);
        CHECK(AAudioStream_requestStart(stream) == AAUDIO_OK);
        std::this_thread::sleep_for(300ms);  // let the stream settle before the first sample
        const auto wall_a = std::chrono::steady_clock::now();
        const std::int64_t read_a = AAudioStream_getFramesRead(stream);
        std::int64_t position_a = 0, time_a = 0;
        const bool stamp_a = AAudioStream_getTimestamp(stream, CLOCK_MONOTONIC, &position_a, &time_a) == AAUDIO_OK;
        // A multi-second window: a single short window can catch the consumer briefly behind under load and read
        // low, so measure over ~3 s, where a bounded transient is a small fraction. Rate from the stamps' deltas.
        std::this_thread::sleep_for(3'000ms);
        const std::int64_t read_b = AAudioStream_getFramesRead(stream);
        const auto wall_b = std::chrono::steady_clock::now();
        std::int64_t position_b = 0, time_b = 0;
        const bool stamp_b = AAudioStream_getTimestamp(stream, CLOCK_MONOTONIC, &position_b, &time_b) == AAUDIO_OK;
        const double wall_seconds = std::chrono::duration<double>(wall_b - wall_a).count();
        const double read_rate = static_cast<double>(read_b - read_a) / wall_seconds;  // frames per second
        const double stamp_rate = time_b > time_a ? static_cast<double>(position_b - position_a) * 1e9 /
                                                        static_cast<double>(time_b - time_a)
                                                  : 0;
        std::fprintf(stderr,
                     "audio-matrix: aaudio clocks %dch %5.0f Hz: framesRead %.0f frames/s over %.2f s, timestamp "
                     "%.0f frames/s (stamps %d %d), framesWritten %lld\n",
                     channels, rate, read_rate, wall_seconds, stamp_rate, stamp_a, stamp_b,
                     static_cast<long long>(AAudioStream_getFramesWritten(stream)));
        CHECK(std::fabs(read_rate - rate) <= rate * 0.08);
        if (stamp_a && stamp_b) CHECK(std::fabs(stamp_rate - rate) <= rate * 0.05);
        CHECK(AAudioStream_getFramesWritten(stream) >= read_b);
        consumer.stop();
        CHECK(AAudioStream_close(stream) == AAUDIO_OK);
    }
}

// MARK: - A late, bursty producer

TEST(a_late_bursty_producer_leaves_gaps_of_zeros_and_no_dropped_frames) {
    use_fake_host();
    const Case c{"aaudio-bursty", Format::f32, 1, 22050};
    const Signal signal = make_signal(c.format, c.channels, c.rate, static_cast<std::int64_t>(c.rate * 1.0));
    AAudioStream *stream = open_aaudio(c, nullptr, nullptr);
    const Opened opened = last_opened();
    Consumer consumer(opened.handle, c.rate, c.channels);
    CHECK(AAudioStream_requestStart(stream) == AAUDIO_OK);
    std::int64_t at = 0;
    int burst = 0;
    while (at < signal.frames) {
        std::this_thread::sleep_for(std::chrono::milliseconds(40 + (burst++ % 3) * 25));
        const auto chunk = static_cast<std::int32_t>(
            std::min<std::int64_t>(static_cast<std::int64_t>(c.rate * 0.04), signal.frames - at));
        const auto result = AAudioStream_write(
            stream, signal.bytes.data() + at * static_cast<std::int64_t>(signal.frame_bytes), chunk, 1'000'000'000);
        CHECK(result == chunk);
        if (result <= 0) break;
        at += result;
    }
    play_out(stream, opened.handle, signal.frames);
    const int xruns = xruns_of(opened.handle);
    const std::int64_t written = AAudioStream_getFramesWritten(stream);
    const std::int64_t read_frames = AAudioStream_getFramesRead(stream);
    const std::vector<float> read = consumer.stop();
    CHECK(AAudioStream_close(stream) == AAUDIO_OK);
    std::fprintf(stderr, "audio-matrix: bursty producer: framesWritten %lld, framesRead %lld, xruns %d\n",
                 static_cast<long long>(written), static_cast<long long>(read_frames), xruns);
    CHECK(written == signal.frames);
    CHECK(read_frames == signal.frames);
    verify(c, signal, opened, read, xruns, true);
}

// MARK: - SDL2's AAudio driver (Ren'Py's output on Android)

// SDL_aaudio.c (zlib), as read: the builder gets the rate, the channel count and the format (I16 for S16, else
// float), no performance mode and no data callback; the stream's rate, channels and format are read back; it is
// started; and the audio thread writes spec.size bytes (1024 frames at 44100 or 48000 Hz) with a timeout of 1 ms
// at once after the last (SDL_RunAudio calls PlayDevice and then WaitDevice, which this driver leaves the default
// no-op), and ignores a positive result: a write that took fewer frames than it was given drops the rest. So nothing
// paces the thread but the write's own wait of at most 1 ms. The lines show what that does at the ring's edge, with
// Minato's 20 ms IO buffer for a stream without a performance mode. Android's answer is the AVD's to give (the
// reference run is pending), so this case reports and checks nothing.
TEST(sdls_aaudio_driver_writes_of_1024_frames_with_a_timeout_of_1_ms_block_and_pace_the_thread) {
    use_fake_host();
    fake.io_buffer = 0.020;
    constexpr std::int32_t rate = 48000, chunk = 1024;
    AAudioStreamBuilder *builder = nullptr;
    CHECK(AAudio_createStreamBuilder(&builder) == AAUDIO_OK);
    AAudioStreamBuilder_setSampleRate(builder, rate);
    AAudioStreamBuilder_setChannelCount(builder, 2);
    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
    AAudioStream *stream = nullptr;
    CHECK(AAudioStreamBuilder_openStream(builder, &stream) == AAUDIO_OK);
    AAudioStreamBuilder_delete(builder);
    CHECK(AAudioStream_getSampleRate(stream) == rate);
    CHECK(AAudioStream_getChannelCount(stream) == 2);
    CHECK(AAudioStream_getFormat(stream) == AAUDIO_FORMAT_PCM_I16);
    const Opened opened = last_opened();
    Consumer consumer(opened.handle, rate, 2, 20ms);
    CHECK(AAudioStream_requestStart(stream) == AAUDIO_OK);
    int partial = 0, calls = 0;
    std::int64_t accepted = 0, frame = 0;
    std::vector<std::int16_t> buffer(static_cast<std::size_t>(chunk) * 2);
    const auto begin = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - begin < 2'000ms) {
        for (std::int32_t i = 0; i < chunk; ++i, ++frame) {
            const float value = tone(0, frame, rate);
            buffer[static_cast<std::size_t>(i) * 2] = static_cast<std::int16_t>(std::lround(value * 32767.0f));
            buffer[static_cast<std::size_t>(i) * 2 + 1] =
                static_cast<std::int16_t>(std::lround(tone(1, frame, rate) * 32767.0f));
        }
        const auto result = AAudioStream_write(stream, buffer.data(), chunk, 1'000'000);
        CHECK(result >= 0);
        ++calls;
        if (result < chunk) ++partial;
        if (result > 0) accepted += result;
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
    const int xruns = xruns_of(opened.handle);
    consumer.stop();
    std::fprintf(stderr,
                 "audio-matrix: SDL AAudio sequence, %d unpaced writes of %d frames at %d Hz with a 1 ms timeout in "
                 "%.2f s: %d took fewer frames than given; %lld of %lld frames accepted (SDL drops the rest), "
                 "%.0f mixed frames/s, xruns %d, buffer size %d frames, frames per burst %d\n",
                 calls, chunk, rate, seconds, partial, static_cast<long long>(accepted), static_cast<long long>(frame),
                 static_cast<double>(frame) / seconds, xruns,
                 AAudioStream_getBufferSizeInFrames(stream), AAudioStream_getFramesPerBurst(stream));
    CHECK(AAudioStream_close(stream) == AAUDIO_OK);
    CHECK(partial == 0);
    CHECK(static_cast<double>(frame) / seconds <= rate * 1.12);
    CHECK(static_cast<double>(frame) / seconds >= rate * 0.95);
}

// MARK: - SDL2's OpenSL ES driver (SDL 2.30's first choice on Android, Ren'Py's output)

// SDL_openslES.c (zlib), as read: a player of SL_DATAFORMAT_PCM (16-bit, rate in milliHz, the channel mask of the
// count, e.g. SL_ANDROID_SPEAKER_STEREO) with two buffers in a simple buffer queue, interfaces BUFFERQUEUE and
// VOLUME (not required), a callback that posts a semaphore of initial count 1; the audio thread mixes a buffer,
// enqueues it and waits on the semaphore, which only the callback of a finished buffer posts. So the thread is paced
// by the stream alone: what it mixes in a second is what the host played in a second, plus the buffers in flight.
struct SdlSl {
    std::counting_semaphore<8> sem{1};
};

void sdl_sl_callback(SLAndroidSimpleBufferQueueItf, void *user) {
    static_cast<SdlSl *>(user)->sem.release();
}

void run_sdl_opensl(std::int32_t rate, std::int32_t samples) {
    use_fake_host();
    fake.io_buffer = 0.020;
    SlEngine engine;
    SLDataLocator_AndroidSimpleBufferQueue queue_locator{SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, 2};
    SLDataFormat_PCM format{SL_DATAFORMAT_PCM, 2, static_cast<SLuint32>(rate) * 1000, 16, 16,
                            SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT, SL_BYTEORDER_LITTLEENDIAN};
    SLDataSource source{&queue_locator, &format};
    SLDataLocator_OutputMix mix_locator{SL_DATALOCATOR_OUTPUTMIX, engine.mix};
    SLDataSink sink{&mix_locator, nullptr};
    SLInterfaceID ids[] = {SL_IID_ANDROIDSIMPLEBUFFERQUEUE, SL_IID_VOLUME};
    SLboolean required[] = {SL_BOOLEAN_TRUE, SL_BOOLEAN_FALSE};
    SLObjectItf player = nullptr;
    CHECK((*engine.engine)->CreateAudioPlayer(engine.engine, &player, &source, &sink, 2, ids, required) ==
          SL_RESULT_SUCCESS);
    CHECK((*player)->Realize(player, SL_BOOLEAN_FALSE) == SL_RESULT_SUCCESS);
    const Opened opened = last_opened();
    SLPlayItf play = nullptr;
    SLAndroidSimpleBufferQueueItf queue = nullptr;
    CHECK((*player)->GetInterface(player, SL_IID_PLAY, &play) == SL_RESULT_SUCCESS);
    CHECK((*player)->GetInterface(player, SL_IID_ANDROIDSIMPLEBUFFERQUEUE, &queue) == SL_RESULT_SUCCESS);
    SdlSl sdl;
    CHECK((*queue)->RegisterCallback(queue, sdl_sl_callback, &sdl) == SL_RESULT_SUCCESS);
    Consumer consumer(opened.handle, rate, opened.channels, 20ms);
    CHECK((*play)->SetPlayState(play, SL_PLAYSTATE_PLAYING) == SL_RESULT_SUCCESS);
    std::vector<std::vector<std::int16_t>> buffers(2, std::vector<std::int16_t>(static_cast<std::size_t>(samples) * 2));
    std::int64_t frame = 0;
    int next = 0, failed = 0;
    const auto begin = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - begin < 2'000ms) {
        // SDL_RunAudio: mix into the device's buffer, play it (Enqueue), then wait (WaitDevice: the semaphore).
        auto &buffer = buffers[static_cast<std::size_t>(next)];
        for (std::int32_t i = 0; i < samples; ++i, ++frame) {
            // A ramp that counts frames, to see every frame once and in order.
            buffer[static_cast<std::size_t>(i) * 2] = static_cast<std::int16_t>(frame % 20000 + 1);
            buffer[static_cast<std::size_t>(i) * 2 + 1] = static_cast<std::int16_t>(-(frame % 20000 + 1));
        }
        if ((*queue)->Enqueue(queue, buffer.data(), static_cast<SLuint32>(buffer.size() * 2)) != SL_RESULT_SUCCESS) {
            ++failed;
            sdl.sem.release();
        }
        next = (next + 1) % 2;
        if (!sdl.sem.try_acquire_for(500ms)) break;  // the thread is stuck: the stream never finishes a buffer
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
    std::this_thread::sleep_for(200ms);
    const std::vector<float> read = consumer.stop();
    (*player)->Destroy(player);
    // Frames the host read, in order: the ramp from frame 1 with no jump and no repeat.
    std::int64_t jumps = 0, heard = 0;
    std::int64_t expected = 1;
    for (std::size_t at = 0; at + 1 < read.size(); at += 2) {
        const float left = read[at];
        if (left == 0.0f && read[at + 1] == 0.0f) continue;
        ++heard;
        const auto value = static_cast<std::int64_t>(std::lround(left * 32768.0f));
        if (value != expected) {
            ++jumps;
            expected = value;
        }
        expected = (expected % 20000) + 1;
    }
    const double mixed_per_second = static_cast<double>(frame) / seconds;
    std::fprintf(stderr,
                 "audio-matrix: SDL OpenSL sequence %d Hz, %d-frame buffers: mixed %lld frames in %.2f s = %.0f "
                 "frames/s, host heard %lld of %lld frames read, jumps %lld, enqueue failures %d\n",
                 rate, samples, static_cast<long long>(frame), seconds, mixed_per_second, static_cast<long long>(heard),
                 static_cast<long long>(read.size() / 2), static_cast<long long>(jumps), failed);
    CHECK(failed == 0);
    CHECK(jumps == 0);
    // Paced by the host: the thread runs ahead of the host by the buffers in flight, 2 buffers and the ring, at most.
    CHECK(static_cast<double>(frame) <= rate * seconds + 2.0 * samples + rate * 0.2);
    // And no gap: of the frames the host read, at least 97 % are the thread's (the rest is the stream's silence).
    CHECK(static_cast<double>(heard) >= 0.97 * static_cast<double>(read.size() / 2));
}

TEST(sdls_opensl_driver_is_paced_by_the_stream_and_hands_the_host_every_frame_in_order) {
    for (const std::int32_t rate : {22050, 44100, 48000}) {
        for (const std::int32_t samples : {512, 1024, 2048}) run_sdl_opensl(rate, samples);
    }
}

}  // namespace

int main() {
    return apk::testing::run_all();
}
