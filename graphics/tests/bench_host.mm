// ADR-0037 a) on the Mac: the parity benchmark's scene built for the host (graphics/bench, MinatoBench's files) draws
// a run on graphics/host's layer the size of an iPhone 15 Pro Max's screen (430 x 932 points at scale 3), with ticks
// scripted in lockstep: tick k comes k periods of 120 Hz after the first, each once frame k-1 is drawn, so every tick
// draws a frame. bench.conf asks for 120 Hz, the load, 9 s and the CRC of frame 1000. It passes when the run ends by
// itself after 1080 frames; bench_test.py then checks its scene.csv and compares the CRC with the APK's.
// With `record` (ADR-0037 b), graphics.recorder): a run of 1 s, 120 frames, which the layer records, and a
// nextDrawable of the test's own before and after the recording. It passes when the recording holds every
// drawable once and in order (recording_check.h), taken on the render thread at QoS user-interactive, or at default
// when the program's own arguments end with -benchmarkRenderQoS default, which MinatoBench reads (ADR-0030 V5).
// A run without `record` may end with -benchmarkQueuePath <value>, which MinatoBench reads too (ADR-0030 V5, the
// queue path's A/B); bench_test.py checks what the run logs.
// With `record-class` (ADR-0042 c) 2), graphics.recorder_class): the same recording through MinatoLayerRecorder, on a
// CAMetalLayer subclass without a nextDrawable of its own, as Godot's GDTMetalLayer. The recorder attaches to that
// class, again too, and to no other: not MinatoMetalLayer, a class with its own nextDrawable, a second class or one
// that is no CAMetalLayer. A second layer of the class draws during the recording, which must not hold its drawable.
//   graphics-bench-host <gles | vulkan> <light | heavy | fill> <scratch> <libMoltenVK.dylib>
//                       [record [-benchmarkRenderQoS <value>] | record-class | -benchmarkQueuePath <value>]
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#include "minato_bench.h"
#include "minato_layer.h"
#include "patience.h"
#include "recording_check.h"

#include <time.h>
#include <unistd.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

// record-class's layers: two classes without a nextDrawable of their own, as Godot's GDTMetalLayer, and one with it.
@interface PlainLayer : CAMetalLayer
@end
@implementation PlainLayer
@end

@interface OtherPlainLayer : CAMetalLayer
@end
@implementation OtherPlainLayer
@end

@interface OwnNextLayer : CAMetalLayer
@end
@implementation OwnNextLayer
- (nullable id<CAMetalDrawable>)nextDrawable {
    return [super nextDrawable];
}
@end

namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

constexpr double period = 1e9 / 120;

std::int64_t now() {
    return static_cast<std::int64_t>(clock_gettime_nsec_np(CLOCK_UPTIME_RAW));
}

/// Waits until the scene drew `count` frames or its run ended: true for the frames. `limit` counts this thread's
/// rounds (patience.h).
bool drawn(std::uint64_t count, std::chrono::milliseconds limit) {
    Patience patience(limit);
    char error[256];
    do {
        if (minato_bench_frames() >= count) return true;
        if (minato_bench_wait(0, error, sizeof error) != 1) return false;
        std::this_thread::sleep_for(100us);
    } while (patience.more(100us));
    return minato_bench_frames() >= count;
}

/// minato_bench_wait for up to `seconds` of this thread's rounds.
int ended(double seconds, char *error, std::size_t size) {
    Patience patience(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::duration<double>(seconds)));
    int status;
    while ((status = minato_bench_wait(0.1, error, size)) == 1 && patience.more(100ms)) {
    }
    return status;
}

}  // namespace

