#include "audio_effects.h"

#include <algorithm>
#include <cmath>

namespace apkrt::effects {
namespace {

constexpr double kPi = 3.14159265358979323846;
// Android's effect bundle (frameworks/av, AndroidEffectBundle.h): five bands with Q 0.96 (EQNB_5BandPresetsQFactors).
// The emulator gives no response to measure.
constexpr float kBandQ = 0.96f;
// Android's dynamic bass enhancement (LVM, android-15.0.0_r1): out = HPF(x) + G * BPF(mean of HPF), a second-order
// high-pass at 90 Hz (Q 0.708) under the whole signal and a band peak at 90 Hz (Q 1.5) of gain G(level), level =
// 15 * strength / 1000. The two filters are made at any rate; kBassScale is LVM's -0.1 dB in its coefficients.
constexpr float kBassCentre = 90.0f;
constexpr float kBassHighPassQ = 0.7079f;
constexpr float kBassBandQ = 1.5f;
constexpr float kBassScale = 0.9886f;
constexpr float kBassGain[16] = {0.412538f, 0.584893f, 0.778279f, 0.995262f, 1.238721f, 1.511886f,
                                 1.818383f, 2.162278f, 2.548134f, 2.981072f, 3.466836f, 4.011872f,
                                 4.623413f, 5.309573f, 6.079458f, 6.943282f};
// Android's virtualizer (LVM's LVCS for headphones, android-15.0.0_r1, LVCS_Headphone_Coeffs.h at 48 kHz): the mid
// signal (L + R) / 2 and the side signal (L - R) / 2 go through a first-order filter and a second-order filter, both
// through the same equaliser, and are mixed with the unprocessed ones by the strength: out = (1 - t) x + t K P(x),
// K = LVCS's processed gain over its unprocessed gain. The reverb and the stereo delay of the bundle are not modelled.
struct Coefficients {
    double a[3], b[2];
};
constexpr Coefficients kVirtMid = {{0.234445, -0.232261, 0.0}, {-0.986713, 0.0}};
constexpr Coefficients kVirtSide = {{0.272606, -0.266952, -0.005654}, {-1.617141, 0.630405}};
constexpr Coefficients kVirtEqualiser = {{1.641177, -2.364687, 0.759910}, {-1.166774, 0.178074}};
constexpr float kVirtGain = 0.8477735f / 0.2087465f;

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

Biquad high_pass(float centre, float rate, float q, float scale) {
    const double w0 = 2 * kPi * centre / rate;
    const double alpha = std::sin(w0) / (2 * q);
    const double cosine = std::cos(w0);
    const double a0 = 1 + alpha;
    Biquad result;
    result.b0 = static_cast<float>((1 + cosine) / 2 / a0 * scale);
    result.b1 = static_cast<float>(-(1 + cosine) / a0 * scale);
    result.b2 = result.b0;
    result.a1 = static_cast<float>(-2 * cosine / a0);
    result.a2 = static_cast<float>((1 - alpha) / a0);
    return result;
}

Biquad band_pass(float centre, float rate, float q, float scale) {
    const double w0 = 2 * kPi * centre / rate;
    const double alpha = std::sin(w0) / (2 * q);
    const double cosine = std::cos(w0);
    const double a0 = 1 + alpha;
    Biquad result;
    result.b0 = static_cast<float>(alpha / a0 * scale);
    result.b1 = 0;
    result.b2 = -result.b0;
    result.a1 = static_cast<float>(-2 * cosine / a0);
    result.a2 = static_cast<float>((1 - alpha) / a0);
    return result;
}

/// A filter given at 48 kHz, made for `rate`: through its analog equivalent (the bilinear transform without
/// pre-warping), so that its response at the same frequencies stays close to the original at the rates near 48 kHz.
Biquad retarget(const Coefficients &c, float rate) {
    const double k = rate / 48000.0;  // u = s / (2 fs) at 48 kHz is k times u at `rate`
    const double n0 = c.a[0] + c.a[1] + c.a[2], n1 = (2 * c.a[0] - 2 * c.a[2]) * k;
    const double n2 = (c.a[0] - c.a[1] + c.a[2]) * k * k;
    const double d0 = 1 + c.b[0] + c.b[1], d1 = (2 - 2 * c.b[1]) * k, d2 = (1 - c.b[0] + c.b[1]) * k * k;
    const double nz0 = n0 + n1 + n2, nz1 = 2 * n0 - 2 * n2, nz2 = n0 - n1 + n2;
    const double dz0 = d0 + d1 + d2, dz1 = 2 * d0 - 2 * d2, dz2 = d0 - d1 + d2;
    Biquad result;
    result.b0 = static_cast<float>(nz0 / dz0);
    result.b1 = static_cast<float>(nz1 / dz0);
    result.b2 = static_cast<float>(nz2 / dz0);
    result.a1 = static_cast<float>(dz1 / dz0);
    result.a2 = static_cast<float>(dz2 / dz0);
    return result;
}

float soft_limit(float x) {
    const float knee = 0.8f;
    const float magnitude = std::fabs(x);
    if (magnitude <= knee) return x;
    const float limited = knee + (1 - knee) * std::tanh((magnitude - knee) / (1 - knee));
    return x < 0 ? -limited : limited;
}

// The reverb (see Reverb in the header). Android's level is the sum of room and reverb level less the maximum of
// 2000 mB (EffectReverb.cpp, ReverbSetReverbLevel and ReverbSetRoomLevel), -12000 mB and below is silent, the send is
// 0.75 (REVERB_SEND_LEVEL); the room HF level is the wet signal's high cut; the decay HF ratio is given at 5 kHz.
constexpr int kLineBase[4] = {1031, 1327, 1721, 2113};  // prime frames at 48 kHz
constexpr int kDiffuserBase[2] = {142, 107};
constexpr float kSend = 0.75f;
constexpr float kDampingHertz = 5000.0f;

}  // namespace

void Reverb::clear() {
    for (auto &line : line_) std::fill(std::begin(line), std::end(line), 0.0f);
    for (auto &line : diffuser_) std::fill(std::begin(line), std::end(line), 0.0f);
    for (int i = 0; i < 4; ++i) at_[i] = 0, low_[i] = 0;
    diffuser_at_[0] = diffuser_at_[1] = 0;
    cut_[0] = cut_[1] = 0;
}

void Reverb::layout(const ReverbSettings &settings, float rate) {
    // A denser room has the shorter lines, down to half the length at density 0.
    const double density = std::clamp(settings.density, 0, 1000) / 1000.0;
    const double scale = std::min<double>(rate / 48000.0, 3.9) * (0.5 + 0.5 * density);
    const double time = std::max(settings.decay_time, 100) / 1000.0;
    const double ratio = std::clamp(settings.decay_hf_ratio, 100, 2000) / 1000.0;
    const double w = 2 * kPi * kDampingHertz / rate;
    for (int i = 0; i < 4; ++i) {
        length_[i] = std::clamp(static_cast<int>(kLineBase[i] * scale), 16, kMaxLine);
        // the loop gain for the decay time: 60 dB in `time`, and at 5 kHz in time * ratio
        const double db = -3.0 * length_[i] / (rate * time);  // log10 of the loop gain at DC
        gain_[i] = static_cast<float>(std::pow(10.0, db));
        const double h = std::pow(10.0, db * (1.0 / std::min(ratio, 1.0) - 1.0));  // |low-pass| needed at 5 kHz
        double p = 0;
        if (h < 0.999) {
            const double b = 2 - 2 * h * h * std::cos(w), a = 1 - h * h;
            p = (b - std::sqrt(std::max(b * b - 4 * a * a, 0.0))) / (2 * a);
        }
        damp_[i] = static_cast<float>(std::clamp(p, 0.0, 0.999));
    }
    for (int i = 0; i < 2; ++i) {
        const double rate_scale = std::min<double>(rate / 48000.0, 3.9);
        diffuser_length_[i] = std::clamp(static_cast<int>(kDiffuserBase[i] * rate_scale), 8, 1024);
    }
    diffuse_ = 0.7f * static_cast<float>(std::clamp(settings.diffusion, 0, 1000)) / 1000.0f;
    // the room HF level cuts the wet signal's highs: -9000 mB at most, a one-pole at 8 kHz at its deepest
    const double hf = std::clamp(settings.room_hf_level, -9000, 0) / 9000.0;  // 0 to -1
    const double cut_hertz = 20000.0 * std::pow(0.4, -hf);                   // 20 kHz down to 8 kHz
    high_cut_ = static_cast<float>(std::exp(-2 * kPi * std::min<double>(cut_hertz, 0.45 * rate) / rate));
}

void Reverb::set(bool on, const ReverbSettings &settings, float rate, bool at_once) {
    const bool was_idle = idle();
    const bool relayout = rate != rate_ || settings.decay_time != settings_.decay_time ||
                          settings.decay_hf_ratio != settings_.decay_hf_ratio ||
                          settings.density != settings_.density || settings.diffusion != settings_.diffusion ||
                          settings.room_hf_level != settings_.room_hf_level;
    const bool density_changes = rate != rate_ || settings.density != settings_.density;
    settings_ = settings;
    rate_ = rate;
    if (on) {
        if (relayout || was_idle) layout(settings, rate);
        if (was_idle || density_changes) clear();  // new line lengths, or a tail from before: start from silence
        const int combined = settings.reverb_level + settings.room_level - 2000;
        wet_target_ = combined <= -12000 ? 0.0f : static_cast<float>(std::pow(10.0, combined / 2000.0)) * kSend;
        if (at_once) wet_ = wet_target_;
    } else {
        wet_target_ = 0;
        if (at_once) wet_ = 0;
    }
    on_ = on;
}

void Reverb::process_frame(float *at, int channels) {
    if (idle()) return;
    // the wet gain moves to its target by a share of the distance: about 2 ms for most of it
    wet_ += (wet_target_ - wet_) * 0.01f;
    float input = channels >= 2 ? 0.5f * (at[0] + at[1]) : at[0];
    for (int i = 0; i < 2; ++i) {  // Schroeder allpass: y = -g x + d, d' = x + g y
        float &slot = diffuser_[i][diffuser_at_[i]];
        const float out = -diffuse_ * input + slot;
        slot = input + diffuse_ * out;
        if (++diffuser_at_[i] >= diffuser_length_[i]) diffuser_at_[i] = 0;
        input = out;
    }
    float tap[4];
    for (int i = 0; i < 4; ++i) tap[i] = line_[i][at_[i]];
    // Hadamard feedback: the sums of the lines, orthogonal at 0.5
    const float mixed[4] = {0.5f * (tap[0] + tap[1] + tap[2] + tap[3]), 0.5f * (tap[0] - tap[1] + tap[2] - tap[3]),
                            0.5f * (tap[0] + tap[1] - tap[2] - tap[3]), 0.5f * (tap[0] - tap[1] - tap[2] + tap[3])};
    for (int i = 0; i < 4; ++i) {
        // the loop: gain, then the damping low-pass
        const float fed = gain_[i] * (mixed[i] + 0.5f * input);
        low_[i] = (1 - damp_[i]) * fed + damp_[i] * low_[i];
        line_[i][at_[i]] = low_[i];
        if (++at_[i] >= length_[i]) at_[i] = 0;
    }
    float left = 0.5f * (tap[0] + tap[1] - tap[2] - tap[3]);
    float right = 0.5f * (tap[0] - tap[1] + tap[2] - tap[3]);
    left = cut_[0] = (1 - high_cut_) * left + high_cut_ * cut_[0];
    right = cut_[1] = (1 - high_cut_) * right + high_cut_ * cut_[1];
    if (channels >= 2) {
        at[0] += wet_ * left;
        at[1] += wet_ * right;
    } else {
        at[0] += wet_ * 0.5f * (left + right);
    }
}

bool Settings::operator==(const Settings &other) const {
    if (equalizer != other.equalizer || bass_boost != other.bass_boost || virtualizer != other.virtualizer ||
        loudness != other.loudness || bass_strength != other.bass_strength ||
        virtualizer_strength != other.virtualizer_strength || loudness_millibel != other.loudness_millibel) {
        return false;
    }
    for (int band = 0; band < kBands; ++band) {
        if (band_millibel[band] != other.band_millibel[band]) return false;
    }
    return reverb == other.reverb && (!reverb || reverb_params == other.reverb_params);
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
    if (settings.bass_boost && kBassCentre < 0.45f * rate) {
        const int level = std::clamp(settings.bass_strength, 0, 1000) * 15 / 1000;
        chain.bass_hp = high_pass(kBassCentre, rate, kBassHighPassQ, kBassScale);
        chain.bass_bp = band_pass(kBassCentre, rate, kBassBandQ, kBassScale);
        chain.bass_gain = kBassGain[level];
        chain.bass_on = true;
    }
    chain.virt_mid = retarget(kVirtMid, rate);
    chain.virt_side = retarget(kVirtSide, rate);
    chain.virt_equaliser = retarget(kVirtEqualiser, rate);
    if (settings.virtualizer) {
        chain.virt = static_cast<float>(std::clamp(settings.virtualizer_strength, 0, 1000)) / 1000.0f;
        chain.virt_on = true;
    }
    if (settings.loudness) {
        chain.gain = static_cast<float>(std::pow(10.0, std::clamp(settings.loudness_millibel, 0, 2000) / 2000.0));
        chain.limit = 1.0f;
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
    const bool had = have_;
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
            now.bass_hp = blend(from_.bass_hp, to_.bass_hp, t);
            now.bass_bp = blend(from_.bass_bp, to_.bass_bp, t);
            now.bass_gain = from_.bass_gain + (to_.bass_gain - from_.bass_gain) * t;
            now.bass_on = from_.bass_on || to_.bass_on;
            now.virt_mid = to_.virt_mid;  // the same filters either way; only the mix moves
            now.virt_side = to_.virt_side;
            now.virt_equaliser = to_.virt_equaliser;
            now.virt = from_.virt + (to_.virt - from_.virt) * t;
            now.virt_on = from_.virt_on || to_.virt_on;
            now.gain = from_.gain + (to_.gain - from_.gain) * t;
            now.limit = from_.limit + (to_.limit - from_.limit) * t;
            from_ = now;
        } else {
            from_ = to_;
        }
        to_ = target;
        ramp_left_ = kRampFrames;
    }
    if (!active_ && settings.any()) reset();  // from idle: no stale delay lines
    reverb_.set(settings.reverb, settings.reverb_params, rate, !had);
    settings_ = settings;
    active_ = settings.any();
}

