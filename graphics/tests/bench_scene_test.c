// The parity benchmark's scene without an API (ADR-0037, decision 1; M6's MetalFX plan, step 0): bench.conf's render
// scale and the offscreen target's size that follows from it.
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

    printf(failures ? "%d failures\n" : "PASS: graphics.bench_scene\n", failures);
    return failures != 0;
}
