// The audio effects of the runtime's mix: Android's Equalizer,
// Dynamic Bass Boost, Virtualizer and Loudness Enhancer as plain DSP on a stream's frames, after the guest's samples
// are copied out in the render block and before the tap, so that the Visualizer sees them. Settings are per audio
// session (the framework's effect service sets them through audio_effect): Settings is what a session has, a Processor
// is one stream's filter with its own delay lines, which makes the coefficients for the stream's rate and moves to a
// new setting over a short ramp, so that a change does not click. Nothing here locks, allocates or calls the system:
// the render block runs it on the audio thread. The shapes are RBJ biquads with the effect bundle's band centres, Q and
// bass centre: an approximation of Android's effects, not bit for bit. fixtures/apps/audiofx-response-test prints the
// response of the effects as an app sees it at the Visualizer.
#pragma once

#include <cstdint>

namespace apkrt::effects {

constexpr int kBands = 5;
constexpr int kMaxChannels = 8;

/// The centre frequencies of the Equalizer's bands, as the bundle has them.
constexpr float kBandCentres[kBands] = {60.0f, 230.0f, 910.0f, 3600.0f, 14000.0f};

/// What a session has set: the effects that are on and their parameters, in the units of Android's API.
struct Settings {
    bool equalizer = false;
    std::int32_t band_millibel[kBands] = {0, 0, 0, 0, 0};  // -1500 to 1500
    bool bass_boost = false;
    std::int32_t bass_strength = 0;  // 0 to 1000
    bool virtualizer = false;
    std::int32_t virtualizer_strength = 0;  // 0 to 1000
    bool loudness = false;
    std::int32_t loudness_millibel = 0;  // the target gain

    bool any() const { return equalizer || bass_boost || virtualizer || loudness; }
    bool operator==(const Settings &other) const;
};

/// A biquad's coefficients, a0 normalized to 1.
struct Biquad {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
};

/// The coefficients of Settings at one rate: identity for what is off, so that a ramp from or to it works the same.
struct Chain {
    Biquad band[kBands];
    Biquad bass_hp, bass_bp;  // the bass enhancement's high-pass under the signal and band peak around 90 Hz
    float bass_gain = 0;      // the peak's gain G; 0 while the bass boost is off
    bool bass_on = false;
    // The virtualizer (LVM's stereo widening for headphones): mid and side each through a filter of their own and a
    // shared equaliser, mixed with the unprocessed signal by `virt`. The filters are always the same, so that a ramp to
    // or from off only moves the mix.
    Biquad virt_mid, virt_side, virt_equaliser;
    float virt = 0;  // the processed share, strength / 1000; 0 while the virtualizer is off
    bool virt_on = false;
    float gain = 1;    // the loudness enhancer's linear gain
    float limit = 0;   // the soft limiter's share after the gain: 0 off, 1 on, between while it ramps
};

/// The coefficients for `settings` at `rate` Hz. A band above 45% of the rate is left out.
Chain make_chain(const Settings &settings, float rate);

/// One stream's effect chain.
class Processor {
public:
    /// How many frames a change of settings takes to arrive: 10 ms at 48 kHz.
    static constexpr int kRampFrames = 480;

    Processor() = default;

    /// The settings the next frames use; the first call sets them at once, later ones move there over kRampFrames.
    void set(const Settings &settings, float rate);

    /// Resets the delay lines (a stream that started again), keeping the settings.
    void reset();

    /// True when the settings change nothing, so that process() may be skipped.
    bool idle() const { return !active_ && ramp_left_ == 0; }

    /// Processes `frames` interleaved frames of `channels` channels in place; a mono stream gets the virtualizer's mid
    /// path.
    void process(float *samples, std::int64_t frames, int channels);

private:
    struct Delay {
        float z1 = 0, z2 = 0;
    };

    static float run(const Biquad &q, Delay &d, float x);
    static Biquad blend(const Biquad &from, const Biquad &to, float t);

    Settings settings_;
    bool have_ = false;
    bool active_ = false;  // the target chain changes something
    Chain from_, to_;
    int ramp_left_ = 0;
    Delay band_[kMaxChannels][kBands];
    Delay bass_hp_[kMaxChannels];
    Delay bass_bp_;
    Delay virt_mid_, virt_mid_eq_, virt_side_, virt_side_eq_;
};

}  // namespace apkrt::effects
