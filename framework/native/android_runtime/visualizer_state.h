// Ours (the audio effects plan): the state and the answers of one android.media.audiofx.Visualizer, as Android 15's
// engine gives them (measured by visualizer-test): the capture size range, the defaults, which calls need the
// Visualizer disabled and which enabled, the codes for a value out of range, and the data of the capture. The data is
// what the output mix carries: silence until the session's output is tapped (the waveform is 8-bit unsigned, so 128 is
// zero level; the FFT and the measured peak and RMS of silence follow from it). Plain C++ without JNI, so that
// framework/tests/native/visualizer_state_test.cpp runs it on the host.
#pragma once

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

    /// The waveform: capture_size bytes of silence (128); the array must hold them.
    int get_waveform(std::vector<uint8_t> &out, size_t room) const {
        if (!enabled) return kInvalidOperation;
        if (room < static_cast<size_t>(capture_size)) return kBadValue;
        out.assign(static_cast<size_t>(capture_size), 128);
        return kSuccess;
    }

    /// The FFT: capture_size bytes (real and imaginary parts of n/2 + 1 points), zero for silence.
    int get_fft(std::vector<uint8_t> &out, size_t room) const {
        if (!enabled) return kInvalidOperation;
        if (room < static_cast<size_t>(capture_size)) return kBadValue;
        out.assign(static_cast<size_t>(capture_size), 0);
        return kSuccess;
    }

    /// The peak and RMS in mB; only with the measurement mode on.
    int get_peak_rms(int *peak, int *rms) const {
        if (measurement_mode != 1) return kInvalidOperation;
        *peak = kSilenceMillibel;
        *rms = kSilenceMillibel;
        return kSuccess;
    }
};

}  // namespace minato::visualizer
