#include "monitor_probe.h"

#include <atomic>

namespace apkrt::monitor {
namespace {

constexpr unsigned kTries = 64;           // tries of one pass before it counts as livelocked
constexpr std::uint64_t kProbePasses = 20000;

// The exclusive cell and the unrelated target sit on pages of their own, so that they lie in different reservation granules
// by any granule size.
alignas(16384) std::uint64_t cell[2048];
alignas(16384) std::uint64_t other[2048];

/// One try: load-exclusive, the variant's one instruction, store-exclusive; the store-exclusive's status.
inline std::uint32_t attempt(Variant variant, std::uint64_t *target, std::uint64_t *granule) {
    std::uint64_t value, scratch = 0;
    std::uint32_t status;
    switch (variant) {
        case Variant::clean:
            __asm__ volatile("ldxr %0, [%2]\n\tadd %0, %0, #1\n\tstxr %w1, %0, [%2]"
                             : "=&r"(value), "=&r"(status) : "r"(target) : "memory");
            break;
        case Variant::clrex:
            __asm__ volatile("ldxr %0, [%2]\n\tclrex\n\tadd %0, %0, #1\n\tstxr %w1, %0, [%2]"
                             : "=&r"(value), "=&r"(status) : "r"(target) : "memory");
            break;
        case Variant::load:
            __asm__ volatile("ldxr %0, [%3]\n\tldr %2, [%4]\n\tadd %0, %0, #1\n\tstxr %w1, %0, [%3]"
                             : "=&r"(value), "=&r"(status), "=&r"(scratch) : "r"(target), "r"(granule) : "memory");
            break;
        case Variant::store:
            __asm__ volatile("ldxr %0, [%3]\n\tstr %0, [%4]\n\tadd %0, %0, #1\n\tstxr %w1, %0, [%3]"
                             : "=&r"(value), "=&r"(status), "=&r"(scratch) : "r"(target), "r"(granule) : "memory");
            break;
    }
    return status;
}

std::atomic<int> forced{-1};  // -1: the probe answers, 0 or 1: a test's answer

bool probe() {
    const Result clean = run(Variant::clean, kProbePasses);
    const Result clrex = run(Variant::clrex, 64);
    const Result load = run(Variant::load, kProbePasses);
    const Result store = run(Variant::store, kProbePasses);
    const bool controls = clean.livelocked == 0 && clrex.livelocked == clrex.passes;
    return controls && load.livelocked == 0 && store.livelocked == 0;
}

}  // namespace

Result run(Variant variant, std::uint64_t passes) {
    Result result;
    for (std::uint64_t pass = 0; pass < passes; ++pass) {
        ++result.passes;
        unsigned tries = 0;
        while (tries < kTries && attempt(variant, &cell[0], &other[0]) != 0) ++tries;
        if (tries == kTries) {
            ++result.livelocked;
        } else if (tries == 0) {
            ++result.first_try;
        } else {
            ++result.retried;
        }
    }
    return result;
}

bool kept() {
    const int answer = forced.load(std::memory_order_acquire);
    if (answer >= 0) return answer != 0;
    static const bool measured = probe();  // once, thread-safe
    return measured;
}

void force_for_testing(std::optional<bool> answer) {
    forced.store(answer ? (*answer ? 1 : 0) : -1, std::memory_order_release);
}

}  // namespace apkrt::monitor