int main(int argc, char **argv) {
    const bool record_class = argc == 6 && std::strcmp(argv[5], "record-class") == 0;
    const bool record = ((argc == 6 || argc == 8) && std::strcmp(argv[5], "record") == 0) || record_class;
    const bool qos = argc == 8 && std::strcmp(argv[6], "-benchmarkRenderQoS") == 0;
    const bool queue_path = argc == 7 && std::strcmp(argv[5], "-benchmarkQueuePath") == 0;
    const bool scaled = argc == 7 && std::strcmp(argv[5], "-benchmarkScale") == 0;
    // ADR-0066 step 4: an engine cap, and an FPS unlock over it.
    const bool capped = (argc == 7 || (argc == 9 && std::strcmp(argv[7], "-benchmarkUnlock") == 0)) &&
                        std::strcmp(argv[5], "-benchmarkCap") == 0;
    if (argc != 5 && !(record && (argc == 6 || qos)) && !queue_path && !scaled && !capped) {
        std::fprintf(stderr, "usage: graphics-bench-host <gles | vulkan> <light | heavy | fill> <scratch> "
                             "<libMoltenVK.dylib> [record [-benchmarkRenderQoS <value>] | record-class | "
                             "-benchmarkScale <percent> | -benchmarkCap <n> [-benchmarkUnlock <m>] | "
                             "-benchmarkQueuePath <value>]\n");
        return 2;
    }
    const bool at_default = qos && std::strcmp(argv[7], "default") == 0;
    const std::string api = argv[1], load = argv[2];
    const fs::path scratch = fs::path(argv[3]) / (api + "-" + load), moltenvk = argv[4];
    // A capped run's engine draws every `every`th tick: the fewest whole periods of 120 Hz, within half a period, that
    // the rate it holds takes, the cap raised to the unlock as the loader's wrapper does, -1 drawing at Unity's 30.
    const int cap = capped ? std::atoi(argv[6]) : 0, unlock = capped && argc == 9 ? std::atoi(argv[8]) : 0;
    const int held = unlock > 0 && (cap <= 0 || cap < unlock) ? unlock : cap;
    std::uint64_t every = 1;
    while (capped && static_cast<double>(every) * period + period / 2 < 1e9 / (held <= 0 ? 30 : held)) ++every;
    const std::uint64_t frames = record ? 120 : 1080 / every;  // 1 s or 9 s at 120 Hz
    if (api == "vulkan" && !fs::exists(moltenvk)) {
        std::printf("skipped: %s is missing (tools/fetch-moltenvk.sh)\n", moltenvk.c_str());
        return 0;
    }
    fs::remove_all(scratch);
    fs::create_directories(scratch);
    const fs::path csv = scratch / "scene.csv";
    const std::string conf = "run=" + api + "-120-" + load + "-native-00\nrate=120\nload=" + load +
                             (record ? "\nseconds=1\n" : capped ? "\nseconds=9\n" : "\nseconds=9\ncheck=1000\n") +
                             (scaled ? "scale=" + std::string(argv[6]) + "\n" : "") +
                             (capped ? "cap=" + std::string(argv[6]) + "\n" : "") +
                             (unlock != 0 ? "unlock=" + std::string(argv[8]) + "\n" : "");

    int failures = 0;
    char error[512] = "";
    if (record_class) {
        // Each refused for its own reason first, while no class has the recorder. Then the class, before it has
        // layers, as the twin attaches before Godot makes its layer, and no second class after it.
        const struct {
            Class layer_class;
            const char *what;
        } refused[] = {{MinatoMetalLayer.class, "MinatoMetalLayer"},
                       {OwnNextLayer.class, "a class with its own nextDrawable"},
                       {NSObject.class, "a class that is no CAMetalLayer"}};
        for (const auto &entry : refused) {
            if ([MinatoLayerRecorder attachToClass:entry.layer_class]) {
                std::printf("FAIL: the recorder attached to %s\n", entry.what);
                return 1;
            }
        }
        if (![MinatoLayerRecorder attachToClass:PlainLayer.class] || ![MinatoLayerRecorder attachToClass:PlainLayer.class]) {
            std::printf("FAIL: the recorder did not attach to a class without its own nextDrawable\n");
            return 1;
        }
        if ([MinatoLayerRecorder attachToClass:OtherPlainLayer.class]) {
            std::printf("FAIL: the recorder attached to a second class\n");
            ++failures;
        }
    }
    CAMetalLayer *layer = record_class ? [PlainLayer layer] : [MinatoMetalLayer layer];
    PlainLayer *other = record_class ? [PlainLayer layer] : nil;
    @autoreleasepool {
        // Opaque, as the App's view makes a session's layer by Android's rule (V5's late frames); ANGLE leaves it, and
        // MoltenVK keeps it for the scene's OPAQUE.
        layer.opaque = YES;
        layer.contentsScale = 3;
        layer.bounds = CGRectMake(0, 0, 430, 932);
        layer.drawableSize = CGSizeMake(1290, 2796);
        if (record) {
            // A drawable before the recording, which it must not hold; room for twice the frames, so that extra
            // drawables show in the count.
            layer.device = MTLCreateSystemDefaultDevice();
            (void)[layer nextDrawable];
            const BOOL started = record_class ? [MinatoLayerRecorder startRecording:layer capacity:2 * frames]
                                              : [(MinatoMetalLayer *)layer startRecording:2 * frames];
            if (!started) {
                std::printf("FAIL: the layer did not start recording\n");
                return 1;
            }
            if (record_class) {
                other.device = layer.device;
                other.drawableSize = CGSizeMake(64, 64);
                (void)[other nextDrawable];  // during the recording, on another layer: not in it
            }
        }
        if (minato_bench_start(api.c_str(), conf.c_str(), (__bridge void *)layer, moltenvk.c_str(), csv.c_str(), error,
                               sizeof error) != 0) {
            std::printf("FAIL: minato_bench_start: %s\n", error);
            return 1;
        }
    }

    // Lockstep: the first tick waits for the scene's start, each later one for the frame before it.
    const std::int64_t first = now();
    std::uint64_t ticks = 0;
    std::vector<std::int64_t> posts;
    for (;; ++ticks) {
        posts.push_back(now());
        minato_bench_tick(first + std::llround(static_cast<double>(ticks) * period));
        if (ticks % every != 0) {
            // A tick the capped engine passes over: no frame to wait for.
            std::this_thread::sleep_for(2ms);
            if (minato_bench_wait(0, error, sizeof error) != 1) break;
            continue;
        }
        if (!drawn(ticks / every + 1, ticks == 0 ? 20000ms : 5000ms)) break;
    }
    const int status = ended(10, error, sizeof error);
    std::printf("host: %llu ticks, %llu frames\n", static_cast<unsigned long long>(ticks + 1),
                static_cast<unsigned long long>(minato_bench_frames()));
    if (status != 0) {
        std::printf("FAIL: the run %s%s\n", status == 1 ? "did not end" : "failed: ", status == 1 ? "" : error);
        ++failures;
    }
    if (minato_bench_frames() != frames) {
        std::printf("FAIL: %llu frames, not %llu\n", static_cast<unsigned long long>(minato_bench_frames()),
                    static_cast<unsigned long long>(frames));
        ++failures;
    }
    if (record) {
        @autoreleasepool {
            MinatoLayerRecording *recording = record_class ? [MinatoLayerRecorder stopRecording:layer timeout:2]
                                                           : [(MinatoMetalLayer *)layer stopRecording:2];
            (void)[layer nextDrawable];  // after the recording: not in it
            std::printf("recorder: %s, host build%s: %llu drawables; %s\n", api.c_str(),
                        record_class ? " on a class of its own" : "", static_cast<unsigned long long>(recording.count),
                        recording_notes(recording).c_str());
            if (record_class && [MinatoLayerRecorder stopRecording:other timeout:0] != nil) {
                std::printf("FAIL: recorder: the second layer had a recording\n");
                ++failures;
            }
            std::vector<std::string> problems = recording_problems(recording, posts, frames);
            const qos_class_t expected = at_default ? QOS_CLASS_DEFAULT : QOS_CLASS_USER_INTERACTIVE;
            if (recording != nil && recording.qosClass != expected) {
                problems.push_back("taken at QoS " + std::string(recording.qosName.UTF8String) +
                                   ", not the render thread's " + (at_default ? "default" : "user-interactive"));
            }
            for (const auto &problem : problems) std::printf("FAIL: recorder: %s\n", problem.c_str());
            failures += static_cast<int>(problems.size());
        }
    }
    if (!fs::exists(csv)) {
        std::printf("FAIL: no scene.csv\n");
        ++failures;
    }
    std::printf(failures == 0 ? "PASS: %s %s\n" : "FAIL: %s %s\n", api.c_str(), load.c_str());
    std::fflush(stdout);
    _exit(failures == 0 ? 0 : 1);
}
