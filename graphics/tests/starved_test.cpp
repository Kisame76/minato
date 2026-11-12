// starved.h, which tells graphics-app-test's live frame intervals the machine's from ours: StarvedRuns on made-up
// samples (runnable with its CPU time standing still is starved; waiting, or runnable and working, is not; runs
// shorter than the shortest kept are dropped; a query outside a run sees nothing), and ThreadSampler on real
// threads: a thread that sleeps is never starved while it sleeps, however loaded the Mac is.
#include "starved.h"

#include <time.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>

namespace {

int failures = 0;

void expect(bool ok, const char *what) {
    if (ok) return;
    std::printf("FAIL: %s\n", what);
    ++failures;
}

constexpr std::uint64_t ms = 1'000'000;

/// Samples of thread 1 every 5 ms from `from` to `to` (ms), runnable or not, with `cpu_share` of each span as CPU.
void feed(StarvedRuns &runs, std::uint64_t &cpu_us, std::uint64_t from, std::uint64_t to, bool runnable,
          double cpu_share) {
    for (std::uint64_t at = from; at <= to; at += 5) {
        runs.add({1, at * ms, cpu_us, runnable});
        cpu_us += static_cast<std::uint64_t>(5000 * cpu_share);
    }
}

}  // namespace

int main() {
    {
        StarvedRuns runs;
        std::uint64_t cpu = 0;
        feed(runs, cpu, 0, 100, true, 1.0);      // runnable and working: not starved
        feed(runs, cpu, 105, 165, true, 0.0);    // runnable, no CPU for 60 ms: starved
        feed(runs, cpu, 170, 470, false, 0.0);   // waiting for 300 ms: ours, not starved
        feed(runs, cpu, 475, 495, true, 0.0);    // starved for 20 ms only: below the shortest kept run
        feed(runs, cpu, 500, 600, true, 0.5);    // runnable with half a CPU: working, not starved
        runs.finish();
        const double starved = runs.longest(100 * ms, 170 * ms);
        std::printf("made up: starved %.0f ms around 100-170 ms, %.0f ms over the wait, %.0f ms over the short run, "
                    "%.0f ms over the half CPU\n",
                    starved, runs.longest(200 * ms, 450 * ms), runs.longest(472 * ms, 498 * ms),
                    runs.longest(505 * ms, 600 * ms));
        expect(starved >= 55 && starved <= 70, "60 ms runnable without CPU time is a starved run of about 60 ms");
        expect(runs.longest(200 * ms, 450 * ms) == 0, "a thread that waits is never starved");
        expect(runs.longest(472 * ms, 498 * ms) == 0, "a starved run under 30 ms is not kept");
        expect(runs.longest(505 * ms, 600 * ms) == 0, "a runnable thread that gets CPU time is not starved");
        expect(runs.longest(0, 100 * ms) == 0, "a runnable thread that works is not starved");
    }
    {
        // A real thread that sleeps 300 ms: while it sleeps it waits, so the sampler never finds it starved there.
        StarvedRuns runs;
        ThreadSampler sampler(runs);
        sampler.start();
        std::uint64_t begun = 0;
        std::uint64_t ended = 0;
        std::thread sleeper([&] {
            begun = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            ended = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        });
        sleeper.join();
        sampler.stop();
        // The sleep's middle: its first and last 50 ms left out (the thread may wait runnable to start and to go on).
        const double starved = runs.longest(begun + 50 * ms, ended - 50 * ms);
        std::printf("sampler: a thread asleep for %.0f ms, starved %.0f ms while asleep\n",
                    static_cast<double>(ended - begun) / 1e6, starved);
        expect(starved == 0, "a sleeping thread is never starved while it sleeps");
    }
    std::printf("%s\n", failures == 0 ? "ok" : "failed");
    return failures == 0 ? 0 : 1;
}
