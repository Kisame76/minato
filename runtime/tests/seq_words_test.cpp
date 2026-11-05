// The sequence-locked words (src/seq_words.h) that carry a session's audio effects to the render block: a reader on
// another thread, with no lock, wait or allocation, never sees the words of two writes mixed.
#include "seq_words.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <thread>

namespace {

int failures = 0;

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

void a_reader_sees_what_was_written() {
    apkrt::SeqWords<9> words;
    std::int32_t out[9];
    CHECK(words.read(out));  // nothing written yet: zeros
    for (const std::int32_t value : out) CHECK(value == 0);
    const std::int32_t in[9] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
    words.write(in);
    CHECK(words.read(out));
    for (int index = 0; index < 9; ++index) CHECK(out[index] == in[index]);
}

void a_reader_never_sees_two_writes_mixed() {
    apkrt::SeqWords<9> words;
    std::atomic<bool> stop{false};
    std::thread writer([&] {
        std::int32_t value = 0;
        while (!stop.load()) {
            ++value;
            std::int32_t in[9];
            for (auto &word : in) word = value;  // every word of one write is the same
            words.write(in);
        }
    });
    long taken = 0, torn = 0;
    for (long round = 0; round < 3'000'000; ++round) {
        std::int32_t out[9];
        if (!words.read(out)) continue;  // a write under way: the reader keeps its last, as the render block does
        ++taken;
        for (int index = 1; index < 9; ++index) {
            if (out[index] != out[0]) {
                ++torn;
                break;
            }
        }
    }
    stop.store(true);
    writer.join();
    CHECK(taken > 1000);
    CHECK(torn == 0);
}

}  // namespace

int main() {
    a_reader_sees_what_was_written();
    a_reader_never_sees_two_writes_mixed();
    std::printf("seq_words_test: %s\n", failures == 0 ? "all fine" : "failed");
    return failures == 0 ? 0 : 1;
}
