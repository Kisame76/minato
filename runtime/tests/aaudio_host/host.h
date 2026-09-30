// What the Mac builds of the guest's audio libraries (runtime/android/aaudio.cpp and opensles.cpp) need around them in
// their tests (host.cpp): the guest's syscall(), which goes to the runtime's dispatcher, and __android_log_print, which
// writes to stderr and counts the lines of each tag; and a fake audio host of 48 kHz with 5 ms IO buffers, which
// records the streams it opens and closes, and whose streams a test reads as the render block would.
#pragma once

#include "APKRuntime/apkrt.h"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace audio_host {

/// A stream the fake host opened, with what the open asked for.
struct Opened {
    std::uint64_t handle = 0;
    double sample_rate = 0;
    std::int32_t channels = 0;
    std::int32_t performance_mode = 0;
};

struct FakeHost {
    std::mutex lock;
    std::vector<Opened> opened;
    std::vector<std::uint64_t> closed;
    bool disconnect_in_open = false;
};
extern FakeHost fake;

/// Makes the fake host the runtime's audio host, with nothing opened or closed yet.
void use_fake_host();

/// The stream the host opened last, or one of zeros.
Opened last_opened();

/// The lines the guest has logged with `tag` so far.
int log_lines(const char *tag);

/// The render block's read of up to `frames` frames of `channels` channels.
std::vector<float> render(std::uint64_t handle, std::int64_t frames, std::int32_t channels = 1);

/// Whether the stream is STOPPED, as the guest's audio_status tells it: also while it plays out what it held.
bool stopped(std::uint64_t handle);

template <typename Done>
bool within(std::chrono::milliseconds limit, Done done) {
    const auto end = std::chrono::steady_clock::now() + limit;
    while (!done()) {
        if (std::chrono::steady_clock::now() >= end) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

}  // namespace audio_host
