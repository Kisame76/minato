// apk-run's audio input (ADR-0064): a microphone that hears a WAV file, so that the guest's recording is checked on the
// Mac (the mic-test fixture and a tone check). MINATO_AUDIO_INPUT_FILE names a PCM WAV file (8, 16 or 32 bits or
// float32, any channels and rate), which each open stream plays from its start and loops, resampled to the stream's
// rate and channels by the nearest frame; without it the microphone hears silence. The hardware is 48 kHz with 10 ms
// bursts, as the output's. Each open stream gets a thread that writes one burst every 10 ms with
// apkrt_audio_input_write, as the App's engine tap does, and the close stops and joins it.
#pragma once

#include "APKRuntime/apkrt.h"

#include <mach/mach_time.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace headless_audio_input {

constexpr double kRate = 48'000;
constexpr double kBurstSeconds = 0.010;

/// The WAV file as float frames: `channels` interleaved samples per frame at `rate`.
struct Clip {
    int channels = 0;
    double rate = 0;
    std::vector<float> samples;
    std::int64_t frames() const { return channels > 0 ? static_cast<std::int64_t>(samples.size()) / channels : 0; }
};

inline std::uint32_t le(const std::vector<std::uint8_t> &bytes, std::size_t at, int width) {
    std::uint32_t value = 0;
    for (int index = 0; index < width && at + static_cast<std::size_t>(index) < bytes.size(); ++index) {
        value |= std::uint32_t{bytes[at + static_cast<std::size_t>(index)]} << (8 * index);
    }
    return value;
}

/// Reads the file named by MINATO_AUDIO_INPUT_FILE; an empty clip (silence) when there is none or it is not PCM.
inline const Clip &clip() {
    static const Clip loaded = [] {
        Clip result;
        const char *path = std::getenv("MINATO_AUDIO_INPUT_FILE");
        if (path == nullptr) return result;
        std::vector<std::uint8_t> bytes;
        if (std::FILE *file = std::fopen(path, "rb")) {  // stdio, not <fstream>, which brings std::quoted into scope
            std::uint8_t chunk[4096];
            for (std::size_t got; (got = std::fread(chunk, 1, sizeof chunk, file)) > 0;) {
                bytes.insert(bytes.end(), chunk, chunk + got);
            }
            std::fclose(file);
        }
        if (bytes.size() < 12 || std::memcmp(bytes.data(), "RIFF", 4) != 0) return result;
        int format = 0, channels = 0, bits = 0;
        std::uint32_t rate = 0;
        std::size_t at = 12;
        while (at + 8 <= bytes.size()) {
            const std::uint32_t size = le(bytes, at + 4, 4);
            const std::size_t body = at + 8;
            if (std::memcmp(bytes.data() + at, "fmt ", 4) == 0) {
                format = static_cast<int>(le(bytes, body, 2));
                channels = static_cast<int>(le(bytes, body + 2, 2));
                rate = le(bytes, body + 4, 4);
                bits = static_cast<int>(le(bytes, body + 14, 2));
            } else if (std::memcmp(bytes.data() + at, "data", 4) == 0 && channels > 0 && rate > 0) {
                const int width = bits / 8;
                const std::size_t count =
                    std::min<std::size_t>(size, bytes.size() - body) / static_cast<std::size_t>(width);
                for (std::size_t index = 0; index < count; ++index) {
                    const std::size_t from = body + index * static_cast<std::size_t>(width);
                    float value = 0;
                    if (format == 3 && bits == 32) {
                        const std::uint32_t raw = le(bytes, from, 4);
                        std::memcpy(&value, &raw, sizeof value);
                    } else if (bits == 8) {
                        value = (static_cast<float>(bytes[from]) - 128.0f) / 128.0f;
                    } else if (bits == 16) {
                        value = static_cast<float>(static_cast<std::int16_t>(le(bytes, from, 2))) / 32768.0f;
                    } else if (bits == 32) {
                        value = static_cast<float>(static_cast<std::int32_t>(le(bytes, from, 4))) / 2147483648.0f;
                    }
                    result.samples.push_back(value);
                }
                result.channels = channels;
                result.rate = rate;
                break;
            }
            at = body + size + (size & 1);
        }
        return result;
    }();
    return loaded;
}

struct Capture {
    std::atomic<bool> stop{false};
    std::thread thread;
};

// Never destroyed: a capture may still run when the program ends, and no static destructor may meet its thread.
inline std::mutex &lock() {
    static auto *mutex = new std::mutex;
    return *mutex;
}

inline std::map<std::uint64_t, std::unique_ptr<Capture>> &captures() {
    static auto *map = new std::map<std::uint64_t, std::unique_ptr<Capture>>;
    return *map;
}

inline void write_bursts(std::uint64_t stream, double rate, std::int32_t channels, const std::atomic<bool> *stop) {
    const auto frames = static_cast<std::int64_t>(std::max(1.0, std::floor(kBurstSeconds * rate + 0.5)));
    std::vector<float> buffer(static_cast<std::size_t>(frames) * static_cast<std::size_t>(channels));
    const Clip &source = clip();
    const auto period = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(kBurstSeconds));
    auto next = std::chrono::steady_clock::now();
    std::int64_t position = 0;  // the stream's frames so far
    while (!stop->load()) {
        for (std::int64_t frame = 0; frame < frames; ++frame, ++position) {
            for (std::int32_t channel = 0; channel < channels; ++channel) {
                float value = 0;
                if (source.frames() > 0) {
                    const auto played = static_cast<double>(position) * source.rate / rate;
                    const std::int64_t at = static_cast<std::int64_t>(played) % source.frames();
                    value = source.samples[static_cast<std::size_t>(at * source.channels +
                                                                    std::min(channel, source.channels - 1))];
                }
                buffer[static_cast<std::size_t>(frame * channels + channel)] = value;
            }
        }
        apkrt_audio_input_write(stream, buffer.data(), frames, mach_absolute_time());
        next += period;
        std::this_thread::sleep_until(next);
    }
}

inline int open(void *, std::uint64_t stream, double sample_rate, std::int32_t channels, std::int32_t,
                apkrt_audio_hardware *hardware) {
    *hardware = {kRate, kBurstSeconds};
    auto capture = std::make_unique<Capture>();
    capture->thread =
        std::thread(write_bursts, stream, sample_rate != 0 ? sample_rate : kRate, channels, &capture->stop);
    const std::lock_guard hold(lock());
    captures()[stream] = std::move(capture);
    return 0;
}

inline void close(void *, std::uint64_t stream) {
    std::unique_ptr<Capture> capture;
    {
        const std::lock_guard hold(lock());
        const auto found = captures().find(stream);
        if (found == captures().end()) return;
        capture = std::move(found->second);
        captures().erase(found);
    }
    capture->stop.store(true);
    capture->thread.join();
}

inline const apkrt_audio_input_host host{nullptr, open, close};

}  // namespace headless_audio_input
