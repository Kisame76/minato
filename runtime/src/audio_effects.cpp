#include "audio_effects.h"

#include <algorithm>
#include <cmath>

namespace apkrt::effects {
namespace {

constexpr double kPi = 3.14159265358979323846;
// Android's effect bundle (frameworks/av, AndroidEffectBundle.h): five bands with Q 0.96 (EQNB_5BandPresetsQFactors),
// bass enhancement centred on 90 Hz with up to 15 dB (BE_EffectLevel 0..15). The emulator gives no response to measure.
constexpr float kBandQ = 0.96f;
constexpr float kBassCorner = 90.0f;         // the bundle's bass enhancement centre
constexpr float kBassMaxDecibel = 15.0f;     // the gain at strength 1000
constexpr float kWidthMax = 2.0f;            // an approximation: the side factor at strength 1000

Biquad peaking(float centre, float rate, float decibel, float q) {
    const double a = std::pow(10.0, decibel / 40.0);
    const double w0 = 2 * kPi * centre / rate;
    const double alpha = std::sin(w0) / (2 * q);
    const double cosine = std::cos(w0);
    const double a0 = 1 + alpha / a;
    Biquad result;
    result.b0 = static_cast<float>((1 + alpha * a) / a0);
    result.b1 = static_cast<float>((-2 * cosine) / a0);
    result.b2 = static_cast<float>((1 - alpha * a) / a0);
    result.a1 = static_cast<float>((-2 * cosine) / a0);
    result.a2 = static_cast<float>((1 - alpha / a) / a0);
    return result;
}

Biquad low_shelf(float corner, float rate, float decibel) {
    const double a = std::pow(10.0, decibel / 40.0);
    const double w0 = 2 * kPi * corner / rate;
    const double cosine = std::cos(w0);
    const double alpha = std::sin(w0) / 2 * std::sqrt(2.0);  // shelf slope 1
    const double root = 2 * std::sqrt(a) * alpha;
    const double a0 = (a + 1) + (a - 1) * cosine + root;
    Biquad result;
    result.b0 = static_cast<float>(a * ((a + 1) - (a - 1) * cosine + root) / a0);
    result.b1 = static_cast<float>(2 * a * ((a - 1) - (a + 1) * cosine) / a0);
    result.b2 = static_cast<float>(a * ((a + 1) - (a - 1) * cosine - root) / a0);
    result.a1 = static_cast<float>(-2 * ((a - 1) + (a + 1) * cosine) / a0);
    result.a2 = static_cast<float>(((a + 1) + (a - 1) * cosine - root) / a0);
    return result;
}

float soft_limit(float x) {
    const float knee = 0.8f;
    const float magnitude = std::fabs(x);
    if (magnitude <= knee) return x;
    const float limited = knee + (1 - knee) * std::tanh((magnitude - knee) / (1 - knee));
    return x < 0 ? -limited : limited;
}

}  // namespace

bool Settings::operator==(const Settings &other) const {
    if (equalizer != other.equalizer || bass_boost != other.bass_boost || virtualizer != other.virtualizer ||
        loudness != other.loudness || bass_strength != other.bass_strength ||
        virtualizer_strength != other.virtualizer_strength || loudness_millibel != other.loudness_millibel) {
        return false;
    }
    for (int band = 0; band < kBands; ++band) {
        if (band_millibel[band] != other.band_millibel[band]) return false;
    }
    return true;
}

Chain make_chain(const Settings &settings, float rate) {
    Chain chain;
    if (settings.equalizer) {
        for (int band = 0; band < kBands; ++band) {
            if (kBandCentres[band] > 0.45f * rate) continue;
            const float decibel = static_cast<float>(std::clamp(settings.band_millibel[band], -1500, 1500)) / 100.0f;
            chain.band[band] = peaking(kBandCentres[band], rate, decibel, kBandQ);
        }
    }
    if (settings.bass_boost && kBassCorner < 0.45f * rate) {
        const float strength = static_cast<float>(std::clamp(settings.bass_strength, 0, 1000)) / 1000.0f;
        chain.bass = low_shelf(kBassCorner, rate, strength * kBassMaxDecibel);
    }
    if (settings.virtualizer) {
        const float strength = static_cast<float>(std::clamp(settings.virtualizer_strength, 0, 1000)) / 1000.0f;
        chain.width = 1.0f + strength * (kWidthMax - 1.0f);
    }
    if (settings.loudness) {
        chain.gain = static_cast<float>(std::pow(10.0, std::clamp(settings.loudness_millibel, 0, 2000) / 2000.0));
        chain.limit = true;
    }
    return chain;
}

float Processor::run(const Biquad &q, Delay &d, float x) {
    const float y = q.b0 * x + d.z1;  // transposed direct form II
    d.z1 = q.b1 * x - q.a1 * y + d.z2;
    d.z2 = q.b2 * x - q.a2 * y;
    return y;
}

Biquad Processor::blend(const Biquad &from, const Biquad &to, float t) {
    Biquad result;
    result.b0 = from.b0 + (to.b0 - from.b0) * t;
    result.b1 = from.b1 + (to.b1 - from.b1) * t;
    result.b2 = from.b2 + (to.b2 - from.b2) * t;
    result.a1 = from.a1 + (to.a1 - from.a1) * t;
    result.a2 = from.a2 + (to.a2 - from.a2) * t;
    return result;
}

void Processor::set(const Settings &settings, float rate) {
    if (have_ && settings == settings_) return;
    Chain target = make_chain(settings, rate);
    if (!have_) {
        from_ = target;
        to_ = target;
        ramp_left_ = 0;
        have_ = true;
    } else {
        // The ramp starts where it is: the chain now in use becomes the one to leave.
        if (ramp_left_ > 0) {
            const float t = 1.0f - static_cast<float>(ramp_left_) / kRampFrames;
            Chain now;
            for (int band = 0; band < kBands; ++band) now.band[band] = blend(from_.band[band], to_.band[band], t);
            now.bass = blend(from_.bass, to_.bass, t);
            now.width = from_.width + (to_.width - from_.width) * t;
            now.gain = from_.gain + (to_.gain - from_.gain) * t;
            now.limit = from_.limit || to_.limit;
            from_ = now;
        } else {
            from_ = to_;
        }
        to_ = target;
        ramp_left_ = kRampFrames;
    }
    if (!active_ && settings.any()) reset();  // from idle: no stale delay lines
    settings_ = settings;
    active_ = settings.any();
}

void Processor::reset() {
    for (auto &channel : band_) {
        for (auto &delay : channel) delay = Delay{};
    }
    for (auto &delay : bass_) delay = Delay{};
}

void Processor::process(float *samples, std::int64_t frames, int channels) {
    if (idle() || channels < 1 || channels > kMaxChannels) return;
    const bool widen = channels == 2;
    for (std::int64_t frame = 0; frame < frames; ++frame) {
        float t = 1.0f;
        if (ramp_left_ > 0) {
            t = 1.0f - static_cast<float>(ramp_left_) / kRampFrames;
            --ramp_left_;
        }
        const bool moving = t < 1.0f;
        Biquad band[kBands];
        Biquad bass;
        float width, gain;
        const bool limit = from_.limit || to_.limit;
        if (moving) {
            for (int b = 0; b < kBands; ++b) band[b] = blend(from_.band[b], to_.band[b], t);
            bass = blend(from_.bass, to_.bass, t);
            width = from_.width + (to_.width - from_.width) * t;
            gain = from_.gain + (to_.gain - from_.gain) * t;
        } else {
            for (int b = 0; b < kBands; ++b) band[b] = to_.band[b];
            bass = to_.bass;
            width = to_.width;
            gain = to_.gain;
        }
        float *at = samples + frame * channels;
        for (int channel = 0; channel < channels; ++channel) {
            float x = at[channel];
            for (int b = 0; b < kBands; ++b) x = run(band[b], band_[channel][b], x);
            x = run(bass, bass_[channel], x);
            at[channel] = x;
        }
        if (widen && width != 1.0f) {
            const float mid = 0.5f * (at[0] + at[1]);
            const float side = 0.5f * (at[0] - at[1]) * width;
            at[0] = mid + side;
            at[1] = mid - side;
        }
        if (limit || gain != 1.0f) {
            for (int channel = 0; channel < channels; ++channel) {
                at[channel] = limit ? soft_limit(at[channel] * gain) : at[channel] * gain;
            }
        }
    }
    if (ramp_left_ == 0) from_ = to_;
}

}  // namespace apkrt::effects
