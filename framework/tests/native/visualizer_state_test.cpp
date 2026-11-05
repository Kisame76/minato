// The state of android.media.audiofx.Visualizer (framework/native/android_runtime/visualizer_state.h) on the host, with
// the answers Android 15 gave in visualizer-test (fixtures/expected/visualizer-test-granted.logcat).
#include "../../native/android_runtime/visualizer_state.h"

#include <algorithm>

#include <cmath>
#include <cstdio>

using namespace minato::visualizer;

namespace {

int failures = 0;

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

void the_capture_size_takes_powers_of_two_in_the_range_while_disabled() {
    State v;
    CHECK(v.capture_size == 1024);
    CHECK(v.set_capture_size(256) == kSuccess && v.capture_size == 256);
    CHECK(v.set_capture_size(3) == kBadValue && v.capture_size == 256);
    CHECK(v.set_capture_size(64) == kBadValue);
    CHECK(v.set_capture_size(2048) == kBadValue);
    CHECK(v.set_capture_size(1000) == kBadValue);
    v.set_enabled(true);
    CHECK(v.set_capture_size(512) == kInvalidOperation && v.capture_size == 256);
}

void the_modes_take_two_values() {
    State v;
    CHECK(v.set_scaling_mode(1) == kSuccess && v.scaling_mode == 1);
    CHECK(v.set_scaling_mode(7) == kBadValue && v.scaling_mode == 1);
    CHECK(v.set_measurement_mode(1) == kSuccess && v.measurement_mode == 1);
    CHECK(v.set_measurement_mode(-1) == kBadValue);
}

void the_data_needs_the_enabled_state_and_a_big_enough_array() {
    State v;
    std::vector<uint8_t> out;
    CHECK(v.get_waveform(out, 1024) == kInvalidOperation);
    v.set_capture_size(256);
    v.set_enabled(true);
    CHECK(v.get_waveform(out, 255) == kBadValue);
    CHECK(v.get_waveform(out, 256) == kSuccess && out.size() == 256 && out[0] == 128 && out[255] == 128);
    CHECK(v.get_fft(out, 256) == kSuccess && out.size() == 256 && out[10] == 0);
    int peak = 0, rms = 0;
    CHECK(v.get_peak_rms(&peak, &rms) == kInvalidOperation);
    v.set_enabled(false);
    v.set_measurement_mode(1);
    CHECK(v.get_peak_rms(&peak, &rms) == kSuccess && peak == -9600 && rms == -9600);
}

std::vector<float> tone(int count, double hertz, double amplitude) {
    std::vector<float> samples(static_cast<size_t>(count));
    for (int index = 0; index < count; ++index) {
        samples[static_cast<size_t>(index)] =
            static_cast<float>(amplitude * std::sin(2 * 3.14159265358979 * hertz * index / 48000.0));
    }
    return samples;
}

void a_tone_shows_in_the_waveform_the_fft_and_the_peak_and_rms() {
    State v;
    v.set_capture_size(512);
    v.set_measurement_mode(1);
    v.set_enabled(true);
    const std::vector<float> samples = tone(512, 1000, 0.5);
    std::vector<uint8_t> wave, fft;
    CHECK(v.get_waveform(wave, 512, &samples) == kSuccess && wave.size() == 512);
    int high = 0, low = 255;
    for (uint8_t b : wave) {
        high = std::max<int>(high, b);
        low = std::min<int>(low, b);
    }
    CHECK(high >= 128 + 60 && high <= 128 + 64 && low <= 128 - 60 && low >= 128 - 64);
    CHECK(v.get_fft(fft, 512, &samples) == kSuccess && fft.size() == 512);
    int peak_bin = 0;
    int peak_magnitude = 0;
    for (int k = 1; k < 256; ++k) {
        const int re = static_cast<int8_t>(fft[static_cast<size_t>(2 * k)]);
        const int im = static_cast<int8_t>(fft[static_cast<size_t>(2 * k + 1)]);
        if (re * re + im * im > peak_magnitude) {
            peak_magnitude = re * re + im * im;
            peak_bin = k;
        }
    }
    CHECK(peak_bin == 10 || peak_bin == 11);  // 1000 / 48000 * 512 = 10.67
    CHECK(peak_magnitude > 20 * 20);
    int peak = 0, rms = 0;
    CHECK(v.get_peak_rms(&peak, &rms, &samples) == kSuccess);
    CHECK(peak >= -640 && peak <= -560);  // 0.5 is -602 mB
    CHECK(rms >= -940 && rms <= -860);    // 0.354 is -903 mB
    const std::vector<float> loud = tone(512, 1000, 1.0);
    CHECK(v.get_waveform(wave, 512, &loud) == kSuccess && wave[0] >= 120 && wave[0] <= 136);
    int loud_peak = 0, loud_rms = 0;
    CHECK(v.get_peak_rms(&loud_peak, &loud_rms, &loud) == kSuccess && loud_peak > -40 && loud_peak <= 0);
}

void the_periodic_capture_is_set_while_disabled() {
    State v;
    CHECK(!v.periodic());
    CHECK(v.set_periodic_capture(10000, true, true) == kSuccess && v.periodic());
    CHECK(v.set_periodic_capture(20001, true, true) == kBadValue);
    v.set_enabled(true);
    CHECK(v.set_periodic_capture(10000, true, false) == kInvalidOperation);
    v.set_enabled(false);
    CHECK(v.set_periodic_capture(10000, false, false) == kSuccess && !v.periodic());
    CHECK(v.set_periodic_capture(0, true, true) == kSuccess && !v.periodic());
}

}  // namespace

int main() {
    the_capture_size_takes_powers_of_two_in_the_range_while_disabled();
    the_modes_take_two_values();
    the_data_needs_the_enabled_state_and_a_big_enough_array();
    a_tone_shows_in_the_waveform_the_fft_and_the_peak_and_rms();
    the_periodic_capture_is_set_while_disabled();
    std::printf("visualizer_state_test: %s\n", failures == 0 ? "all fine" : "failed");
    return failures == 0 ? 0 : 1;
}
