// Ours (the audio effects plan): the state and the answers of one android.media.audiofx.Visualizer, as Android 15's
// engine gives them (measured by visualizer-test): the capture size range, the defaults, which calls need the
// Visualizer disabled and which enabled, the codes for a value out of range, and the data of the capture. The data is
// what the output mix carries, given as `samples`: the capture size's last mono frames at 48 kHz, which the runtime's
// audio_tap gives (none means silence). The waveform is 8-bit unsigned, so 128 is zero level; the FFT is the real and
// imaginary parts of n/2 + 1 points as signed bytes, 127 for a full-scale sine at its bin; the peak and RMS are in mB.
// Plain C++ without JNI, so that framework/tests/native/visualizer_state_test.cpp runs it on the host.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace minato::visualizer {

// Visualizer.java's status codes.
constexpr int kSuccess = 0, kError = -1, kAlreadyExists = -2, kNoInit = -3, kBadValue = -4, kInvalidOperation = -5;

constexpr int kMinCaptureSize = 128, kMaxCaptureSize = 1024, kMaxCaptureRate = 20000;  // the rate in mHz
constexpr int kSamplingRate = 48000000;                                                // mHz
constexpr int kSilenceMillibel = -9600;

struct State {
    bool enabled = false;
    int capture_size = 1024;
    int scaling_mode = 0;      // SCALING_MODE_NORMALIZED
    int measurement_mode = 0;  // MEASUREMENT_MODE_NONE
    int rate = 0;              // the periodic capture's, in mHz
    bool periodic_waveform = false;
    bool periodic_fft = false;

    int set_enabled(bool value) {
        enabled = value;
        return kSuccess;
    }

    /// Only while disabled; a power of two in the range.
    int set_capture_size(int size) {
        if (enabled) return kInvalidOperation;
        if (size < kMinCaptureSize || size > kMaxCaptureSize || (size & (size - 1)) != 0) return kBadValue;
        capture_size = size;
        return kSuccess;
    }

    int set_scaling_mode(int mode) {
        if (mode != 0 && mode != 1) return kBadValue;
        scaling_mode = mode;
        return kSuccess;
    }

    int set_measurement_mode(int mode) {
        if (mode != 0 && mode != 1) return kBadValue;
        measurement_mode = mode;
        return kSuccess;
    }

    /// The periodic capture can only be changed while disabled; a rate above the maximum is refused.
    int set_periodic_capture(int new_rate, bool waveform, bool fft) {
        if (enabled) return kInvalidOperation;
        if (new_rate < 0 || new_rate > kMaxCaptureRate) return kBadValue;
        rate = new_rate;
        periodic_waveform = waveform;
        periodic_fft = fft;
        return kSuccess;
    }

    bool periodic() const { return rate > 0 && (periodic_waveform || periodic_fft); }

    /// The waveform: capture_size unsigned bytes round 128; the array must hold them.
    int get_waveform(std::vector<uint8_t> &out, size_t room, const std::vector<float> *samples = nullptr) const {
        if (!enabled) return kInvalidOperation;
        if (room < static_cast<size_t>(capture_size)) return kBadValue;
        out.assign(static_cast<size_t>(capture_size), 128);
        for (size_t index = 0; samples != nullptr && index < out.size() && index < samples->size(); ++index) {
            const float value = std::clamp((*samples)[index], -1.0f, 1.0f);
            out[index] = static_cast<uint8_t>(std::lround(128.0f + value * 127.0f));
        }
        return kSuccess;
    }

    /// The FFT: capture_size signed bytes: the real parts of the DC and of the Nyquist point, then the real and
    /// imaginary parts of points 1 to n/2 - 1; zero for silence.
    int get_fft(std::vector<uint8_t> &out, size_t room, const std::vector<float> *samples = nullptr) const {
        if (!enabled) return kInvalidOperation;
        if (room < static_cast<size_t>(capture_size)) return kBadValue;
        const size_t n = static_cast<size_t>(capture_size);
        out.assign(n, 0);
        if (samples == nullptr || samples->size() < n) return kSuccess;
        std::vector<double> re(samples->begin(), samples->begin() + static_cast<std::ptrdiff_t>(n)), im(n, 0.0);
        transform(re, im);
        const double scale = 127.0 / (static_cast<double>(n) / 2);
        const auto byte = [&](double value) {
            return static_cast<uint8_t>(static_cast<int8_t>(std::clamp<long>(std::lround(value * scale), -128, 127)));
        };
        out[0] = byte(re[0] / 2);
        out[1] = byte(re[n / 2] / 2);
        for (size_t k = 1; k < n / 2; ++k) {
            out[2 * k] = byte(re[k]);
            out[2 * k + 1] = byte(im[k]);
        }
        return kSuccess;
    }

    /// The peak and RMS in mB (0 is full scale); only with the measurement mode on.
    int get_peak_rms(int *peak, int *rms, const std::vector<float> *samples = nullptr) const {
        if (measurement_mode != 1) return kInvalidOperation;
        double top = 0, sum = 0;
        const size_t n = samples == nullptr ? 0 : std::min(samples->size(), static_cast<size_t>(capture_size));
        for (size_t index = 0; index < n; ++index) {
            const double value = (*samples)[index];
            top = std::max(top, std::fabs(value));
            sum += value * value;
        }
        const double level = n == 0 ? 0 : std::sqrt(sum / static_cast<double>(n));
        const auto millibel = [](double linear) {
            if (linear <= 0) return kSilenceMillibel;
            return std::max<int>(kSilenceMillibel, static_cast<int>(std::lround(2000 * std::log10(linear))));
        };
        *peak = millibel(top);
        *rms = millibel(level);
        return kSuccess;
    }

private:
    /// An in-place radix-2 FFT of a power-of-two size.
    static void transform(std::vector<double> &re, std::vector<double> &im) {
        const size_t n = re.size();
        for (size_t i = 1, j = 0; i < n; ++i) {
            size_t bit = n >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) {
                std::swap(re[i], re[j]);
                std::swap(im[i], im[j]);
            }
        }
        for (size_t length = 2; length <= n; length <<= 1) {
            const double angle = -2 * 3.14159265358979323846 / static_cast<double>(length);
            for (size_t start = 0; start < n; start += length) {
                for (size_t k = 0; k < length / 2; ++k) {
                    const double phase = angle * static_cast<double>(k);
                    const double wr = std::cos(phase), wi = std::sin(phase);
                    const size_t a = start + k, b = start + k + length / 2;
                    const double tr = re[b] * wr - im[b] * wi, ti = re[b] * wi + im[b] * wr;
                    re[b] = re[a] - tr;
                    im[b] = im[a] - ti;
                    re[a] += tr;
                    im[a] += ti;
                }
            }
        }
    }
};

}  // namespace minato::visualizer
