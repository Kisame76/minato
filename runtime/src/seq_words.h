// A few 32-bit words that one writer at a time sets and any number of readers take as one snapshot, with no lock, wait
// or allocation on the reading side (the audio effects of a session, which the render block reads on CoreAudio's
// thread). A sequence lock over atomic words: the writer makes the sequence odd, writes the words, makes it even;
// a reader that finds it odd or changed asks again. The order is the standard one for ARM64: the odd sequence is
// visible before any word (a release fence after it), and the even one after every word (a release store).
#pragma once

#include <atomic>
#include <cstdint>

namespace apkrt {

template <int N>
class SeqWords {
public:
    /// One writer at a time (the caller holds a lock).
    void write(const std::int32_t (&words)[N]) {
        const std::uint32_t before = sequence_.load(std::memory_order_relaxed);
        sequence_.store(before + 1, std::memory_order_relaxed);  // odd
        std::atomic_thread_fence(std::memory_order_release);
        for (int index = 0; index < N; ++index) words_[index].store(words[index], std::memory_order_relaxed);
        sequence_.store(before + 2, std::memory_order_release);  // even
    }

    /// The words as of one write; false when a write is under way after a few tries (`out` is then not set).
    bool read(std::int32_t (&out)[N]) const {
        for (int attempt = 0; attempt < 4; ++attempt) {
            const std::uint32_t before = sequence_.load(std::memory_order_acquire);
            if ((before & 1) != 0) continue;
            std::int32_t taken[N];
            for (int index = 0; index < N; ++index) taken[index] = words_[index].load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (sequence_.load(std::memory_order_relaxed) != before) continue;
            for (int index = 0; index < N; ++index) out[index] = taken[index];
            return true;
        }
        return false;
    }

    /// The words without the sequence, for the writer (which holds the lock) to build on.
    void peek(std::int32_t (&out)[N]) const {
        for (int index = 0; index < N; ++index) out[index] = words_[index].load(std::memory_order_relaxed);
    }

private:
    std::atomic<std::uint32_t> sequence_{0};
    std::atomic<std::int32_t> words_[N] = {};
};

}  // namespace apkrt
