// ADR-0037 a) on the Mac: the parity benchmark's scene built for the host (graphics/bench, MinatoBench's files) draws
// a run on a CAMetalLayer the size of an iPhone 15 Pro Max's screen (430 x 932 points at scale 3), with ticks
// scripted in lockstep: tick k comes k periods of 120 Hz after the first, each once frame k-1 is drawn, so every tick
// draws a frame. bench.conf asks for 120 Hz, the load, 9 s and the CRC of frame 1000. It passes when the run ends by
// itself after 1080 frames; bench_test.py then checks its scene.csv and compares the CRC with the APK's.
//   graphics-bench-host <gles | vulkan> <light | heavy> <scratch> <libMoltenVK.dylib>
#import <Foundation/Foundation.h>
#import <QuartzCore/QuartzCore.h>

#include "minato_bench.h"

#include <time.h>
#include <unistd.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>

namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

constexpr int frames = 1080;  // 9 s at 120 Hz
constexpr double period = 1e9 / 120;

std::int64_t now() {
    return static_cast<std::int64_t>(clock_gettime_nsec_np(CLOCK_UPTIME_RAW));
}

/// Waits until the scene drew `count` frames or its run ended: true for the frames.
bool drawn(std::uint64_t count, std::chrono::milliseconds limit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    char error[256];
    while (std::chrono::steady_clock::now() < deadline) {
        if (minato_bench_frames() >= count) return true;
        if (minato_bench_wait(0, error, sizeof error) != 1) return false;
        std::this_thread::sleep_for(100us);
    }
    return false;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc != 5) {
        std::fprintf(stderr,
                     "usage: graphics-bench-host <gles | vulkan> <light | heavy> <scratch> <libMoltenVK.dylib>\n");
        return 2;
    }
    const std::string api = argv[1], load = argv[2];
    const fs::path scratch = fs::path(argv[3]) / (api + "-" + load), moltenvk = argv[4];
    if (api == "vulkan" && !fs::exists(moltenvk)) {
        std::printf("skipped: %s is missing (tools/fetch-moltenvk.sh)\n", moltenvk.c_str());
        return 0;
    }
    fs::remove_all(scratch);
    fs::create_directories(scratch);
    const fs::path csv = scratch / "scene.csv";
    const std::string conf = "run=" + api + "-120-" + load + "-native-00\nrate=120\nload=" + load +
                             "\nseconds=9\ncheck=1000\n";

    int failures = 0;
    char error[512] = "";
    @autoreleasepool {
        CAMetalLayer *layer = [CAMetalLayer layer];
        layer.contentsScale = 3;
        layer.bounds = CGRectMake(0, 0, 430, 932);
        layer.drawableSize = CGSizeMake(1290, 2796);
        if (minato_bench_start(api.c_str(), conf.c_str(), (__bridge void *)layer, moltenvk.c_str(), csv.c_str(), error,
                               sizeof error) != 0) {
            std::printf("FAIL: minato_bench_start: %s\n", error);
            return 1;
        }
    }

    // Lockstep: the first tick waits for the scene's start, each later one for the frame before it.
    const std::int64_t first = now();
    std::uint64_t ticks = 0;
    for (;; ++ticks) {
        minato_bench_tick(first + std::llround(static_cast<double>(ticks) * period));
        if (!drawn(ticks + 1, ticks == 0 ? 20000ms : 5000ms)) break;
    }
    const int status = minato_bench_wait(10, error, sizeof error);
    std::printf("host: %llu ticks, %llu frames\n", static_cast<unsigned long long>(ticks + 1),
                static_cast<unsigned long long>(minato_bench_frames()));
    if (status != 0) {
        std::printf("FAIL: the run %s%s\n", status == 1 ? "did not end" : "failed: ", status == 1 ? "" : error);
        ++failures;
    }
    if (minato_bench_frames() != frames) {
        std::printf("FAIL: %llu frames, not %d\n", static_cast<unsigned long long>(minato_bench_frames()), frames);
        ++failures;
    }
    if (!fs::exists(csv)) {
        std::printf("FAIL: no scene.csv\n");
        ++failures;
    }
    std::printf(failures == 0 ? "PASS: %s %s\n" : "FAIL: %s %s\n", api.c_str(), load.c_str());
    std::fflush(stdout);
    _exit(failures == 0 ? 0 : 1);
}
