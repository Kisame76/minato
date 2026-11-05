// The runtime's audio effects (runtime/src/audio_effects.h) on the host, without a stream: the frequency response of
// each effect by sine tones through a Processor, against the gains of Android's effect bundle as its open sources give
// them (five bands at 60/230/910/3600/14000 Hz, Q 0.96; the emulator gives no response to measure), the identity of
// what is off, the stereo widening, the loudness gain and its limiter, and the
// ramp: a change of settings on a running tone moves the signal with no step larger than the tone's own.
#include "audio_effects.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace apkrt::effects;

namespace {

int failures = 0;

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

constexpr double kPi = 3.14159265358979323846;
constexpr float kRate = 48000;

/// The steady-state amplitude of a sine of `hertz` through the Processor, in dB relative to the input's: the tone runs
/// for a second, the last 0.2 s are measured. `left_only` puts the tone on the left channel only.
double gain_db(const Settings &settings, double hertz, int channels = 1, bool left_only = false, double amplitude = 0.1,
               int channel_measured = 0) {
    Processor processor;
    processor.set(settings, kRate);
    const int total = static_cast<int>(kRate);
    std::vector<float> samples(static_cast<size_t>(total) * static_cast<size_t>(channels), 0.0f);
    for (int frame = 0; frame < total; ++frame) {
        const float value = static_cast<float>(amplitude * std::sin(2 * kPi * hertz * frame / kRate));
        for (int channel = 0; channel < channels; ++channel) {
            samples[static_cast<size_t>(frame * channels + channel)] = (left_only && channel > 0) ? 0.0f : value;
        }
    }
    processor.process(samples.data(), total, channels);
    // The level as RMS over the last 4800 frames, a whole number of periods of every tone here (also those with only
    // three samples to a period, where a peak read of the samples would be 1 dB low).
    double sum = 0;
    for (int frame = total - 4800; frame < total; ++frame) {
        const double value = samples[static_cast<size_t>(frame * channels + channel_measured)];
        sum += value * value;
    }
    return 20 * std::log10(std::sqrt(2 * sum / 4800) / amplitude);
}

Settings equalizer(int band, int millibel) {
    Settings settings;
    settings.equalizer = true;
    settings.band_millibel[band] = millibel;
    return settings;
}

// The gains in dB the bundle's bands give at their centres and an octave away.
struct Measured {
    int band;
    int millibel;
    double hertz;
    double db;
    double tolerance;
};
const Measured kMeasured[] = {
    {2, 1500, 910, 15.0, 0.5},   {2, -1500, 910, -15.0, 0.5}, {0, 1500, 60, 15.0, 0.5},
    {4, 1500, 14000, 15.0, 0.5}, {3, 1500, 3600, 15.0, 0.5},  {1, 1500, 230, 15.0, 0.5},
};

void the_equalizer_gains_match_the_measured_points() {
    for (const Measured &m : kMeasured) {
        const double got = gain_db(equalizer(m.band, m.millibel), m.hertz);
        if (std::fabs(got - m.db) > m.tolerance) {
            std::printf("band %d %d mB at %.0f Hz: %.2f dB, expected %.2f +- %.2f\n", m.band, m.millibel, m.hertz, got,
                        m.db, m.tolerance);
            ++failures;
        }
    }
    // Far from the band the tone is left alone.
    CHECK(std::fabs(gain_db(equalizer(2, 1500), 60)) < 1.0);
    CHECK(std::fabs(gain_db(equalizer(2, 1500), 14000)) < 1.0);
    // Another band does not touch this one's centre much.
    CHECK(std::fabs(gain_db(equalizer(0, 1500), 3600)) < 1.0);
}

void what_is_off_is_the_identity() {
    Settings off;
    std::vector<float> samples(1000), copy;
    for (size_t i = 0; i < samples.size(); ++i) {
        samples[i] = static_cast<float>(std::sin(static_cast<double>(i) * 0.37));
    }
    copy = samples;
    Processor processor;
    processor.set(off, kRate);
    CHECK(processor.idle());
    processor.process(samples.data(), 500, 2);
    CHECK(samples == copy);
    Settings on = equalizer(2, 0);  // enabled, every band flat
    processor.set(on, kRate);
    processor.process(samples.data(), 500, 2);
    for (size_t i = 0; i < samples.size(); ++i) CHECK(std::fabs(samples[i] - copy[i]) < 1e-5f);
}

// Android's dynamic bass enhancement (LVM, android-15.0.0_r1: a second-order high-pass at 90 Hz under the whole signal
// and a band peak around 90 Hz of gain G(level), level = 15 * strength / 1000), as its small-signal response gives it
// in dB at 48 kHz, for strengths 0, 500 and 1000.
struct BassPoint {
    int strength;
    double hertz;
    double decibel;
};
const BassPoint kBassPoints[] = {
    {0, 20, -26.16},   {0, 40, -13.89},   {0, 60, -6.53},   {0, 90, -0.13},   {0, 120, 0.65},   {0, 200, 0.23},
    {0, 1000, -0.08},  {500, 20, -25.44}, {500, 40, -11.25}, {500, 60, -1.48}, {500, 90, 6.83},  {500, 120, 6.53},
    {500, 200, 2.95},  {500, 300, 1.35},  {500, 500, 0.44},  {1000, 20, -22.41}, {1000, 40, -5.22}, {1000, 60, 5.96},
    {1000, 90, 14.81}, {1000, 120, 14.24}, {1000, 200, 9.06}, {1000, 300, 5.67}, {1000, 500, 2.73}, {1000, 1000, 0.77}};

void the_bass_boost_follows_the_response_of_androids_dynamic_bass_enhancement() {
    for (const BassPoint &point : kBassPoints) {
        Settings settings;
        settings.bass_boost = true;
        settings.bass_strength = point.strength;
        const double got = gain_db(settings, point.hertz);
        if (std::fabs(got - point.decibel) > 0.5) {
            std::printf("bass strength %d at %g Hz: %.2f dB, wanted %.2f\n", point.strength, point.hertz, got,
                        point.decibel);
        }
        CHECK(std::fabs(got - point.decibel) <= 0.5);
    }
    Settings settings;
    settings.bass_boost = true;
    settings.bass_strength = 1000;
    CHECK(std::fabs(gain_db(settings, 90, 2, false) - 14.81) <= 0.5);  // both channels equal: as one
    CHECK(std::fabs(gain_db(settings, 3000)) < 1.0);                    // the top is untouched
}

/// The gain in dB of a stereo tone whose channels are in anti-phase (a pure side signal) at the left channel.
double side_gain_db(const Settings &settings, double hertz) {
    Processor processor;
    processor.set(settings, kRate);
    const int total = static_cast<int>(kRate);
    std::vector<float> samples(static_cast<size_t>(total) * 2, 0.0f);
    for (int frame = 0; frame < total; ++frame) {
        const float value = static_cast<float>(0.1 * std::sin(2 * kPi * hertz * frame / kRate));
        samples[static_cast<size_t>(frame * 2)] = value;
        samples[static_cast<size_t>(frame * 2 + 1)] = -value;
    }
    processor.process(samples.data(), total, 2);
    double sum = 0;
    for (int frame = total - 4800; frame < total; ++frame) {
        const double value = samples[static_cast<size_t>(frame * 2)];
        sum += value * value;
    }
    return 20 * std::log10(std::sqrt(2 * sum / 4800) / 0.1);
}

// Android's virtualizer (LVM's LVCS for headphones, android-15.0.0_r1): mid and side go through filters of their own
// and a shared equaliser, mixed with the unprocessed signal by the strength. Its small-signal response at 48 kHz, in
// dB: the mid path (what a mono signal hears) against the unprocessed level, and the side-to-mid ratio against it.
const double kVirtHertz[] = {50, 100, 200, 500, 1000, 2000, 4000, 8000, 16000};
const double kVirtMid[3][9] = {{2.19, 1.87, 1.03, 0.23, 0.06, 0.07, 0.26, 0.83, 1.62},
                               {3.94, 3.46, 2.07, 0.49, 0.12, 0.14, 0.56, 1.66, 3.00},
                               {6.67, 6.05, 4.01, 1.11, 0.26, 0.28, 1.20, 3.22, 5.28}};
const double kVirtRatio[3][9] = {{-2.19, 0.38, 2.67, 3.87, 3.90, 3.48, 2.43, 0.90, -0.67},
                                 {-2.91, 1.09, 4.36, 6.38, 6.56, 5.96, 4.30, 1.63, -1.16},
                                 {-2.48, 2.21, 6.25, 9.55, 10.16, 9.42, 6.91, 2.66, -1.82}};

void the_virtualizer_follows_the_response_of_androids_stereo_widening() {
    const int strengths[3] = {250, 500, 1000};
    for (int row = 0; row < 3; ++row) {
        Settings settings;
        settings.virtualizer = true;
        settings.virtualizer_strength = strengths[row];
        for (int column = 0; column < 9; ++column) {
            const double mid = gain_db(settings, kVirtHertz[column], 2, false);
            const double side = side_gain_db(settings, kVirtHertz[column]);
            const bool near = std::fabs(mid - kVirtMid[row][column]) <= 0.5 &&
                              std::fabs(side - mid - kVirtRatio[row][column]) <= 0.5;
            if (!near) {
                std::printf("virtualizer %d at %g Hz: mid %.2f (wanted %.2f), side/mid %.2f (wanted %.2f)\n",
                            strengths[row], kVirtHertz[column], mid, kVirtMid[row][column], side - mid,
                            kVirtRatio[row][column]);
            }
            CHECK(std::fabs(mid - kVirtMid[row][column]) <= 0.5);
            CHECK(std::fabs(side - mid - kVirtRatio[row][column]) <= 0.5);
        }
    }
}

void the_virtualizer_at_strength_zero_leaves_every_signal_alone() {
    Settings settings;
    settings.virtualizer = true;
    settings.virtualizer_strength = 0;
    for (const double hertz : {50.0, 1000.0, 16000.0}) {
        CHECK(std::fabs(gain_db(settings, hertz, 2, false)) < 0.1);
        CHECK(std::fabs(side_gain_db(settings, hertz)) < 0.1);
        CHECK(std::fabs(gain_db(settings, hertz, 1)) < 0.1);
    }
    CHECK(std::isinf(gain_db(settings, 1000, 2, true, 0.1, 1)));  // left only, the right channel is silent: no bleed
}

void a_mono_stream_hears_the_virtualizers_mid_path() {
    Settings settings;
    settings.virtualizer = true;
    settings.virtualizer_strength = 1000;
    CHECK(std::fabs(gain_db(settings, 50, 1) - 6.67) <= 0.5);
    CHECK(std::fabs(gain_db(settings, 1000, 1) - 0.26) <= 0.5);
    CHECK(std::fabs(gain_db(settings, 16000, 1) - 5.28) <= 0.5);
}

void the_loudness_gain_is_linear_and_limited() {
    Settings settings;
    settings.loudness = true;
    settings.loudness_millibel = 600;
    CHECK(std::fabs(gain_db(settings, 1000) - 6.0) < 0.2);  // a small tone: the gain as asked
    settings.loudness_millibel = 1500;
    CHECK(std::fabs(gain_db(settings, 1000) - 15.0) < 0.3);
    // A big tone cannot pass full scale.
    Processor processor;
    processor.set(settings, kRate);
    std::vector<float> samples(4800);
    for (size_t i = 0; i < samples.size(); ++i) {
        samples[i] = static_cast<float>(0.9 * std::sin(static_cast<double>(i) * 0.1));
    }
    processor.process(samples.data(), 4800, 1);
    float top = 0;
    for (float s : samples) top = std::max(top, std::fabs(s));
    CHECK(top <= 1.0f && top > 0.9f);
}

void a_change_of_settings_ramps_without_a_click() {
    Processor processor;
    Settings quiet = equalizer(2, 0);
    processor.set(quiet, kRate);
    const int total = 4800;
    std::vector<float> samples(total);
    for (int i = 0; i < total; ++i) {
        samples[static_cast<size_t>(i)] = static_cast<float>(0.1 * std::sin(2 * kPi * 910 * i / kRate));
    }
    std::vector<float> out(samples);
    // The first half with the flat band, then a +15 dB boost at the tone's own frequency.
    processor.process(out.data(), 2400, 1);
    processor.set(equalizer(2, 1500), kRate);
    processor.process(out.data() + 2400, 2400, 1);
    // The largest step of the steady boosted tone, for the bound.
    Processor steady;
    steady.set(equalizer(2, 1500), kRate);
    std::vector<float> reference(samples);
    steady.process(reference.data(), total, 1);
    float bound = 0;
    for (size_t i = 2500; i < static_cast<size_t>(total); ++i) {
        bound = std::max(bound, std::fabs(reference[i] - reference[i - 1]));
    }
    float worst = 0;
    for (size_t i = 2400; i < static_cast<size_t>(total); ++i) worst = std::max(worst, std::fabs(out[i] - out[i - 1]));
    CHECK(worst <= bound * 1.15f);
    // And it does arrive: the end is the boosted tone.
    float end_peak = 0, ref_peak = 0;
    for (int i = total - 480; i < total; ++i) {
        end_peak = std::max(end_peak, std::fabs(out[static_cast<size_t>(i)]));
        ref_peak = std::max(ref_peak, std::fabs(reference[static_cast<size_t>(i)]));
    }
    CHECK(std::fabs(end_peak - ref_peak) < 0.05f * ref_peak);
}

void settings_off_again_returns_to_the_identity_through_a_ramp() {
    Processor processor;
    processor.set(equalizer(2, 1500), kRate);
    std::vector<float> samples(4800);
    for (size_t i = 0; i < samples.size(); ++i) {
        samples[i] = static_cast<float>(0.1 * std::sin(2 * kPi * 910 * static_cast<double>(i) / kRate));
    }
    std::vector<float> original(samples);
    processor.process(samples.data(), 1200, 1);
    Settings off;
    processor.set(off, kRate);
    processor.process(samples.data() + 1200, 3600, 1);
    float diff = 0;
    for (size_t i = 4000; i < 4800; ++i) diff = std::max(diff, std::fabs(samples[i] - original[i]));
    CHECK(diff < 1e-3f);
}

void the_limiter_comes_in_and_goes_out_with_the_ramp_of_the_gain() {
    // A steady 0.95 (above the limiter's knee) while the loudness effect is switched on at 0 mB and off again: the
    // output moves from 0.95 to its limited value and back over the ramp, with no step of its own.
    Settings on;
    on.loudness = true;
    on.loudness_millibel = 0;
    Processor processor;
    processor.set(Settings{}, kRate);
    std::vector<float> samples(2000, 0.95f);
    processor.process(samples.data(), 100, 1);  // settled, off: untouched
    CHECK(std::fabs(samples[99] - 0.95f) < 1e-6f);
    processor.set(on, kRate);
    std::vector<float> turning_on(2000, 0.95f);
    processor.process(turning_on.data(), 2000, 1);
    float worst = std::fabs(turning_on[0] - 0.95f);
    for (size_t i = 1; i < turning_on.size(); ++i) {
        worst = std::max(worst, std::fabs(turning_on[i] - turning_on[i - 1]));
    }
    CHECK(worst < 0.002f);
    const float limited = turning_on.back();
    CHECK(limited < 0.94f && limited > 0.9f);  // in the end the limiter is on
    processor.set(Settings{}, kRate);
    std::vector<float> turning_off(2000, 0.95f);
    processor.process(turning_off.data(), 2000, 1);
    worst = std::fabs(turning_off[0] - limited);
    for (size_t i = 1; i < turning_off.size(); ++i) {
        worst = std::max(worst, std::fabs(turning_off[i] - turning_off[i - 1]));
    }
    CHECK(worst < 0.002f);
    CHECK(std::fabs(turning_off.back() - 0.95f) < 1e-6f);
}

}  // namespace

int main() {
    the_equalizer_gains_match_the_measured_points();
    what_is_off_is_the_identity();
    the_bass_boost_follows_the_response_of_androids_dynamic_bass_enhancement();
    the_virtualizer_follows_the_response_of_androids_stereo_widening();
    the_virtualizer_at_strength_zero_leaves_every_signal_alone();
    a_mono_stream_hears_the_virtualizers_mid_path();
    the_loudness_gain_is_linear_and_limited();
    a_change_of_settings_ramps_without_a_click();
    settings_off_again_returns_to_the_identity_through_a_ramp();
    the_limiter_comes_in_and_goes_out_with_the_ramp_of_the_gain();
    std::printf("audio_effects_test: %s\n", failures == 0 ? "all fine" : "failed");
    return failures == 0 ? 0 : 1;
}
