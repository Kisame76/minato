// apk-run's audio output (ADR-0036): an output that plays nowhere, as the host has no speaker of its own, so the
// guest's streams open and play on the Mac as on a device (the AudioTrack of gap 13, audiotrack-test). The hardware is
// 48 kHz with 10 ms bursts; each open stream gets a thread that reads one burst of its ring every 10 ms with
// apkrt_audio_read, as the App's render block does, and drops it. The close stops and joins that thread.
#pragma once

#include "APKRuntime/apkrt.h"

#include <mach/mach_time.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace headless_audio {

constexpr double kRate = 48'000;
constexpr double kBurstSeconds = 0.010;

struct Reader {
    std::atomic<bool> stop{false};
    std::thread thread;
};

// Never destroyed: a reader may still run when the program ends, and no static destructor may meet its thread.
inline std::mutex &lock() {
    static auto *mutex = new std::mutex;
    return *mutex;
}

inline std::map<std::uint64_t, std::unique_ptr<Reader>> &readers() {
    static auto *map = new std::map<std::uint64_t, std::unique_ptr<Reader>>;
    return *map;
}

inline void read_bursts(std::uint64_t stream, double rate, std::int32_t channels, const std::atomic<bool> *stop) {
    const auto frames = static_cast<std::int64_t>(std::max(1.0, std::floor(kBurstSeconds * rate + 0.5)));
    std::vector<float> buffer(static_cast<std::size_t>(frames) * static_cast<std::size_t>(channels));
    const auto period = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(kBurstSeconds));
    auto next = std::chrono::steady_clock::now();
    while (!stop->load()) {
        apkrt_audio_read(stream, buffer.data(), frames, mach_absolute_time());
        next += period;
        std::this_thread::sleep_until(next);
    }
}

inline int open(void *, std::uint64_t stream, double sample_rate, std::int32_t channels, std::int32_t,
                apkrt_audio_hardware *hardware) {
    *hardware = {kRate, kBurstSeconds};
    auto reader = std::make_unique<Reader>();
    reader->thread = std::thread(read_bursts, stream, sample_rate != 0 ? sample_rate : kRate, channels, &reader->stop);
    const std::lock_guard hold(lock());
    readers()[stream] = std::move(reader);
    return 0;
}

inline void close(void *, std::uint64_t stream) {
    std::unique_ptr<Reader> reader;
    {
        const std::lock_guard hold(lock());
        const auto found = readers().find(stream);
        if (found == readers().end()) return;
        reader = std::move(found->second);
        readers().erase(found);
    }
    reader->stop.store(true);
    reader->thread.join();
}

inline const apkrt_audio_host host{nullptr, open, close};

}  // namespace headless_audio
