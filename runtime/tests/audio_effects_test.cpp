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
    double peak = 0;
    for (int frame = total - total / 5; frame < total; ++frame) {
        peak = std::max<double>(peak, std::fabs(samples[static_cast<size_t>(frame * channels + channel_measured)]));
    }
    return 20 * std::log10(peak / amplitude);
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

void the_bass_boost_raises_the_low_end_more_than_the_high() {
    Settings settings;
    settings.bass_boost = true;
    settings.bass_strength = 1000;
    const double low = gain_db(settings, 40);
    const double high = gain_db(settings, 3000);
    CHECK(low > 9.0 && low < 16.0);
    CHECK(std::fabs(high) < 1.0);
    settings.bass_strength = 500;
    const double half = gain_db(settings, 40);
    CHECK(half > 2.0 && half < low - 1.0);
    settings.bass_strength = 0;
    CHECK(std::fabs(gain_db(settings, 40)) < 0.2);
}

void the_virtualizer_widens_stereo_and_leaves_mono_alone() {
    Settings settings;
    settings.virtualizer = true;
    settings.virtualizer_strength = 1000;
    CHECK(std::fabs(gain_db(settings, 1000, 2, false)) < 0.2);  // both channels equal: no side signal
    CHECK(std::fabs(gain_db(settings, 1000, 1)) < 0.2);          // a mono stream is not widened
    const double left = gain_db(settings, 1000, 2, true, 0.1, 0);
    const double right = gain_db(settings, 1000, 2, true, 0.1, 1);
    CHECK(left > 3.0 && left < 4.0);    // a left-only tone is louder on the left: 1.5 times at the factor 2
    CHECK(right > -7.0 && right < -5.0);  // and bleeds, in anti-phase, onto the right at half
    settings.virtualizer_strength = 0;
    CHECK(std::isinf(gain_db(settings, 1000, 2, true, 0.1, 1)));  // strength 0: the right channel stays silent
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

}  // namespace

int main() {
    the_equalizer_gains_match_the_measured_points();
    what_is_off_is_the_identity();
    the_bass_boost_raises_the_low_end_more_than_the_high();
    the_virtualizer_widens_stereo_and_leaves_mono_alone();
    the_loudness_gain_is_linear_and_limited();
    a_change_of_settings_ramps_without_a_click();
    settings_off_again_returns_to_the_identity_through_a_ramp();
    std::printf("audio_effects_test: %s\n", failures == 0 ? "all fine" : "failed");
    return failures == 0 ? 0 : 1;
}
