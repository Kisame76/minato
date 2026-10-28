// The parity benchmark's scene without an API (ADR-0037, decision 1; M6's MetalFX plan, step 0): bench.conf's render
// scale and the offscreen target's size that follows from it; the engine cap and the FPS unlock (ADR-0066 step 4):
// bench.conf's cap and unlock, the rate an engine draws at for its target, the wrapper's raise and the limiter.
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "scene.h"

void bench_log(const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    vfprintf(stderr, format, arguments);
    va_end(arguments);
}

static int failures;

static void expect(int ok, const char *what) {
    if (ok) return;
    printf("FAIL: %s\n", what);
    ++failures;
}

static int parse(const char *text, struct bench_conf *conf) {
    char error[128];
    bench_conf_defaults(conf);
    return bench_conf_parse(text, conf, error, sizeof error);
}

int main(void) {
    struct bench_conf conf;
    bench_conf_defaults(&conf);
    expect(conf.scale == 100, "the default scale is 100 %");
    expect(parse("run=a-1\nscale=67\n", &conf) == 0 && conf.scale == 67, "scale=67 is taken");
    expect(parse("scale=100\n", &conf) == 0 && conf.scale == 100, "scale=100 is taken");
    expect(parse("scale=25\n", &conf) == 0 && conf.scale == 25, "scale=25, the least, is taken");
    static const char *const refused[] = {"scale=24\n",  "scale=101\n", "scale=0\n",
                                          "scale=abc\n", "scale=\n",    "scale=-5\n"};
    for (size_t index = 0; index < sizeof refused / sizeof refused[0]; ++index) {
        char what[64];
        snprintf(what, sizeof what, "%.*s is refused", (int)strcspn(refused[index], "\n"), refused[index]);
        expect(parse(refused[index], &conf) == -1, what);
    }

    // The fill-heavy load (M6's MetalFX plan, step 0): full-screen alpha layers and no meshes; the others as before.
    expect(parse("load=fill\n", &conf) == 0 && conf.load == BENCH_FILL, "load=fill is taken");
    expect(parse("load=heavy\n", &conf) == 0 && conf.load == BENCH_HEAVY, "load=heavy is still taken");
    expect(parse("load=filled\n", &conf) == -1, "load=filled is refused");
    expect(strcmp(bench_load_name(BENCH_FILL), "fill") == 0, "the fill load's name");
    expect(bench_meshes(BENCH_GLES, BENCH_FILL) == 0 && bench_meshes(BENCH_VULKAN, BENCH_FILL) == 0,
           "the fill load has no meshes");
    expect(bench_fill_layers(BENCH_FILL) > 0, "the fill load has layers");
    expect(bench_fill_layers(BENCH_LIGHT) == 0 && bench_fill_layers(BENCH_HEAVY) == 0, "light and heavy have none");
    expect(bench_meshes(BENCH_GLES, BENCH_LIGHT) == 2048 && bench_meshes(BENCH_GLES, BENCH_HEAVY) == 8192 &&
               bench_meshes(BENCH_VULKAN, BENCH_LIGHT) == 2048 && bench_meshes(BENCH_VULKAN, BENCH_HEAVY) == 8192,
           "the meshes of light and heavy are unchanged");

    int width = 0, height = 0;
    bench_target_size(1290, 2796, 100, &width, &height);
    expect(width == 1290 && height == 2796, "100 % is the window's size");
    bench_target_size(1290, 2796, 50, &width, &height);
    expect(width == 645 && height == 1398, "50 % halves each side");
    bench_target_size(1290, 2796, 67, &width, &height);
    expect(width == 864 && height == 1873, "67 % rounds to the nearest pixel (864.3, 1873.3)");
    bench_target_size(1290, 2796, 75, &width, &height);
    expect(width == 968 && height == 2097, "75 % rounds to the nearest pixel (967.5 up, 2097)");
    bench_target_size(3, 3, 25, &width, &height);
    expect(width == 1 && height == 1, "a side is never 0");
    bench_target_size(256, 256, 100, &width, &height);
    expect(width == 256 && height == 256, "a small window at 100 %");

    // ADR-0066 step 4: without cap and unlock the scene draws every tick, as before.
    bench_conf_defaults(&conf);
    expect(conf.cap == 0 && conf.unlock == 0, "no cap and no unlock by default");
    expect(parse("cap=-1\n", &conf) == 0 && conf.cap == -1, "cap=-1, Unity's default, is taken");
    expect(parse("cap=30\nunlock=120\n", &conf) == 0 && conf.cap == 30 && conf.unlock == 120, "cap=30, unlock=120");
    expect(parse("cap=1000\nunlock=60\n", &conf) == 0 && conf.cap == 1000 && conf.unlock == 60, "cap=1000, unlock=60");
    static const char *const refused_cap[] = {"cap=0\n", "cap=-2\n", "cap=1001\n", "cap=x\n", "cap=\n",
                                              "unlock=90\n", "unlock=0\n", "unlock=-1\n", "unlock=\n"};
    for (size_t index = 0; index < sizeof refused_cap / sizeof refused_cap[0]; ++index) {
        char what[64];
        snprintf(what, sizeof what, "%.*s is refused", (int)strcspn(refused_cap[index], "\n"), refused_cap[index]);
        expect(parse(refused_cap[index], &conf) == -1, what);
    }

    // Unity on Android and iOS: -1 (or any value up to 0) draws at 30 frames per second, a positive value at itself.
    expect(bench_cap_rate(-1) == 30 && bench_cap_rate(0) == 30, "-1 and 0 draw at 30");
    expect(bench_cap_rate(60) == 60 && bench_cap_rate(240) == 240, "a set rate draws at itself");
    // The loader's wrapper (runtime/android/frame_rate.c): -1 or a rate below the unlock becomes the unlock.
    expect(bench_cap_raise(30, 0) == 30 && bench_cap_raise(-1, 0) == -1, "no unlock keeps the cap");
    expect(bench_cap_raise(30, 120) == 120 && bench_cap_raise(-1, 60) == 60, "the unlock raises -1 and a lower cap");
    expect(bench_cap_raise(240, 120) == 240 && bench_cap_raise(60, 60) == 60, "a cap at or above the unlock stays");

    // The limiter on ticks `period` apart: a tick is due once the rate's interval has passed, within half a period.
    const int64_t at120 = 8333333, at60 = 16666667;
    expect(!bench_cap_due(3 * at120, at120, 30) && bench_cap_due(4 * at120, at120, 30), "30 on 120 Hz: every 4th tick");
    expect(!bench_cap_due(at120, at120, 60) && bench_cap_due(2 * at120, at120, 60), "60 on 120 Hz: every 2nd tick");
    expect(bench_cap_due(at120, at120, 120), "120 on 120 Hz: every tick");
    expect(bench_cap_due(at60, at60, 120) && bench_cap_due(at60, at60, 60), "120 or 60 on 60 Hz: every tick");
    expect(!bench_cap_due(at60, at60, 30) && bench_cap_due(2 * at60, at60, 30), "30 on 60 Hz: every 2nd tick");
    expect(bench_cap_due(4 * at120 - 2000000, at120, 30) && !bench_cap_due(3 * at120 + 2000000, at120, 30),
           "a tick 2 ms early or late is taken as its own");
    // A rate that divides no display rate: the nearest whole number of periods.
    expect(bench_cap_due(at120, at120, 90), "90 on 120 Hz: every tick");
    expect(!bench_cap_due(2 * at120, at120, 45) && bench_cap_due(3 * at120, at120, 45), "45 on 120 Hz: every 3rd tick");

    printf(failures ? "%d failures\n" : "PASS: graphics.bench_scene\n", failures);
    return failures != 0;
}