void Processor::reset() {
    for (auto &channel : band_) {
        for (auto &delay : channel) delay = Delay{};
    }
    for (auto &delay : bass_hp_) delay = Delay{};
    bass_bp_ = Delay{};
    virt_mid_ = virt_mid_eq_ = virt_side_ = virt_side_eq_ = Delay{};
}

void Processor::process(float *samples, std::int64_t frames, int channels) {
    if (idle() || channels < 1 || channels > kMaxChannels) return;
    for (std::int64_t frame = 0; frame < frames; ++frame) {
        float t = 1.0f;
        if (ramp_left_ > 0) {
            t = 1.0f - static_cast<float>(ramp_left_) / kRampFrames;
            --ramp_left_;
        }
        const bool moving = t < 1.0f;
        Biquad band[kBands];
        Biquad bass_hp, bass_bp;
        float virt, gain, bass_gain, limit;
        const bool bass = from_.bass_on || to_.bass_on;
        const bool virtualizing = from_.virt_on || to_.virt_on;
        Biquad virt_mid = to_.virt_mid, virt_side = to_.virt_side, virt_equaliser = to_.virt_equaliser;
        if (moving) {
            for (int b = 0; b < kBands; ++b) band[b] = blend(from_.band[b], to_.band[b], t);
            bass_hp = blend(from_.bass_hp, to_.bass_hp, t);
            bass_bp = blend(from_.bass_bp, to_.bass_bp, t);
            bass_gain = from_.bass_gain + (to_.bass_gain - from_.bass_gain) * t;
            virt = from_.virt + (to_.virt - from_.virt) * t;
            gain = from_.gain + (to_.gain - from_.gain) * t;
            limit = from_.limit + (to_.limit - from_.limit) * t;
        } else {
            for (int b = 0; b < kBands; ++b) band[b] = to_.band[b];
            bass_hp = to_.bass_hp;
            bass_bp = to_.bass_bp;
            bass_gain = to_.bass_gain;
            virt = to_.virt;
            gain = to_.gain;
            limit = to_.limit;
        }
        float *at = samples + frame * channels;
        for (int channel = 0; channel < channels; ++channel) {
            float x = at[channel];
            for (int b = 0; b < kBands; ++b) x = run(band[b], band_[channel][b], x);
            if (bass) x = run(bass_hp, bass_hp_[channel], x);
            at[channel] = x;
        }
        if (bass) {  // the peak of the mean of the high-passed channels, added to each
            float mean = 0;
            for (int channel = 0; channel < channels; ++channel) mean += at[channel];
            const float peak = bass_gain * run(bass_bp, bass_bp_, mean / static_cast<float>(channels));
            for (int channel = 0; channel < channels; ++channel) at[channel] += peak;
        }
        if (virtualizing && channels <= 2) {
            // Mid and side through their paths, mixed with themselves by the strength; a mono stream is its mid.
            const float mid = channels == 2 ? 0.5f * (at[0] + at[1]) : at[0];
            const float processed_mid = run(virt_equaliser, virt_mid_eq_, run(virt_mid, virt_mid_, mid));
            const float mid_out = (1 - virt) * mid + virt * kVirtGain * processed_mid;
            if (channels == 2) {
                const float side = 0.5f * (at[0] - at[1]);
                const float processed_side = run(virt_equaliser, virt_side_eq_, run(virt_side, virt_side_, side));
                const float side_out = (1 - virt) * side + virt * kVirtGain * processed_side;
                at[0] = mid_out + side_out;
                at[1] = mid_out - side_out;
            } else {
                at[0] = mid_out;
            }
        }
        if (limit > 0.0f || gain != 1.0f) {
            for (int channel = 0; channel < channels; ++channel) {
                const float loud = at[channel] * gain;
                at[channel] = loud + limit * (soft_limit(loud) - loud);  // the limiter's share ramps with the gain
            }
        }
        reverb_.process_frame(at, channels);
    }
    if (ramp_left_ == 0) from_ = to_;
}

}  // namespace apkrt::effects
